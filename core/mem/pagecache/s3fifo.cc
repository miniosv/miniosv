/*
 * Default eviction policy inspired by S3-FIFO[SOSP'23]
 * It differs slightly by using the accessed bit instead of tracking accesses explicitly
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <atomic>
#include <cstdlib>

#include <osv/debug.hh>
#include <osv/ilog2.hh>

#include "internal.hh"

namespace mem {
namespace pagecache {

namespace {

// The small queue's share: one tenth on probation, the rest proven.
constexpr size_t small_share = 10;

// Buffers one call to evict() looks at before giving up on finding a cold one.
constexpr unsigned steps_max = 4096;

constexpr uint64_t ring_cap_min = 1024;

// Whose turn a slot is: its own index means an enqueue may fill it, one past
// that means a dequeue may take it.
struct slot {
    std::atomic<uint64_t> seq;
    buffer *b;
};

struct fifo {
    slot *at = nullptr;
    uint64_t mask = 0;
    std::atomic<uint64_t> head{0}, tail{0};
    std::atomic<size_t> bytes{0};
};

struct queues {
    fifo small, main;

    // The names of what the small queue gave up: one slot per hash, the
    // newest name winning it. Forgets early, invents nothing.
    std::atomic<uint32_t> *ghost = nullptr;
    size_t ghost_mask = 0;
};

bool fifo_make(fifo &q, uint64_t cap)
{
    q.at = static_cast<slot *>(std::malloc(cap * sizeof(slot)));
    if (!q.at) {
        return false;
    }
    for (uint64_t i = 0; i < cap; i++) {
        q.at[i].seq.store(i, std::memory_order_relaxed);
        q.at[i].b = nullptr;
    }
    q.mask = cap - 1;
    return true;
}

bool push(fifo &q, buffer *b)
{
    for (;;) {
        uint64_t t = q.tail.load(std::memory_order_relaxed);
        slot &s = q.at[t & q.mask];
        int64_t d = int64_t(s.seq.load(std::memory_order_acquire)) - int64_t(t);
        if (d < 0) {
            return false;
        }
        if (d == 0 && q.tail.compare_exchange_weak(t, t + 1,
                                                   std::memory_order_relaxed)) {
            s.b = b;
            s.seq.store(t + 1, std::memory_order_release);
            q.bytes.fetch_add(size(*b), std::memory_order_relaxed);
            return true;
        }
    }
}

buffer *pop(fifo &q)
{
    for (;;) {
        uint64_t h = q.head.load(std::memory_order_relaxed);
        slot &s = q.at[h & q.mask];
        int64_t d = int64_t(s.seq.load(std::memory_order_acquire)) - int64_t(h + 1);
        if (d < 0) {
            return nullptr;
        }
        if (d == 0 && q.head.compare_exchange_weak(h, h + 1,
                                                   std::memory_order_relaxed)) {
            buffer *b = s.b;
            s.seq.store(h + q.mask + 1, std::memory_order_release);
            q.bytes.fetch_sub(size(*b), std::memory_order_relaxed);
            return b;
        }
    }
}

void enqueue(fifo &q, buffer *b)
{
    if (!push(q, b)) {
        abort("pagecache: the policy queue is full\n");
    }
}

uint32_t name_of(uint64_t off)
{
    uint64_t h = (off / page_size) * 0x9e3779b97f4a7c15ull;
    return uint32_t(h >> 32) | 1;
}

bool remembered(queues &s, uint64_t off)
{
    uint32_t n = name_of(off);
    return s.ghost[n & s.ghost_mask].load(std::memory_order_relaxed) == n;
}

void remember(queues &s, uint64_t off)
{
    uint32_t n = name_of(off);
    s.ghost[n & s.ghost_mask].store(n, std::memory_order_relaxed);
}

void destroy(void *p)
{
    auto *s = static_cast<queues *>(p);
    if (!s) {
        return;
    }
    std::free(s->small.at);
    std::free(s->main.at);
    std::free(s->ghost);
    delete s;
}

void *create(uint64_t store_size)
{
    auto *s = new (std::nothrow) queues();
    if (!s) {
        return nullptr;
    }

    // A slot for every buffer the cache could hold, so that a push never
    // fails and nothing goes untracked.
    uint64_t capacity =
        std::min<uint64_t>(store_size, frames::phys_mem_size) / page_size;
    uint64_t cap = uint64_t(1)
                   << ilog2_roundup(std::max<uint64_t>(capacity, ring_cap_min));

    uint64_t want = std::min<uint64_t>(
        std::max<uint64_t>(store_size / page_size / 8, 1024), 1u << 20);
    s->ghost_mask = (size_t(1) << ilog2_roundup(want)) - 1;
    s->ghost = static_cast<std::atomic<uint32_t> *>(
        std::calloc(s->ghost_mask + 1, sizeof(uint32_t)));

    if (!s->ghost || !fifo_make(s->small, cap) || !fifo_make(s->main, cap)) {
        destroy(s);
        return nullptr;
    }
    return s;
}

// One page, wherever the fault landed.
void fault_extent(void *, uint64_t off, uint64_t *start, uint64_t *len)
{
    *start = off & ~uint64_t(page_size - 1);
    *len = page_size;
}

void on_fault(void *p, buffer &b)
{
    auto *s = static_cast<queues *>(p);
    if (!s) {
        return;
    }
    // Don't upgrade prefetched pages into the main queue
    if (prefetched(b)) {
        enqueue(s->small, &b);
        return;
    }
    enqueue(remembered(*s, offset(b)) ? s->main : s->small, &b);
}

void evict(void *p, size_t bytes, buffer_list &victims)
{
    auto *s = static_cast<queues *>(p);
    if (!s) {
        return;
    }
    size_t got = 0;
    unsigned chances = 0;
    for (unsigned step = 0;
         got < bytes && !victims.full() && step < steps_max; step++) {
        size_t sb = s->small.bytes.load(std::memory_order_relaxed);
        size_t mb = s->main.bytes.load(std::memory_order_relaxed);
        // Probation is over once the small queue exceeds its share.
        bool from_small = sb * small_share > sb + mb;
        buffer *b = pop(from_small ? s->small : s->main);
        if (!b) {
            from_small = !from_small;
            b = pop(from_small ? s->small : s->main);
        }
        if (!b) {
            break;
        }
        if (accessed(*b) && chances < steps_max / 2) {
            // A second chance, capped so that hot buffers cannot starve the
            // caller of a victim.
            clear_accessed(*b);
            chances++;
            if (prefetched(*b)) {
                clear_prefetched(*b);
                enqueue(s->small, b);
            } else {
                enqueue(s->main, b);
            }
            continue;
        }
        if (from_small) {
            remember(*s, offset(*b));
        }
        victims.add(b);
        got += size(*b);
    }
}

void on_evicted(void *, buffer &)
{
}

const policy s3fifo = {
    .bytes_per_buffer = 0,
    .create = create,
    .destroy = destroy,
    .fault_extent = fault_extent,
    .prefetch = nullptr,
    .prefetch_depth = 0,
    .evict = evict,
    .on_fault = on_fault,
    .on_evicted = on_evicted,
    .is_dirty = nullptr,
};

} // namespace

const policy &defaults()
{
    return s3fifo;
}

}
}
