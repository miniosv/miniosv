/*
 * The heap's address space: one reservation, carved into 2 MiB pages.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/vspace.hh>
#include <osv/mutex.h>
#include <osv/spinlock.h>
#include <osv/sched.hh>

#include <algorithm>
#include <cassert>
#include <cstring>

#include "internal.hh"
#include "../linear.hh"

namespace mem {
namespace heap {

uintptr_t window_start;
std::atomic<uintptr_t> window_end;
page_desc *pages;

namespace {

vspace::region window;


/*
 * Tracks pages in the window. A page is in one of four states:
 *
 *   used    Mapped and has objects in it.
 *   ready   empty, and still has its frame.
 *   free    empty, but without a frame.
 *   dirty   empty and without frame, but its address is not safe yet
 */
spinlock slots;
uint32_t free_list = no_page;
uint32_t ready_list = no_page;
uint32_t ready_count;
uint32_t dirty_head = no_page;
uint32_t dirty_tail = no_page;
uint32_t dirty_count;

// dirty page count limit. TODO: measure and tune this value. It is a tradeoff between memory usage and TLB flushes.
constexpr uint32_t dirty_max = 256;

// How many pages one pass of the pressure callback gives back.
constexpr uint32_t release_batch = 16;
frames::pressure_watcher pressure;

uint32_t pop(uint32_t &list)
{
    uint32_t i = list;
    if (i != no_page) {
        list = pages[i].next;
    }
    return i;
}

void push(uint32_t &list, uint32_t i)
{
    pages[i].next = list;
    list = i;
}

void dirty_push(uint32_t i)
{
    pages[i].next = no_page;
    if (dirty_tail == no_page) {
        dirty_head = i;
    } else {
        pages[dirty_tail].next = i;
    }
    dirty_tail = i;
    dirty_count++;
}

// Skip slots no TLB can be holding any more.
void harvest()
{
    uint64_t now = mapping::flush_epoch();
    while (dirty_head != no_page && now >= pages[dirty_head].epoch + 2) {
        uint32_t i = pop(dirty_head);
        if (dirty_head == no_page) {
            dirty_tail = no_page;
        }
        dirty_count--;
        push(free_list, i);
    }
}

// Force the TLB to forget all the addresses in the dirty list.
void force()
{
    uint32_t head, tail;
    WITH_LOCK(slots) {
        head = dirty_head;
        tail = dirty_tail;
        dirty_head = dirty_tail = no_page;
        dirty_count = 0;
    }
    if (head == no_page) {
        return;
    }
    mapping::flush_all();
    WITH_LOCK(slots) {
        pages[tail].next = free_list;
        free_list = head;
    }
}

/*
 * Unmap a page and give its frame back, leaving the address to the quarantine.
 *
 * The frame can go at once. The page emptied before it got here, so no live
 * pointer names an address in it, and an entry a TLB has not noticed is gone
 * is only dangerous to something that follows it. The address is what waits.
 */
void release(uint32_t i)
{
    mapping::pending_invalidation stale;
    mapping::detach_deferred(page_range(i), stale);
    frames::free(pages[i].phys, page_bytes);

    WITH_LOCK(slots) {
        pages[i].epoch = stale.epoch;
        dirty_push(i);
    }
}


// Give back up to "count" of the pages being held, and say whether any were.
bool release_some(uint32_t count)
{
    uint32_t n = 0;
    while (n < count) {
        uint32_t i;
        WITH_LOCK(slots) {
            i = pop(ready_list);
            if (i == no_page) {
                break;
            }
            ready_count--;
        }
        release(i);
        n++;
    }
    return n != 0;
}

bool under_pressure()
{
    return release_some(release_batch);
}

}

void init()
{
    if (vspace::reserve(window, window_bytes, page_bytes) != vspace::resa_result::success) {
        abort("heap: no room for a %zu GiB window\n", window_bytes >> 30);
    }
    window.perm = perm_rw;

    // Init the page table region that will hold the descriptors.
    size_t bytes = sizeof(page_desc) * page_count;
    frames::phys_addr p = frames::alloc(bytes, page_bytes);
    if (p == frames::no_memory) {
        abort("heap: no room for %zu KiB of page descriptors\n", bytes >> 10);
    }
    pages = static_cast<page_desc *>(frames::to_linear(p));
    memset(pages, 0, bytes);

    // Initialize all pages to free
    for (uint32_t i = page_count; i-- > 0; ) {
        push(free_list, i);
    }

    frames::watch_pressure(pressure, under_pressure, frames::pressure_heap);

    window_start = window.span.start;
    window_end.store(window.span.end, std::memory_order_release);
}

bool ready()
{
    return window_end.load(std::memory_order_acquire) != 0;
}

uint32_t page_get()
{
    uint32_t i;
    bool quarantined = false;
    WITH_LOCK(slots) {
        i = pop(ready_list);
        if (i != no_page) {
            ready_count--;
            return i;
        }
        harvest();
        i = pop(free_list);
        quarantined = i == no_page && dirty_head != no_page;
    }
    if (quarantined) { // we go here only if harvest() did not free enough pages.
        force();
        WITH_LOCK(slots) {
            i = pop(free_list);
        }
    }
    if (i == no_page) {
        return no_page;
    }

    frames::phys_addr p = frames::alloc(page_bytes, page_bytes);
    if (p == frames::no_memory) {
        WITH_LOCK(slots) {
            push(free_list, i);
        }
        return no_page;
    }
    if (!mapping::attach(page_range(i), p, perm_rw, mattr::normal)) {
        // The slot was ours and nothing else can be mapping it, so this is a
        // bug in the heap rather than a condition to recover from.
        abort("heap: page %u is already mapped\n", i);
    }
    pages[i].phys = p;
    return i;
}

void page_put(uint32_t i)
{
    bool crowded;
    WITH_LOCK(slots) {
        ready_count++;
        push(ready_list, i);
        crowded = dirty_count >= dirty_max;
    }
    if (crowded) { // flushes the dirty pages to make space
        force();
    }
}

// A reservation is aligned to the huge page it starts on, and nothing here
// can do better than that.
bool takes(size_t bytes, size_t alignment)
{
    return alignment <= large_min;
}

void *alloc(size_t bytes, size_t alignment)
{
    assert(takes(bytes, alignment));
    if (bytes <= alloc_max &&
        (alignment <= align_max || std::max(bytes, alignment) <= small_max)) {
        return paged_alloc(bytes, alignment);
    }
    return large_alloc(bytes, alignment);
}

void free(void *p)
{
    if (!in_window(p)) {
        return large_free(p);
    }
    uint32_t i = index_of(p);
    if (pages[i].live.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        page_put(i);
    }
}

// This heap manager does not need the size
void free(void *p, size_t)
{
    free(p);
}

size_t size_of(void *p)
{
    return in_window(p) ? pages[index_of(p)].obj_size : large_size(p);
}

bool owns(void *p)
{
    return in_window(p) || is_large(p);
}

}
}
