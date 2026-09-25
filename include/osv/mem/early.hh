/*
 * The allocator that serves malloc before the heap exists.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_EARLY_HH
#define OSV_MEM_EARLY_HH

#include <osv/mem/types.hh>

namespace mem {
namespace early {

// Check if the early allocator can satisfy an allocation of "bytes" with at
// least "alignment". It bumps through one page at a time and puts the size in
// front of every object, so anything near a page is out.
bool takes(size_t bytes, size_t alignment);

// Take an object of "bytes", aligned to at least "alignment".
void *alloc(size_t bytes, size_t alignment);

// Give back an object alloc() returned. Rarely returns the page: only when
// the last object in it goes, or when it was the last one handed out.
void free(void *p);

// What alloc() was asked for.
size_t size_of(void *p);

// True if this address is one alloc() handed out.
bool owns(void *p);

}
}

#endif /* OSV_MEM_EARLY_HH */
