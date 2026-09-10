/*
 * Physically contiguous allocation of any size.
 *
 * llfree serves blocks up to 2 MiB. Larger requests are stitched together here
 * by claiming consecutive blocks with llfree_get_at.
 * Unlike a reserved pool this can fail when memory is fragmented. A caller that
 * cannot tolerate that should allocate at boot.
 */

#include <osv/align.hh>
#include <osv/mem/frames.hh>
#include <cassert>
#include <osv/ilog2.hh>

#include "../linear.hh"

#include "internal.hh"
#include <osv/mem/mapping.hh>

namespace mem {
namespace frames {

unsigned order_of(size_t frames)
{
    unsigned order = 0;
    while ((size_t(1) << order) < frames) {
        order++;
    }
    return order;
}

// How many frames an allocation of this size occupies: a whole block when it
// fits in one, whole frames otherwise. alloc() and free() both go through here,
// which is what lets free() work from the size alone.
size_t frames_for(size_t bytes)
{
    size_t need = align_up(bytes, size_t(mem::mapping::page_size)) >> mem::mapping::page_size_shift;
    unsigned order = order_of(need);
    return order <= block_max ? (size_t(1) << order) : need;
}

namespace {

void release(uint64_t first, uint64_t last)
{
    for_each_block(first, last, block_max, [](uint64_t frame, unsigned order) {
        (void)llfree_put(allocator(), current_core(), frame, llflags(order));
    });
}

// Try claiming [first, last) from llfree. If any block fails, release everything claimed so far.
bool claim(uint64_t first, uint64_t last)
{
    uint64_t done = first;
    bool ok = true;
    for_each_block(first, last, block_max, [&](uint64_t frame, unsigned order) {
        if (!ok) {
            return;
        }
        llfree_result_t r = llfree_get_at(allocator(), current_core(), frame, llflags(order));
        if (llfree_is_ok(r)) {
            done = frame + (uint64_t(1) << order);
        } else {
            ok = false;
        }
    });
    if (!ok && done > first) {
        release(first, done);
    }
    return ok;
}

} // namespace

void release_run(uint64_t first, uint64_t last)
{
    release(first, last);
}

uint64_t claim_run(size_t need, size_t align)
{
    // Frame 0 is never handed out, so a physical address of 0 can mean "no
    // memory"; start the search above it.
    uint64_t step = align >> mem::mapping::page_size_shift;
    uint64_t first = step;
    if (need >= (size_t(1) << block_max) && step < (uint64_t(1) << block_max)) {
        step = uint64_t(1) << block_max;
        first = step;
    }
    uint64_t last = total_frames();
    for (uint64_t frame = first; frame + need <= last; frame += step) {
        bool plausible = true;
        for_each_block(frame, frame + need, block_max, [&](uint64_t f, unsigned o) {
            // Probe llfree to see if this block is free. If not, skip to the next candidate run.
            unsigned ask = o >= LLFREE_HUGE_ORDER ? unsigned(LLFREE_HUGE_ORDER) : 0u;
            if (plausible && !llfree_is_free(allocator(), f, ask)) {
                plausible = false;
            }
        });
        if (!plausible) {
            continue;
        }
        if (claim(frame, frame + need)) {
            return frame;
        }
    }
    return no_frame;
}

} // namespace frames
} // namespace mem
