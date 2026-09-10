/*
 * Where the time in a fault goes.
 * Set CONF_pagecache_stats at compile time to turn on.
 */

#ifndef MEM_PAGECACHE_STATS_HH
#define MEM_PAGECACHE_STATS_HH

#include <atomic>
#include <cstdint>

#include <osv/kernel_config.h>

namespace mem {
namespace pagecache {

#if CONF_pagecache_stats

struct counters {
    std::atomic<uint64_t> fault_ticks{0};      // the whole fault
    std::atomic<uint64_t> claim_ticks{0};      // choosing the tile, claiming frames, submitting
    std::atomic<uint64_t> wait_ticks{0};       // waiting for the device, then publishing
    std::atomic<uint64_t> room_ticks{0};       // eviction, inside the claim
    std::atomic<uint64_t> spin_ticks{0};       // waiting for another cpu's load
    std::atomic<uint64_t> adopt_ticks{0};      // finishing a prefetch someone parked

    std::atomic<uint64_t> faults{0};           // calls that had to bring something in
    std::atomic<uint64_t> hits{0};             // calls that found it already there
    std::atomic<uint64_t> bytes_in{0};         // tile bytes brought in
    std::atomic<uint64_t> spins{0};            // trips round a spin loop
    std::atomic<uint64_t> evicted{0};          // buffers taken back
    std::atomic<uint64_t> evicted_bytes{0};
    std::atomic<uint64_t> prefetched{0};       // reads started ahead of demand
    std::atomic<uint64_t> adopted{0};          // of those, installed by a later fault
    std::atomic<uint64_t> completed{0};        // and published straight from the completion
    std::atomic<uint64_t> nomem{0};            // faults that could not get memory
};

extern counters stats;

inline void add(std::atomic<uint64_t> &c, uint64_t v)
{
    c.fetch_add(v, std::memory_order_relaxed);
}

struct phase {
    std::atomic<uint64_t> &c;
    uint64_t t0;
    explicit phase(std::atomic<uint64_t> &into)
        : c(into), t0(processor::ticks()) {}
    ~phase() { add(c, processor::ticks() - t0); }
};


struct spin_watch {
    uint64_t t0 = 0, n = 0;
    bool running = false;
    void tick()
    {
        if (!running) { t0 = processor::ticks(); running = true; }
        n++;
    }
    void stop()
    {
        if (running) { add(stats.spin_ticks, processor::ticks() - t0); running = false; }
    }
    ~spin_watch() { stop(); if (n) { add(stats.spins, n); } }
};

#define PAGECACHE_PHASE(which) ::mem::pagecache::phase _phase_##which(::mem::pagecache::stats.which)
#define PAGECACHE_COUNT(which, n) ::mem::pagecache::add(::mem::pagecache::stats.which, (n))

#else

struct spin_watch {
    void tick() {}
    void stop() {}
};

#define PAGECACHE_PHASE(which) do {} while (0)
#define PAGECACHE_COUNT(which, n) do {} while (0)

#endif

}
}

#endif /* MEM_PAGECACHE_STATS_HH */
