/*
 * The heap allocator behind malloc.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_HEAP_HH
#define OSV_MEM_HEAP_HH

#include <osv/mem/types.hh>

namespace mem {
namespace heap {

// Reserve the heap's window. Until then malloc is served by mem::early.
void init();
bool ready();

// Whether alloc() can serve this alignment: at most 2 MiB.
bool takes(size_t bytes, size_t alignment);

// An object of at least "bytes", aligned to "alignment". nullptr only if it
// cannot fit in the window; running out of memory aborts the kernel.
void *alloc(size_t bytes, size_t alignment);

// Give back an object alloc() returned.
void free(void *p);

// The bytes an object can hold, or 0 for an address outside the heap.
size_t size_of(void *p);

// Whether "p" lies in the heap's window.
bool owns(void *p);

}
}

#endif /* OSV_MEM_HEAP_HH */
