/*
 * The frame allocator: a façade over llfree.
 */

#include <atomic>

#include <osv/align.hh>
#include <cassert>

#include <osv/debug.hh>
#include <osv/mem/frames.hh>
#include <osv/sched.hh>

#include "internal.hh"
#include "../linear.hh"
#include <osv/mem/mapping.hh>
#include <osv/mem/phys.hh>

extern void *elf_start;
extern size_t elf_size;
extern "C" u64 kernel_vm_shift;

namespace mem {
namespace frames {

// Total usable RAM the firmware reported, which is more than the allocator
// ever holds: the kernel image's own pages are never handed to it.
size_t phys_mem_size;

static_assert(page_size == mem::mapping::page_size, "frames::page_size disagrees with mmu");

namespace {

llfree_t *llf;
uintptr_t base_linear;     // frame 0
size_t frame_count;
size_t total;

/*
* llfree starts with every frame marked allocated and boot.cc then gives back
* whatever it never handed out to others subsystems during boot.
*/
void put_run(uintptr_t start, uintptr_t end)
{
    for_each_block(frame_of(start), frame_of(end), block_max,
                   [](uint64_t frame, unsigned order) {
        // Everything starts allocated and is given back exactly once, so this
        // cannot legitimately fail.
        (void)llfree_put(llf, 0, frame, llflags(order));
    });
}

} // namespace

llfree_t *allocator()
{
    return llf;
}

uint64_t frame_of(uintptr_t linear)
{
    return (linear - base_linear) >> mem::mapping::page_size_shift;
}

uintptr_t linear_of(uint64_t frame)
{
    return base_linear + (frame << mem::mapping::page_size_shift);
}

phys_addr phys_of(uint64_t frame)
{
    return from_linear(reinterpret_cast<void *>(linear_of(frame)));
}

uint64_t frame_of_phys(phys_addr p)
{
    return frame_of(reinterpret_cast<uintptr_t>(to_linear(p)));
}

size_t total_frames()
{
    return frame_count;
}

// Until the scheduler runs on every cpu there is no current cpu to ask, and
// llfree indexes its per-core state with whatever it is given.
std::atomic<bool> percpu_ready;

size_t current_core()
{
    if (!percpu_ready.load(std::memory_order_relaxed)) {
        return 0;
    }
    sched::cpu *c = sched::cpu::current();
    if (!c) {
        return 0;
    }
    size_t cores = llfree_cores(llf);
    return c->id < cores ? c->id : c->id % cores;
}

void enable_percpu()
{
    percpu_ready.store(true, std::memory_order_relaxed);
}

bool ready()
{
    return llf != nullptr;
}

void init(size_t cores)
{
    if (llf) {
        return;
    }

    uintptr_t lowest, highest;
    boot_bounds(lowest, highest);
    if (lowest >= highest) {
        return;
    }

    // llfree wants its region aligned to the largest block it serves.
    base_linear = align_down(lowest, static_cast<uintptr_t>(LLFREE_ALIGN));
    frame_count = (highest - base_linear) >> mem::mapping::page_size_shift;

    llfree_meta_size_t sizes = llfree_metadata_size(cores, frame_count);
    llfree_meta_t meta = {
        .local = static_cast<uint8_t *>(boot_alloc(sizes.local, LLFREE_CACHE_SIZE)),
        .trees = static_cast<uint8_t *>(boot_alloc(sizes.trees, LLFREE_CACHE_SIZE)),
        .lower = static_cast<uint8_t *>(boot_alloc(sizes.lower, LLFREE_CACHE_SIZE)),
    };
    llfree_t *self = static_cast<llfree_t *>(boot_alloc(sizes.llfree, LLFREE_CACHE_SIZE));
    if (!self || !meta.local || !meta.trees || !meta.lower) {
        abort("frames: no memory for the frame allocator's own metadata\n");
    }

    llfree_result_t r = llfree_init(self, cores, frame_count, LLFREE_INIT_ALLOC, meta);
    if (!llfree_is_ok(r)) {
        abort("frames: llfree_init failed\n");
    }
    llf = self;

    boot_for_each_free([](uintptr_t start, uintptr_t end) {
        put_run(start, end);
        total += end - start;
    });

    // alloc() reports failure as physical address 0, so make sure no frame can
    // ever carry that address.
    // For now, this never happened but guard just in case.
    if (phys_of(0) == no_memory) {
        llfree_result_t claim = llfree_get_at(llf, 0, 0, llflags(0));
        if (llfree_is_ok(claim)) {
            total -= page_size;
        }
    }

    pressure_init(total);
}

static phys_addr try_alloc(size_t need, size_t align)
{
    // One block, which llfree hands out aligned to its own size. The common
    // case, and the only one that does not search.
    unsigned order = order_of(need);
    if (order <= block_max && (page_size << order) >= align) {
        llfree_result_t r = llfree_get(llf, current_core(), llflags(order));
        return llfree_is_ok(r) ? phys_of(r.frame) : no_memory;
    }

    // Try to claim multiple contiguous blocks.
    uint64_t frame = claim_run(need, align);
    return frame == no_frame ? no_memory : phys_of(frame);
}

phys_addr alloc(size_t bytes, size_t align)
{
    if (!bytes) {
        return no_memory;
    }
    if (align < page_size) {
        align = page_size;
    }
    size_t need = frames_for(bytes);

    if (!llf) {
        void *p = boot_alloc(need << mem::mapping::page_size_shift, align);
        return p ? from_linear(p) : no_memory;
    }

    phys_addr p = try_alloc(need, align);
    // Clients hold memory they are willing to give back rather than handing it
    // over the moment they stop using it, so running out is a question to ask
    // them rather than an answer. Keep asking while they keep giving.
    while (p == no_memory && reclaim(need << mem::mapping::page_size_shift)) {
        p = try_alloc(need, align);
    }
    if (p != no_memory) {
        check_pressure();
    }
    return p;
}

void free(phys_addr addr, size_t bytes)
{
    if (!addr || !bytes) {
        return;
    }
    if (!llf) {
        boot_free_page(to_linear(addr));
        return;
    }
    uint64_t first = frame_of_phys(addr);
    release_run(first, first + frames_for(bytes));
}

/*
 * The linear map: every frame is reachable at a fixed offset from its physical
 * address, so translating one into a pointer is arithmetic.
 *
 * The kernel image is the exception. It is loaded wherever the firmware put it
 * and linked to run at a fixed virtual address.
 */
void *to_linear(phys_addr p)
{
    void *addr = reinterpret_cast<void *>(p);
    if (addr >= elf_phys_start &&
        addr < static_cast<char *>(elf_phys_start) + elf_size) {
        return static_cast<char *>(addr) + kernel_vm_shift;
    }
    return mem::linear + p;
}

bool in_linear_map(const void *addr, size_t bytes)
{
    if (addr >= elf_start &&
        static_cast<const char *>(addr) + bytes <= static_cast<char *>(elf_start) + elf_size) {
        return true;
    }
    return addr >= mem::linear;
}

phys_addr from_linear(void *addr)
{
    if (addr >= elf_start &&
        addr < static_cast<char *>(elf_start) + elf_size) {
        return reinterpret_cast<phys_addr>(static_cast<char *>(addr) - kernel_vm_shift);
    }
    // Anything else has to be in the linear map: there is nowhere else a
    // physical address can be recovered from.
    assert(addr >= mem::linear);
    return reinterpret_cast<uintptr_t>(addr) & (mem::linear_size - 1);
}

size_t total_available_bytes()
{
    return llf ? total : boot_total();
}

// From llfree's own counters
size_t free_bytes()
{
    if (!llf) {
        return boot_total();
    }
    return llfree_free_frames(llf) << mem::mapping::page_size_shift;
}

} // namespace frames
} // namespace mem
