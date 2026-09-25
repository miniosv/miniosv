/*
 * Objects allocated before the heap exists.
 *
 * A bump pointer through one page at a time, with the size of every object in
 * the two bytes before it and a count of them in the page header. A page goes
 * back when its count reaches zero, and the last object allocated can be
 * un-bumped, which is enough to keep boot from walking through memory: nothing
 * else here reuses anything.
 *
 * It has to work before there are threads, before there is a scheduler, and
 * before the frame allocator has taken over from the boot regions, which is
 * why it is this and not the heap.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <cassert>
#include <cstdint>

#include <algorithm>

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/early.hh>
#include <osv/mem/frames.hh>
#include <osv/mutex.h>
#include "linear.hh"

namespace mem {
namespace early {

namespace {

constexpr size_t page_size = frames::page_size;

struct page_header {
    page_header *next;
    unsigned short allocations_count;
};

mutex lock;

char *page = nullptr;
size_t next_offset = 0;
size_t previous_offset = 0;

// Pages that this allocator holds, most recent first.
page_header *held = nullptr;

page_header *header_of(void *object)
{
    return reinterpret_cast<page_header *>(
            reinterpret_cast<std::uintptr_t>(object) & ~(page_size - 1));
}

unsigned short *size_field(void *object)
{
    return reinterpret_cast<unsigned short *>(
            static_cast<char *>(object) - sizeof(unsigned short));
}

// Big allocation
constexpr unsigned short big_mark = 0xffff;

struct big_header {
    size_t total;
    uint32_t offset;            // from the start of the run to the object
    unsigned short pad;
    unsigned short mark;        // sits in the two bytes before the object
};

constexpr size_t big_offset = sizeof(big_header);

void *big_alloc(size_t bytes, size_t alignment)
{
    size_t offset = align_up(big_offset, alignment);
    size_t total = align_up(bytes + offset, page_size);
    frames::phys_addr p = frames::alloc(total, std::max(alignment, page_size));
    if (p == frames::no_memory) {
        return nullptr;
    }
    char *obj = static_cast<char *>(frames::to_linear(p)) + offset;
    auto *h = reinterpret_cast<big_header *>(obj - sizeof(big_header));
    h->total = total;
    h->offset = offset;
    h->mark = big_mark;
    return obj;
}

bool is_big(void *p)
{
    return *size_field(p) == big_mark;
}

big_header *big_header_of(void *p)
{
    return reinterpret_cast<big_header *>(static_cast<char *>(p) - sizeof(big_header));
}

// Not the boot allocator: by the time this needs a page, llfree may already
// own the memory. frames::alloc() picks whichever is current.
void take_page()
{
    frames::phys_addr p = frames::alloc();
    if (p == frames::no_memory) {
        abort("early: out of memory before the heap exists\n");
    }
    page = static_cast<char *>(frames::to_linear(p));
    auto *h = header_of(page);
    h->allocations_count = 0;
    h->next = held;
    held = h;
    next_offset = sizeof(page_header);
}

void give_back(page_header *h)
{
    page_header **link = &held;
    while (*link && *link != h) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = h->next;
    }
    frames::free(frames::from_linear(h));
}

}

bool takes(size_t bytes, size_t alignment)
{
    auto lowest = align_up(sizeof(page_header) + sizeof(unsigned short), alignment);
    return lowest + bytes <= page_size;
}

void *alloc(size_t bytes, size_t alignment)
{
    if (!takes(bytes, alignment)) {
        return big_alloc(bytes, alignment);
    }
    WITH_LOCK(lock) {
        if (!page) {
            take_page();
        }

        size_t offset = align_up(next_offset + sizeof(unsigned short), alignment);
        if (offset + bytes > page_size) {
            take_page();
            offset = align_up(next_offset + sizeof(unsigned short), alignment);
        }
        assert(offset + bytes <= page_size);

        auto ret = page + offset;
        previous_offset = next_offset;
        next_offset = offset + bytes;

        *size_field(ret) = static_cast<unsigned short>(bytes);
        header_of(page)->allocations_count++;
        return ret;
    }
}

void free(void *p)
{
    if (is_big(p)) {
        auto *b = big_header_of(p);
        frames::free(frames::from_linear(static_cast<char *>(p) - b->offset),
                     b->total);
        return;
    }
    WITH_LOCK(lock) {
        page_header *h = header_of(p);
        unsigned short bytes = *size_field(p);
        if (!bytes) {
            return;
        }
        // Zero says it has been freed, so a second free does nothing.
        *size_field(p) = 0;

        if (--h->allocations_count == 0) {
            give_back(h);
            if (page == reinterpret_cast<char *>(h)) {
                page = nullptr;
            }
        } else if (page == reinterpret_cast<char *>(h) &&
                   page + (next_offset - bytes) == p) {
            // The object allocated last, freed straight after: put the bump
            // pointer back where it was. This is what makes a malloc/free pair
            // during boot cost nothing.
            next_offset = previous_offset;
        }
    }
}

size_t size_of(void *p)
{
    if (is_big(p)) {
        auto *b = big_header_of(p);
        return b->total - b->offset;
    }
    return *size_field(p);
}

bool owns(void *p)
{
    if (is_big(p)) {
        return true;
    }
    page_header *h = header_of(p);
    WITH_LOCK(lock) {
        for (page_header *k = held; k; k = k->next) {
            if (k == h) {
                return true;
            }
        }
    }
    return false;
}

}
}
