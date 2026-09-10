/*
 * Printing the allocation-size histogram.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/debug.hh>

#include "histogram.hh"

#if CONF_memory_histogram

namespace mem {
namespace heap {

void histogram_dump()
{
    // Every exit route ends in poweroff(), and some of them arrive twice.
    static std::atomic<bool> dumped;
    if (dumped.exchange(true)) {
        return;
    }

    uint64_t alloc[hist_buckets] = {}, sized[hist_buckets] = {}, freed = 0;
    for (unsigned c = 0; c < sched::max_cpus; c++) {
        for (unsigned b = 0; b < hist_buckets; b++) {
            alloc[b] += hist[c].alloc[b];
            sized[b] += hist[c].freed_sized[b];
        }
        freed += hist[c].freed_total;
    }

    uint64_t alloc_total = 0, sized_total = 0;
    for (unsigned b = 0; b < hist_buckets; b++) {
        alloc_total += alloc[b];
        sized_total += sized[b];
    }
    if (!alloc_total) {
        return;
    }

    printf("\n######## allocation histogram ########\n");
    printf("%14s %14s %8s %14s\n", "size", "allocations", "share", "sized frees");
    uint64_t cum = 0;
    for (unsigned b = 0; b < hist_buckets; b++) {
        if (!alloc[b] && !sized[b]) {
            continue;
        }
        cum += alloc[b];
        char label[24];
        uint64_t lo = b ? (uint64_t(1) << b) : 0;
        if (lo >= (1ul << 20)) {
            snprintf(label, sizeof(label), "%lu MiB", lo >> 20);
        } else if (lo >= (1ul << 10)) {
            snprintf(label, sizeof(label), "%lu KiB", lo >> 10);
        } else {
            snprintf(label, sizeof(label), "%lu B", lo);
        }
        printf("%12s.. %14lu %7.2f%% %14lu\n", label, alloc[b],
               100.0 * cum / alloc_total, sized[b]);
    }
    printf("\n  allocations     %lu\n", alloc_total);
    printf("  frees           %lu\n", freed);
    printf("  of them sized   %lu (%.2f%%)\n", sized_total,
           100.0 * sized_total / (freed ? freed : 1));
    printf("######## end allocation histogram ########\n");
    fflush(stdout);
}

}
}

#endif
