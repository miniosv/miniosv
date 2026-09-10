/*
 * The heap allocator.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_HEAP_HH
#define OSV_MEM_HEAP_HH

#include <osv/mem/types.hh>

namespace mem {
namespace heap {

// Reserve the window and the page descriptors.
void init();
bool ready();

// Check if the heap can satisfy an allocation of "bytes" with at least "alignment".
bool takes(size_t bytes, size_t alignment);

// Take an object of "bytes", aligned to at least "alignment".
void *alloc(size_t bytes, size_t alignment);

// Give back an object alloc() returned.
void free(void *p);

// The same from a caller that knows the size, which C++ supplies at every
// delete of a known type.
void free(void *p, size_t bytes);

// The size class of an object.
size_t size_of(void *p);

// True if this address is one alloc() handed out.
bool owns(void *p);

}
}

#endif /* OSV_MEM_HEAP_HH */
