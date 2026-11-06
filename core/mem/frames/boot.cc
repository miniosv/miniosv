/*
 * Memory before there is an allocator.
 *
 * The firmware memory map arrives long before llfree can exist: llfree needs
 * its own metadata allocated somewhere, and the core count to size the per-core
 * state. So the ranges are parked here, served by a bump pointer plus a free
 * list threaded through the freed pages themselves, and handed over in
 * frames::init().
 *
 * What was handed out before the handover simply stays allocated: llfree starts
 * with everything marked allocated and only the memory still free here is given
 * back to it.
 */

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/frames.hh>
#include "../linear.hh"
#include <osv/mem/mapping.hh>

namespace mem {
namespace frames {

namespace {

// regions handed by the hardware memory map.
constexpr unsigned max_regions = 512;

struct region {
    uintptr_t base;    // linear address of the first usable byte
    uintptr_t end;
    uintptr_t cursor;  // [base, cursor) handed out, [cursor, end) untouched
};

region regions[max_regions];
unsigned region_count;

// Freed pages, linked through their own first word.
void *free_list;
size_t free_list_count;

size_t total;

} // namespace

void *elf_phys_start;

void add_region(phys_addr base, size_t bytes)
{
    if (!base) {
        // Nothing may be handed out at physical zero: that is how alloc() says
        // it has nothing.
        ++base;
        --bytes;
    }
    auto linear = reinterpret_cast<uintptr_t>(to_linear(base));
    uintptr_t b = align_up(linear, mem::mapping::page_size);
    uintptr_t e = align_down(linear + bytes, mem::mapping::page_size);
    if (e <= b) {
        return;
    }
    if (region_count == max_regions) {
        abort("frames: the firmware memory map has more than %u usable ranges, "
              "which is all this kernel can record.\n"
              "       Dropped %llu KiB at %p; %zu MiB collected so far.\n"
              "       Raise max_regions in core/mem/frames/boot.cc.\n",
              max_regions, (unsigned long long)((e - b) >> 10),
              reinterpret_cast<void *>(b), total >> 20);
    }
    regions[region_count++] = region{b, e, b};
    total += e - b;
}

void *boot_alloc(size_t bytes, size_t align, size_t offset)
{
    bytes = align_up(bytes, mem::mapping::page_size);
    for (unsigned i = 0; i < region_count; i++) {
        region &r = regions[i];
        // Same contract as frames::alloc(): it is start + offset that comes out
        // aligned.
        uintptr_t start = align_up(r.cursor + offset, align) - offset;
        if (start >= r.cursor && start + bytes <= r.end) {
            r.cursor = start + bytes;
            return reinterpret_cast<void *>(start);
        }
    }
    return nullptr;
}

void *boot_alloc_page()
{
    if (free_list) {
        void *p = free_list;
        free_list = *static_cast<void **>(p);
        free_list_count--;
        return p;
    }
    return boot_alloc(mem::mapping::page_size, mem::mapping::page_size);
}

void boot_free_page(void *addr)
{
    *static_cast<void **>(addr) = free_list;
    free_list = addr;
    free_list_count++;
}

size_t boot_total()
{
    return total;
}

void boot_for_each_free(void (*fn)(uintptr_t start, uintptr_t end))
{
    while (free_list) {
        void *p = free_list;
        free_list = *static_cast<void **>(p);
        free_list_count--;
        uintptr_t a = reinterpret_cast<uintptr_t>(p);
        fn(a, a + mem::mapping::page_size);
    }
    for (unsigned i = 0; i < region_count; i++) {
        region &r = regions[i];
        if (r.cursor < r.end) {
            fn(r.cursor, r.end);
            r.cursor = r.end;
        }
    }
}

void boot_bounds(uintptr_t &lowest, uintptr_t &highest)
{
    lowest = ~uintptr_t(0);
    highest = 0;
    for (unsigned i = 0; i < region_count; i++) {
        if (regions[i].base < lowest) {
            lowest = regions[i].base;
        }
        if (regions[i].end > highest) {
            highest = regions[i].end;
        }
    }
}

} // namespace frames
} // namespace mem
