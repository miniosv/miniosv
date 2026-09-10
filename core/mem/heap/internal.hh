/*
 * Shared definitions for the heap implementation.
 * Implementation is inspired by the BareHeap[DaMoN'26] allocator.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEM_HEAP_INTERNAL_HH
#define MEM_HEAP_INTERNAL_HH

#include <atomic>
#include <cstdint>

#include <osv/mem/mapping.hh>

namespace mem {
namespace heap {

// Objects up to 1MiB are carved in size classes (from pages).
constexpr size_t alloc_max = 1ul << 20;

// Threshold for the per-cpu bump allocator.
// Bigger objects are allocated from a shared bump pointer.
constexpr size_t small_max = 8192;

// The maximum alignment the heap can satisfy.
constexpr size_t align_max = 2048;

// The unit the heap allocates, maps and reclaims (2 MiB huge page).
constexpr size_t page_bytes = mapping::huge_page_size;

// Virtual memory reserved for the heap.
constexpr size_t window_bytes = 512ul << 30; // 512GiB should be a lot
constexpr uint32_t page_count = window_bytes / page_bytes;

constexpr uint32_t no_page = ~0u;

/*
 * Cacheline-aligned descriptor of a page in the window.
 */
struct alignas(64) page_desc {
    std::atomic<uint32_t> live; // how many objects in this page are still held
    uint32_t obj_size;    // the size every object in this page has
    uint32_t next;        // while the slot is on a list: the next one, or no_page
    frames::phys_addr phys;   // the frame behind it, while it has one
    uint64_t epoch;       // while it waits for the tlb to forget it
};

extern uintptr_t window_start;
extern std::atomic<uintptr_t> window_end;
extern page_desc *pages;

// Checks if an address is in the heap's window.
inline bool in_window(const void *p)
{
    auto end = window_end.load(std::memory_order_acquire);
    auto a = reinterpret_cast<uintptr_t>(p);
    return a < end && a >= window_start;
}

inline uint32_t index_of(const void *p)
{
    return (reinterpret_cast<uintptr_t>(p) - window_start) / page_bytes;
}

inline uintptr_t page_start(uint32_t i)
{
    return window_start + size_t(i) * page_bytes;
}

inline range page_range(uint32_t i)
{
    return {page_start(i), page_start(i) + page_bytes};
}

// A 2 MiB page with a frame behind it, or no_page.
uint32_t page_get();

// Give a page back. The caller must be the last one to hold an object in it.
void page_put(uint32_t i);

// Make a page just taken from the window ready to hold objects of "size".
inline void page_init(uint32_t i, size_t size)
{
    pages[i].obj_size = size;
    pages[i].live.store(page_bytes / size, std::memory_order_relaxed);
}

// Allocate an object of "bytes" (aligned) from a page.
void *paged_alloc(size_t bytes, size_t alignment);

// From here up, an allocation is worth a vspace reservation of its own.
constexpr size_t large_min = 2ul << 20;

// Reserve, allocate and map `bytes` (dedicated reservation).
void *large_alloc(size_t bytes, size_t alignment = mapping::page_size);

// Give back what large_alloc returned. The pointer must be one of its results.
void large_free(void *p);

// What large_alloc was asked for, which is less than what it mapped.
size_t large_size(void *p);

// True if this address is one large_alloc handed out.
bool is_large(void *p);

}
}

#endif /* MEM_HEAP_INTERNAL_HH */
