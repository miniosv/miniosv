/*
 * Virtual address space manager
 * This layer only keeps track of virtual memory region reservations.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_VSPACE_HH
#define OSV_MEM_VSPACE_HH

#include <osv/mem/types.hh>

namespace mem {
namespace vspace {

struct region;

struct region_ops {
    bool (*fault)(region &r, uintptr_t addr, unsigned error);
};

struct region {
    range span;
    unsigned perm; // for reference, not enforced by this layer
    const region_ops *ops = nullptr;
};

// Range of addresses available for reservations.
range app_window();

enum class resa_result {
    success,            // Reservation succeeded
    no_space,       // No space available for the requested range
    already_reserved,    // For reserve_at() only, part of the range is already reserved
};

resa_result reserve(region &r, size_t bytes, size_t align);
resa_result reserve_at(region &r, range at);
void release(region &r);

region *lookup(uintptr_t addr); // without locking.

bool reserved(range r);

size_t reserved_bytes();
size_t count();

void for_each(void (*fn)(const region &, void *), void *arg);

// Check the index's invariants. O(n), for the tests.
bool self_check();

}
}

#endif
