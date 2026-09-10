/*
 * Sub-page manager.
 * Each page corresponds to a size class and contains per-cpu bump pointers
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <algorithm>
#include <cassert>

#include <osv/ilog2.hh>
#include <osv/mem/heap.hh>
#include <osv/percpu.hh>
#include <osv/preempt-lock.hh>
#include <osv/sched.hh>

#include "internal.hh"

namespace mem {
namespace heap {

namespace {

// Size classes. Powers of two up to small_max, then four steps per order.
constexpr unsigned min_shift = 4;                                  // 16 bytes
constexpr unsigned small_shift = ilog2_roundup_constexpr(small_max);
constexpr unsigned last_order = ilog2_roundup_constexpr(alloc_max) - 1;

// Doubling classes first, four-per-order ones after them, in one numbering.
constexpr unsigned small_classes = small_shift - min_shift + 1;
constexpr unsigned medium_classes = (last_order - small_shift + 1) * 4;
constexpr unsigned classes = small_classes + medium_classes;

// The class that can hold "bytes": the smallest one that is not smaller.
unsigned class_of(size_t bytes)
{
    if (bytes <= small_max) {
        unsigned s = ilog2_roundup(bytes);
        return s > min_shift ? s - min_shift : 0;
    }

    unsigned e = ilog2(bytes - 1);
    size_t step = size_t(1) << (e - 2);
    size_t size = (bytes + step - 1) & ~(step - 1);
    return small_classes + (e - small_shift) * 4 + unsigned(size >> (e - 2)) - 5;
}

// Get the size of a class given as argument
constexpr size_t class_size(unsigned cls)
{
    if (cls < small_classes) {
        return size_t(1) << (cls + min_shift);
    }
    // Which order the class falls in, and which quarter of it.
    unsigned i = cls - small_classes;
    unsigned e = small_shift + i / 4;
    return size_t(5 + i % 4) << (e - 2);
}

static_assert(class_size(small_classes - 1) == small_max, "the halves must meet");
static_assert(class_size(small_classes) > small_max, "and not overlap");
static_assert(class_size(classes - 1) == alloc_max, "the last class is the cap");

// Where to find objects of a class.
struct cursor {
    uintptr_t next;
    uintptr_t base;
};

// One cursor per class, and one set of them per cpu
struct cursors {
    cursor c[classes];
};

PERCPU(cursors, bump);

// Put a page under this cpu's cursor for "cls" and cut the first object from it.
//Off the fast path -- once per 2 MiB per class per cpu.
void *refill(unsigned cls, size_t size)
{
    // Outside the preempt_lock below: taking a page maps it, which can wait.
    uint32_t fresh = page_get();
    if (fresh == no_page) {
        return nullptr;
    }
    page_init(fresh, size);

    uint32_t spare = no_page;
    void *obj;
    WITH_LOCK(preempt_lock) {
        auto &c = bump->c[cls];
        if (c.next - c.base >= size) {
            // Another thread on this cpu refilled while this one was mapping.
            spare = fresh;
        } else {
            c.base = page_start(fresh);
            c.next = c.base + page_bytes;
        }
        // The cursor now has room, so cut from whichever page it is pointing at.
        c.next -= size;
        obj = reinterpret_cast<void *>(c.next);
    }
    if (spare != no_page) {
        page_put(spare);
    }
    return obj;
}

}

// Entry point. Allocates an object from the corresponding page.
void *paged_alloc(size_t bytes, size_t alignment)
{
    assert(bytes <= alloc_max && alignment <= small_max);
    unsigned cls = class_of(std::max(bytes, std::min(alignment, small_max)));
    size_t size = class_size(cls);

    WITH_LOCK(preempt_lock) { // fast path
        auto &c = bump->c[cls];
        if (c.next - c.base >= size) { // there is an object to return
            c.next -= size;
            return reinterpret_cast<void *>(c.next);
        }
    } // if fast path fails then refill the bump allocator
    return refill(cls, size);
}

}
}
