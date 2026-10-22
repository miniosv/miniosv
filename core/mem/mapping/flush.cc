/*
 * Invalidation, and the epoch counter that lets a client detach without one.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <atomic>

#include <osv/align.hh>
#include <osv/mem/mapping.hh>

namespace mem {
namespace mapping {

// Global flushes started, and global flushes finished. Two counters, because
// what a client needs to know is that one began after its entries were cleared,
// which a count of completions alone cannot say.
static std::atomic<uint64_t> begun, done;

uint64_t flush_epoch()
{
    // The caller has just cleared entries. They must be visible to every
    // page-table walker before a flush another cpu begins from here on is
    // counted as having covered them.
    pte_barrier();
    return begun.load(std::memory_order_seq_cst);
}

void flush_local(range r)
{
    if (r.size() > flush_batch * page_size) {
        tlb_flush_local();
        return;
    }
    for (uintptr_t va = r.start; va < r.end; va += page_size) {
        tlb_flush_page(va);
    }
}

// Naming each address in turn, on every cpu.
// Does not move the epoch.
void flush_range(range r)
{
    size_t pages = align_up(r.size(), page_size) / page_size;
    if (pages > flush_batch) {
        flush_all();
        return;
    }
    uintptr_t va[flush_batch];
    uintptr_t a = align_down(r.start, page_size);
    for (size_t i = 0; i < pages; i++, a += page_size) {
        va[i] = a;
    }
    tlb_flush_pages_all(va, pages);
}

void flush_all()
{
    begun.fetch_add(1, std::memory_order_seq_cst);
    tlb_flush_all();
    done.fetch_add(1, std::memory_order_seq_cst);
}

void pending_invalidation::add(uintptr_t addr)
{
    if (count == flush_batch) {
        all = true;
        return;
    }
    va[count++] = addr;
}

void pending_invalidation::invalidate()
{
    // More flushes have finished than had started when the entries were
    // cleared, so one of them started after: it named these addresses already.
    // Flushes overlap, which is why finishing later is not enough on its own.
    if ((count || all) && done.load(std::memory_order_seq_cst) <= epoch) {
        if (all) {
            flush_all();
        } else {
            tlb_flush_pages_all(va, count);
        }
    }
    count = 0;
    all = false;
    epoch = never_flushed;
}

}
}
