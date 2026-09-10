/*
 * The demand-paged cache (based on uCache[FAST'26]).
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_PAGECACHE_HH
#define OSV_MEM_PAGECACHE_HH

#include <osv/mem/store.hh>
#include <osv/mem/types.hh>

namespace mem {
namespace pagecache {


// One unit of the cache
struct buffer;

// Each buffer can be of arbitrary size (multiple of hardware-supported size).
uint64_t offset(const buffer &b);
size_t size(const buffer &b);

// Where it is in memory, for a policy that wants to look at the contents.
void *data(const buffer &b);

// Replacement policy helpers
void *policy_data(const buffer &b);
bool accessed(const buffer &b);
void clear_accessed(buffer &b);
bool dirty(const buffer &b);

bool prefetched(const buffer &b);
void clear_prefetched(buffer &b);

// Where a policy puts the buffers it has chosen. Fixed capacity, because a
// policy runs when there is no memory to allocate from.
struct buffer_list {
    buffer **at;
    unsigned count;
    unsigned max;

    bool add(buffer *b) { return count < max ? (at[count++] = b, true) : false; }
    bool full() const { return count == max; }
};

// A list of offsets to prefetch
struct offset_list {
    uint64_t *at;
    unsigned count;
    unsigned max;

    bool add(uint64_t off) { return count < max ? (at[count++] = off, true) : false; }
};

constexpr unsigned prefetch_max = 512;

// Customizable policies
struct policy {
    // Bytes the cache keeps beside each buffer for the policy's own use,
    // reached through policy_data(). Zero for a policy that needs none.
    unsigned bytes_per_buffer;

    // Made when the mapping is and destroyed with it, which are the only two
    // moments a policy may allocate. Handed to every other call.
    void *(*create)(uint64_t store_size);
    void (*destroy)(void *state);

    // Describes the range of a buffer (enables per-buffer size)
    void (*fault_extent)(void *state, uint64_t offset, uint64_t *start,
                         uint64_t *len);

    // Read these in the same batch as the buffer that faulted.
    void (*prefetch)(void *state, buffer &b, offset_list &also);

    unsigned prefetch_depth;

    // Select at least "bytes" worth of candidates for eviction.
    void (*evict)(void *state, size_t bytes, buffer_list &victims);

    void (*on_fault)(void *state, buffer &b);
    void (*on_evicted)(void *state, buffer &b);

    // Whether it has to be written back before it is taken.
    bool (*is_dirty)(void *state, buffer &b);
};

const policy &defaults();

// Create a memory-mapping to the size of the store. "limit" caps how much
// memory it may hold; 0 lets it grow until the frame allocator says stop.
void *map(store &s, const policy &p = defaults(), size_t limit = 0);

// Unmaps (with write-back).
void unmap(void *addr);

// Write back every dirty buffer in the range.
int64_t sync(void *addr, size_t bytes);

// Check if the buffer containing this address is mapped.
bool resident(void *addr);

// Ensures the buffers overlapping with the range are mapped.
size_t fetch(void *addr, size_t bytes);

// Statistics
void stats_dump();
void stats_reset();

}
}

#endif /* OSV_MEM_PAGECACHE_HH */
