/*
 * Handle mappings not managed by the kernel.
 * Mostly (only?) for the drivers
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/phys.hh>
#include <osv/mem/vspace.hh>

#include <algorithm>
#include <cassert>

#include "linear.hh"

namespace mem {

void *map_phys(frames::phys_addr pa, size_t bytes, mattr ma)
{
    auto start = align_down(pa, mapping::page_size);
    auto end = align_up(pa + bytes, mapping::page_size);
    auto virt = reinterpret_cast<uintptr_t>(frames::to_linear(start));

    if (!mapping::attach_missing({virt, virt + (end - start)}, start, perm_rw,
                                 mapping::huge_page_size, ma)) {
        abort("map_phys: %p is already mapped to other physical memory\n",
              reinterpret_cast<void *>(pa));
    }
    return frames::to_linear(pa);
}

void map_phys_at(void *virt_addr, frames::phys_addr pa, size_t bytes, size_t slop,
                 mattr ma)
{
    auto virt = reinterpret_cast<uintptr_t>(virt_addr);
    // Rounding outward is only safe as far as the two addresses run together,
    // and never further than the largest leaf.
    slop = std::min(slop, mapping::huge_page_size);
    assert((virt & (slop - 1)) == (pa & (slop - 1)));
    // Firmware ranges share leaves, and a driver may later map a table inside
    // one, so overlapping is normal and only a disagreement is an error.
    if (!mapping::attach_missing({virt, virt + bytes}, pa, perm_rwx, slop, ma)) {
        abort("map_phys_at: %p is already mapped to other physical memory\n", virt_addr);
    }

    // Hold the addresses so nothing else is handed them. No ops: it is mapped
    // for good, and a fault in it is a bug rather than work to do.
    auto *r = new vspace::region();
    r->perm = perm_rwx;
    if (vspace::reserve_at(*r, {virt, virt + bytes}) != vspace::resa_result::success) {
        delete r;
    }
}

}
