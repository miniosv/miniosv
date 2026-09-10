/*
 * Where allocation sizes fall, and how many frees arrive knowing the size.
 *
 * Counts are per-cpu and unsynchronised: a thread migrating mid-increment can
 * lose one, which does not matter for a distribution.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEM_HEAP_HISTOGRAM_HH
#define MEM_HEAP_HISTOGRAM_HH

#include <osv/kernel_config.h>
#include <osv/sched.hh>

#include <atomic>
#include <cstdint>

extern bool smp_allocator;

namespace mem {
namespace heap {

#if CONF_memory_histogram

constexpr unsigned hist_buckets = 40;

struct alignas(64) hist_row {
    uint64_t alloc[hist_buckets];
    uint64_t freed_sized[hist_buckets];
    uint64_t freed_total;
};

inline hist_row hist[sched::max_cpus];

inline hist_row &hist_row_for()
{
    if (!smp_allocator) {
        return hist[0];
    }
    auto *c = sched::cpu::current();
    return hist[c && c->id < sched::max_cpus ? c->id : 0];
}

inline unsigned hist_bucket(size_t n)
{
    unsigned b = n < 2 ? 0 : 63 - __builtin_clzll(n);
    return b < hist_buckets ? b : hist_buckets - 1;
}

inline void hist_alloc(size_t n)
{
    hist_row_for().alloc[hist_bucket(n)]++;
}

inline void hist_freed_sized(size_t n)
{
    hist_row_for().freed_sized[hist_bucket(n)]++;
}

// Every free lands here, including the sized deletes below, which fall
// through to free() once they have recorded their size.
inline void hist_freed()
{
    hist_row_for().freed_total++;
}

void histogram_dump();

#else

inline void hist_alloc(size_t) {}
inline void hist_freed_sized(size_t) {}
inline void hist_freed() {}
inline void histogram_dump() {}

#endif

}
}

#endif /* MEM_HEAP_HISTOGRAM_HH */
