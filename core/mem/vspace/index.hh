/*
 * Interface between the vspace manager and the index of reserved regions.
 * An index should be compatible with this interface to be usable by the vspace manager.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEM_VSPACE_INDEX_HH
#define MEM_VSPACE_INDEX_HH

#include <osv/mem/vspace.hh>

namespace mem {
namespace vspace {
namespace index {

/*
* Region lifetime is the callers responsibility.
* The index does not own the region objects, it only points to them.
*/

// Create a region of size "bytes" inside the "within" range, aligned to "align".
// Sets r.span. False if there is no room.
bool insert(region &r, size_t bytes, size_t align, range within);

// Insert a region over exactly this range. False if any of it is taken.
bool insert_at(region &r, range at);

// Remove a region from the index.
void remove(region &r);

// Looks up the index for a region containing "addr".
// Caller has no guarantee that the region will remain valid after this call.
region *find(uintptr_t addr);

// Check if the given range is fully contained in reserved regions.
bool covered(range r);

// Return the total number of bytes reserved in the index.
size_t reserved_bytes();

// Return the total number of regions in the index.
size_t count();

// Iterate over all regions in the index, calling `fn` for each one.
void for_each(void (*fn)(const region &, void *), void *arg);

// Check the internal consistency of the index. Returns true if consistent.
bool self_check();

} // index
} // vspace
} // mem

#endif
