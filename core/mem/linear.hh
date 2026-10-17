/*
 * The linear map: every frame the allocator owns is reachable at a fixed
 * offset from its physical address.
 *
 * This is internal to the memory subsystem
 * A driver should manage its own frames or use mem::map_phys()
 * Anything else wants the heap.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEM_LINEAR_HH
#define MEM_LINEAR_HH

#include <osv/mem/frames.hh>

namespace mem {
namespace frames {

void *to_linear(phys_addr p);
phys_addr from_linear(void *addr);
bool in_linear_map(const void *addr, size_t bytes = 0);

}
}

#endif /* MEM_LINEAR_HH */
