#ifndef OSV_MEM_FRAMES_INTERNAL_HH
#define OSV_MEM_FRAMES_INTERNAL_HH

#include <cstddef>
#include <cstdint>

extern "C" {
#include "llfree_platform.h"
#include "llfree.h"
}

namespace mem {
namespace frames {

// The largest block, as an order.
constexpr unsigned block_max = 9;   // 2 MiB

// Walks [first, last) as the largest aligned blocks that fit; calling fn(frame, order).
// Used to give a physical region to llfree and to take one back.
template <typename F>
void for_each_block(uint64_t first, uint64_t last, unsigned max, F fn)
{
    uint64_t frame = first;
    while (frame < last) {
        unsigned order = 0;
        while (order + 1 <= max) {
            uint64_t size = uint64_t(1) << (order + 1);
            if ((frame & (size - 1)) != 0 || frame + size > last) {
                break;
            }
            order++;
        }
        fn(frame, order);
        frame += uint64_t(1) << order;
    }
}

// boot.cc
size_t boot_total();
void boot_for_each_free(void (*fn)(uintptr_t start, uintptr_t end));
void boot_bounds(uintptr_t &lowest, uintptr_t &highest);

// contiguous.cc -- runs of more than one block, and the rounding both alloc()
// and free() use to agree on how a run splits.
constexpr uint64_t no_frame = ~uint64_t(0);

size_t frames_for(size_t bytes);
unsigned order_of(size_t frames);
uint64_t claim_run(size_t need, size_t align);
void release_run(uint64_t first, uint64_t last);

// frames.cc
llfree_t *allocator();
uint64_t frame_of(uintptr_t linear);
uintptr_t linear_of(uint64_t frame);
phys_addr phys_of(uint64_t frame);
uint64_t frame_of_phys(phys_addr p);
size_t total_frames();
size_t current_core();
void enable_percpu();

// pressure.cc
void pressure_init(size_t total_bytes);

} // namespace frames
} // namespace mem

#endif
