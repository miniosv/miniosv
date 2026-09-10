/*
 * Building and following the page tables: one entry at a time.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <string.h>

#include <osv/mutex.h>
#include <osv/rcu.hh>

#include "internal.hh"
#include "../linear.hh"

namespace mem {
namespace mapping {

void walk_result::retire(frames::phys_addr p)
{
    if (count == max_retired) {
        flush_all();
        settle();
    }
    retired[count++] = p;
}

void walk_result::settle()
{
    for (unsigned i = 0; i < count; ++i) {
        osv::rcu_defer([](void *page) {
            frames::free(frames::from_linear(page));
        }, frames::to_linear(retired[i]));
    }
    count = 0;
}

pte build_table(std::atomic<pte> *slot)
{
    frames::phys_addr p = frames::alloc();
    if (p == frames::no_memory) {
        return 0;
    }
    memset(frames::to_linear(p), 0, page_size);

    pte want = pte_make_table(p);
    pte have = 0;
    pte_barrier();
    if (slot->compare_exchange_strong(have, want, std::memory_order_acq_rel)) {
        return want;
    }
    // Another cpu built one first. Its table is as good as ours.
    frames::free(p);
    return have;
}

pte break_leaf(std::atomic<pte> *slot, unsigned level, pte e)
{
    frames::phys_addr p = frames::alloc();
    if (p == frames::no_memory) {
        return 0;
    }
    auto *child = static_cast<std::atomic<pte> *>(frames::to_linear(p));
    for (unsigned i = 0; i < entries_per_table; ++i) {
        child[i].store(pte_demote(e, level, i), std::memory_order_relaxed);
    }

    pte want = pte_make_table(p);
    pte_barrier();
    if (slot->compare_exchange_strong(e, want, std::memory_order_acq_rel)) {
        return want;
    }
    frames::free(p);
    return e;
}

// The read lock keeps a table this descent is standing in from being handed
// back under it; a reclaimed table is freed through rcu for exactly this.
pte_ref find(uintptr_t addr)
{
    WITH_LOCK(osv::rcu_read_lock) {
        auto *slot = root_slot(addr);
        pte e = slot->load(std::memory_order_acquire);

        for (unsigned level = levels; level > 0; ) {
            if (pte_empty(e)) {
                return {};
            }
            if (level < levels && pte_is_leaf(e, level)) {
                return {slot, level};
            }
            --level;
            slot = &table_of(e)[level_index(addr, level)];
            e = slot->load(std::memory_order_acquire);
        }
        return pte_empty(e) ? pte_ref() : pte_ref(slot, 0);
    }
}

frames::phys_addr to_phys(void *addr)
{
    return to_phys(reinterpret_cast<uintptr_t>(addr));
}

frames::phys_addr to_phys(uintptr_t addr)
{
    if (frames::in_linear_map(reinterpret_cast<void *>(addr))) {
        return frames::from_linear(reinterpret_cast<void *>(addr));
    }
    auto e = find(addr);
    if (!e) {
        return frames::no_memory;
    }
    return e.addr() + (addr & (e.size() - 1));
}

bool is_contiguous(const void *addr, size_t bytes)
{
    auto start = reinterpret_cast<uintptr_t>(addr);
    if (frames::in_linear_map(addr, bytes)) {
        return true;
    }
    // Leaf by leaf: each one has to pick up where the last left off.
    uintptr_t end = start + bytes;
    frames::phys_addr expect = frames::no_memory;
    for (uintptr_t va = start; va < end;) {
        auto e = find(va);
        if (!e) {
            return false;
        }
        frames::phys_addr p = e.addr() + (va & (e.size() - 1));
        if (va != start && p != expect) {
            return false;
        }
        uintptr_t next = (va & ~(e.size() - 1)) + e.size();
        expect = p + (next - va);
        va = next;
    }
    return true;
}

pte_ref prepare(uintptr_t addr, size_t leaf_size)
{
    unsigned target = level_of(leaf_size);
    auto *slot = root_slot(addr);
    bool split = false;

    for (unsigned level = levels; level > target; ) {
        pte e = slot->load(std::memory_order_acquire);
        while (pte_empty(e) || (level < levels && pte_is_leaf(e, level))) {
            if (pte_empty(e)) {
                e = build_table(slot);
            } else {
                e = break_leaf(slot, level, e);
                split = true;
            }
            if (!e) {
                return {};
            }
        }
        --level;
        slot = &table_of(e)[level_index(addr, level)];
    }

    // An empty table left behind by a smaller mapping gives way to the leaf.
    walk_result res;
    if (target > 0) {
        pte e = slot->load(std::memory_order_acquire);
        if (!pte_empty(e) && !pte_is_leaf(e, target) && table_is_empty(table_of(e))) {
            if (slot->compare_exchange_strong(e, 0, std::memory_order_acq_rel)) {
                res.retire(pte_table_addr(e));
                split = true;
            }
        }
    }
    if (split) {
        flush_all();
    }
    res.settle();
    return {slot, target};
}

bool prepare(range r, size_t leaf_size)
{
    walk_opts o{true, true, false, level_of(leaf_size)};
    auto res = walk_range(r, o, [](pte_ref, uintptr_t) { return true; });
    if (res.split) {
        flush_all();
    }
    res.settle();
    return res.complete;
}

}
}
