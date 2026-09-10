/*
 * Reporting where the time in a fault went.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/clock.hh>
#include <osv/debug.hh>

#include "processor.hh"

#include "internal.hh"
#include "stats.hh"

namespace mem {
namespace pagecache {

#if CONF_pagecache_stats

counters stats;

namespace {


// Ticks per microsecond
uint64_t ticks_per_us()
{
    const int64_t n0 = osv::clock::uptime::now().time_since_epoch().count();
    const uint64_t t0 = processor::ticks();
    int64_t n1;
    do {
        processor::spin_hint();
        n1 = osv::clock::uptime::now().time_since_epoch().count();
    } while (n1 - n0 < 2000000);          // 2 ms
    const uint64_t t1 = processor::ticks();
    const uint64_t ns = uint64_t(n1 - n0);
    return ns ? (t1 - t0) * 1000 / ns : 0;
}

// One row: the phase, how long it took, and its share of the whole.
void line(const char *name, uint64_t ticks, uint64_t tpus, uint64_t total)
{
    const uint64_t us = tpus ? ticks / tpus : 0;
    const unsigned permille = total ? unsigned(ticks * 1000 / total) : 0;
    printf("  %-24s %8llu.%03llu ms  %3u.%u%%\n", name,
           (unsigned long long)(us / 1000), (unsigned long long)(us % 1000),
           permille / 10, permille % 10);
}

} // namespace

void stats_reset()
{
    stats.fault_ticks = 0; stats.claim_ticks = 0; stats.wait_ticks = 0;
    stats.room_ticks = 0;  stats.spin_ticks = 0;  stats.adopt_ticks = 0;
    stats.faults = 0; stats.hits = 0; stats.bytes_in = 0; stats.spins = 0;
    stats.evicted = 0; stats.evicted_bytes = 0;
    stats.prefetched = 0; stats.adopted = 0; stats.completed = 0;
    stats.nomem = 0;
}

void stats_dump()
{
    const uint64_t tpus = ticks_per_us();
    const uint64_t total = stats.fault_ticks.load();
    const uint64_t faults = stats.faults.load();

    printf("\n######## page cache ########\n\n");
    printf("  %llu faults, %llu found resident, %llu MiB read in\n",
           (unsigned long long)faults,
           (unsigned long long)stats.hits.load(),
           (unsigned long long)(stats.bytes_in.load() >> 20));
    printf("  %llu buffers evicted (%llu MiB); %llu prefetched, %llu published "
           "on completion, %llu adopted by a fault; %llu faults found no memory\n",
           (unsigned long long)stats.evicted.load(),
           (unsigned long long)(stats.evicted_bytes.load() >> 20),
           (unsigned long long)stats.prefetched.load(),
           (unsigned long long)stats.completed.load(),
           (unsigned long long)stats.adopted.load(),
           (unsigned long long)stats.nomem.load());
    if (faults && tpus) {
        printf("  %llu us per fault, %llu trips round a spin loop\n",
               (unsigned long long)(total / tpus / faults),
               (unsigned long long)stats.spins.load());
    }

    printf("\n  where the time went (the parts nest inside the whole):\n");
    line("whole fault", total, tpus, total);
    line("  claim and submit", stats.claim_ticks.load(), tpus, total);
    line("    make room", stats.room_ticks.load(), tpus, total);
    line("  wait for the device", stats.wait_ticks.load(), tpus, total);
    line("  install a prefetch", stats.adopt_ticks.load(), tpus, total);
    line("  spin for another cpu", stats.spin_ticks.load(), tpus, total);
    printf("\n");
}

#else

void stats_dump() {}
void stats_reset() {}

#endif

}
}
