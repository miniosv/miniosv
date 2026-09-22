/*
 * The heap: one window, mapped from its start up to a break and carved into
 * blocks. A header in front of each object holds the size of its block. Free
 * blocks are kept in a list sorted by address, so a freed block merges with
 * the free blocks either side of it. Memory is reused, never given back.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/vspace.hh>
#include <osv/mutex.h>

namespace mem {
namespace heap {

namespace {

constexpr size_t window_bytes = 512ul << 30;
// The break moves up in steps of this.
constexpr size_t step = mapping::huge_page_size;
constexpr size_t align_max = mapping::huge_page_size;

// "USED" and "FREE" in a memory dump.
constexpr uint32_t magic_used = 0x44455355;
constexpr uint32_t magic_free = 0x45455246;

struct header {
    size_t size;        // the whole block, this header included
    uint32_t magic;
    uint32_t unused;
};

// A free block.
struct zone {
    header h;
    zone *next;
    zone *prev;
};

constexpr size_t grain = sizeof(header);
constexpr size_t min_block = sizeof(zone);
static_assert(grain == 16 && min_block % grain == 0, "blocks are multiples of 16 bytes");

vspace::region window;
uintptr_t brk;          // end of the mapped part of the window
zone *first;            // the free blocks, lowest address first
mutex lock;
std::atomic<bool> up;

uintptr_t addr_of(const void *p)
{
    return reinterpret_cast<uintptr_t>(p);
}

uintptr_t end_of(const zone *z)
{
    return addr_of(z) + z->h.size;
}

header *header_of(void *p)
{
    return static_cast<header *>(p) - 1;
}

void unlink(zone *z)
{
    if (z->prev) {
        z->prev->next = z->next;
    } else {
        first = z->next;
    }
    if (z->next) {
        z->next->prev = z->prev;
    }
}

// Make [at, at + size) a free block, merged with the free blocks either side.
void insert(uintptr_t at, size_t size)
{
    zone *prev = nullptr;
    zone *next = first;
    while (next && addr_of(next) < at) {
        prev = next;
        next = next->next;
    }
    auto *z = reinterpret_cast<zone *>(at);
    z->h.size = size;
    z->h.magic = magic_free;
    z->prev = prev;
    z->next = next;
    if (prev) {
        prev->next = z;
    } else {
        first = z;
    }
    if (next) {
        next->prev = z;
    }
    if (next && end_of(z) == addr_of(next)) {
        z->h.size += next->h.size;
        unlink(next);
    }
    if (prev && end_of(prev) == at) {
        prev->h.size += z->h.size;
        unlink(z);
    }
}

// Map at least "bytes" more of the window at the break. False if it is full.
bool grow(size_t bytes)
{
    bytes = align_up(bytes, step);
    if (bytes > window.span.end - brk) {
        return false;
    }
    if (!mapping::populate({brk, brk + bytes}, perm_rw, mapping::page_size, false)) {
        abort("heap: %#lx, past the break, is already mapped\n", brk);
    }
    insert(brk, bytes);
    brk += bytes;
    return true;
}

// Where a block of "size" fits in z with its object aligned, or 0.
uintptr_t fit(const zone *z, size_t size, size_t alignment)
{
    uintptr_t at = align_up(addr_of(z) + grain, alignment) - grain;
    // What is left in front has to be large enough to be a free block.
    if (at != addr_of(z) && at - addr_of(z) < min_block) {
        at = align_up(addr_of(z) + min_block + grain, alignment) - grain;
    }
    return at + size <= end_of(z) ? at : 0;
}

// Take [at, at + size) out of z. What is left either side stays free.
void *carve(zone *z, uintptr_t at, size_t size)
{
    uintptr_t end = end_of(z);
    if (at == addr_of(z)) {
        unlink(z);
    } else {
        z->h.size = at - addr_of(z);
    }
    if (end - (at + size) >= min_block) {
        insert(at + size, end - (at + size));
    } else {
        size = end - at;
    }
    auto *h = reinterpret_cast<header *>(at);
    h->size = size;
    h->magic = magic_used;
    return h + 1;
}

// First fit: the object goes in the lowest free block it fits in.
void *take(size_t size, size_t alignment)
{
    for (zone *z = first; z; z = z->next) {
        if (uintptr_t at = fit(z, size, alignment)) {
            return carve(z, at, size);
        }
    }
    return nullptr;
}

} // namespace

void init()
{
    window.perm = perm_rw;
    if (vspace::reserve(window, window_bytes, step) != vspace::resa_result::success) {
        abort("heap: no room for a %zu GiB window\n", window_bytes >> 30);
    }
    brk = window.span.start;
    up.store(true, std::memory_order_release);
}

bool ready()
{
    return up.load(std::memory_order_acquire);
}

bool takes(size_t, size_t alignment)
{
    return alignment <= align_max;
}

void *alloc(size_t bytes, size_t alignment)
{
    if (bytes > window_bytes) {
        return nullptr;
    }
    alignment = std::max(alignment, grain);
    size_t size = std::max(align_up(bytes, grain) + grain, min_block);
    WITH_LOCK(lock) {
        if (void *p = take(size, alignment)) {
            return p;
        }
        // Enough that the block fits in what is added, whatever its alignment.
        if (!grow(size + alignment + min_block)) {
            return nullptr;
        }
        return take(size, alignment);
    }
}

void free(void *p)
{
    header *h = header_of(p);
    WITH_LOCK(lock) {
        if (addr_of(h) < window.span.start || addr_of(p) >= brk || h->magic != magic_used) {
            abort("heap: free(%p) of something the heap did not hand out\n", p);
        }
        insert(addr_of(h), h->size);
    }
}

size_t size_of(void *p)
{
    return owns(p) ? header_of(p)->size - grain : 0;
}

bool owns(void *p)
{
    uintptr_t a = addr_of(p);
    return ready() && a >= window.span.start && a < window.span.end;
}

}
}
