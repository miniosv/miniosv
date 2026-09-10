/*
 * The address space layer.
 * Uses the index via the interface in index.hh to track reserved regions.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "index.hh"
#include <osv/mem/frames.hh>
#include <osv/align.hh>
#include <algorithm>

namespace mem {
namespace vspace {

static constexpr size_t page_size = frames::page_size;

// Above the kernel image and below the linear map of physical memory.
range app_window()
{
    return {0x200000000000ul, 0x400000000000ul};
}

resa_result reserve(region &r, size_t bytes, size_t align)
{
    bytes = align_up(bytes, page_size);
    align = std::max(align, page_size);
    if (!bytes) {
        return resa_result::no_space;
    }
    return index::insert(r, bytes, align, app_window()) ? resa_result::success
                                                        : resa_result::no_space;
}

resa_result reserve_at(region &r, range at)
{
    at = {align_down(at.start, page_size), align_up(at.end, page_size)};
    if (at.empty()) {
        return resa_result::no_space;
    }
    return index::insert_at(r, at) ? resa_result::success
                                   : resa_result::already_reserved;
}

void release(region &r)
{
    index::remove(r);
}

region *lookup(uintptr_t addr)
{
    return index::find(addr);
}

bool reserved(range r)
{
    return index::covered(r);
}

size_t reserved_bytes()
{
    return index::reserved_bytes();
}

size_t count()
{
    return index::count();
}

void for_each(void (*fn)(const region &, void *), void *arg)
{
    index::for_each(fn, arg);
}

bool self_check()
{
    return index::self_check();
}

}
}
