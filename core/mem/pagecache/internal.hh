/*
 * Shared definitions for the page cache.
 * Inspired by uCache [FAST'26] with arbitrary buffer sizes.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEM_PAGECACHE_INTERNAL_HH
#define MEM_PAGECACHE_INTERNAL_HH

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <osv/align.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/pagecache.hh>
#include <osv/mem/store.hh>
#include <osv/mem/vspace.hh>
#include <osv/mutex.h>

namespace mem {
namespace pagecache {

constexpr size_t page_size = mapping::page_size;
constexpr size_t huge_size = mapping::huge_page_size;
constexpr size_t huge_pages = huge_size / page_size;

// The most one extent can be.
constexpr size_t tile_max = 1ul << 30;

constexpr unsigned sw_right = 0;
constexpr unsigned sw_left = 1;
constexpr unsigned sw_busy = 2;

struct cache;

// One piece of a buffer's footprint: a contiguous frame allocation, mapped as
// one huge leaf or as `pages` small entries.
struct chunk {
    frames::phys_addr phys;     // no_memory until allocated
    io *req;                    // in flight while loading
    uint64_t pid;
    uint32_t pages;
    uint32_t placed;            // entries installed so far
    bool leaf;
    bool edge;                  // single page a neighbour may share
    bool adopted;               // the neighbour made it; no frame of our own
    bool taken;                 // eviction cleared it; the frame is ours
};

// An extent of the object. Reached only through the policy, which is handed it
// at on_fault and gives it back as an eviction victim.
struct buffer {
    cache *owner;
    uint64_t off;
    uint64_t bytes;
    uint32_t chunks;
    bool hooked, hook_wet;      // the policy's is_dirty verdict, taken mapped
    bool prefetched;            // brought in by prefetch(), not yet proven hot

    // How many IO ops are still needed
    std::atomic<uint32_t> ios_left;
    uint32_t slot;
};

// Prefetch claims waiting to be installed.
constexpr unsigned pending_slots = 64;

struct cache {
    vspace::region r;
    store *s;
    const policy *p;
    void *state;
    uint64_t store_bytes;
    size_t policy_bytes;
    size_t limit;               // memory it may hold, or 0 for whatever there is
    std::atomic<size_t> resident_bytes;
    std::atomic<cache *> next;

    std::atomic<uint64_t> pending_off[pending_slots] = {};
    std::atomic<buffer *> pending[pending_slots] = {};
};

inline chunk *chunks_of(buffer *b)
{
    return reinterpret_cast<chunk *>(reinterpret_cast<char *>(b + 1) +
                                     b->owner->policy_bytes);
}

inline uint64_t page_of(uint64_t off) { return off / page_size; }
inline uintptr_t va_of(const cache &c, uint64_t off) { return c.r.span.start + off; }

inline uint64_t first_pid(const buffer *b) { return page_of(b->off); }
inline uint64_t last_pid(const buffer *b) { return page_of(b->off + b->bytes - 1); }

inline range range_of(const buffer *b)
{
    uintptr_t s = va_of(*b->owner, first_pid(b) * page_size);
    return {s, s + (last_pid(b) - first_pid(b) + 1) * page_size};
}

// How much of [from, from + len) the object actually has.
inline size_t stored(const cache &c, uint64_t from, size_t len)
{
    return from >= c.store_bytes ? 0
                                 : std::min<uint64_t>(len, c.store_bytes - from);
}

/* cache.cc */

cache *cache_of(vspace::region *r);

// The extent covering "off": its start, and its length. Must be a pure
// function of the offset, so every fault in a tile computes the same tile.
uint64_t tile_of(cache &c, uint64_t off, uint64_t &start);

enum class load_result {
    started,   // out holds a buffer, and load_finish() owes it a wait
    taken,     // another cpu owns that tile; spin on the faulting entry
    failed,    // there was no memory for it
};

load_result load_start(cache &c, uint64_t off, buffer *&out);
bool load_finish(cache &c, buffer *b);

// Take a parked prefetch claim, or nullptr if there is none to take.
buffer *pending_take(cache &c, uint64_t off, bool any);

bool fault_in(cache &c, uint64_t off);

// Claim a victim: anchor claimed, present bits cleared, stale addresses
// recorded. The frames stay until after the invalidation. False if the buffer
// is held by a writer.
bool evict_take(cache &c, buffer *b, mapping::pending_invalidation &stale);

// After the invalidation, on one cpu: start every write-back, then settle --
// wait for the writes, zero the entries, free the frames, free the buffer.
void evict_write_start(cache &c, buffer *b);
void evict_settle(cache &c, buffer *b);

// The list of caches. The mutex serializes map and unmap; evictors walk the
// list with only walkers held, and unmap waits for them to drain before its
// cache may be freed.
extern mutex caches_mutex;
extern std::atomic<cache *> caches;
extern std::atomic<unsigned> walkers;

/* reclaim.cc -- the manager over every cache. */

size_t evict_bytes(cache &c, size_t bytes);

// Make room for "bytes" about to be taken: this cache's own limit, then a
// proportional pass over every cache when memory is short everywhere.
void make_room(cache &c, size_t bytes);

// Wait until no evictor can still be looking at an unlinked cache.
void quiesce_manager();

void watch_start();

}
}

#endif /* MEM_PAGECACHE_INTERNAL_HH */
