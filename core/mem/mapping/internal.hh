/*
 * The page-table walk, shared by everything that operates on a range.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef MEM_MAPPING_INTERNAL_HH
#define MEM_MAPPING_INTERNAL_HH

#include <osv/mem/mapping.hh>
#include <osv/mem/frames.hh>
#include <osv/align.hh>
#include "../linear.hh"

namespace mem {
namespace mapping {

// What a walk may do to the tables it crosses on the way.
struct walk_opts {
    bool create = false;      // build missing interior levels
    bool split = false;       // break a large leaf the range covers only part of
    bool reclaim = false;     // give back a table the walk leaves empty
    unsigned max_level = 0;   // largest leaf the walk may hand to fn
    bool replace = false;     // hand fn the slot of an empty table it covers whole
};

struct walk_result {
    bool complete = true;     // fn ran over the whole range
    bool split = false;       // a large leaf was broken up on the way

    static constexpr unsigned max_retired = 16;
    frames::phys_addr retired[max_retired];
    unsigned count = 0;

    void retire(frames::phys_addr p);
    void settle();
};

// The table below this entry, building it if it is not there. Returns the
// entry as it stands afterwards, or 0 if there was no memory for the table.
pte build_table(std::atomic<pte> *slot);

// Replace a large leaf with a table of leaves one level down over the same
// frames. Leaves the TLB to the caller.
pte break_leaf(std::atomic<pte> *slot, unsigned level, pte e);

inline std::atomic<pte> *table_of(pte e)
{
    return static_cast<std::atomic<pte> *>(frames::to_linear(pte_table_addr(e)));
}

inline bool table_is_empty(std::atomic<pte> *table)
{
    for (unsigned i = 0; i < entries_per_table; ++i) {
        if (!pte_empty(table[i].load(std::memory_order_relaxed))) {
            return false;
        }
    }
    return true;
}

/*
 * Visit every leaf in region "r", based what "walk_opts" allows.
 * An entry is handed to fn when it is a leaf level, or when
 * the range covers the whole of what the entry spans and nothing finer is
 * already mapped there; otherwise the walk descends.
 *
 * fn returns false to stop the walk. Uses "level" to indicate the level of
 * recursion, called with levels - 1 for the root table.
 */
template <typename Fn>
static bool walk_table(std::atomic<pte> *table, unsigned level, range r,
                       const walk_opts &o, Fn &fn, walk_result &res)
{
    size_t step = level_size(level);
    uintptr_t va = align_down(r.start, step);

    for (unsigned i = level_index(va, level);
         i < entries_per_table && va < r.end; ++i, va += step) {
        auto *slot = &table[i];
        pte e = slot->load(std::memory_order_acquire);

        bool whole = level <= o.max_level && va >= r.start && va + step <= r.end;
        // An empty table left behind by a smaller mapping gives way to a leaf.
        if (whole && o.replace && !pte_empty(e) && !pte_is_leaf(e, level) &&
            table_is_empty(table_of(e))) {
            pte was = e;
            if (slot->compare_exchange_strong(was, 0, std::memory_order_acq_rel)) {
                res.retire(pte_table_addr(e));
                res.split = true;
                e = 0;
            } else {
                e = was;
            }
        }
        if (level == 0 || (whole && (pte_empty(e) || pte_is_leaf(e, level)))) {
            if (!fn(pte_ref(slot, level), va)) {
                return false;
            }
            continue;
        }

        // Turn whatever is here into a table to descend through.
        bool skip = false;
        while (pte_empty(e) || pte_is_leaf(e, level)) {
            if (pte_empty(e)) {
                if (!o.create) {
                    skip = true;
                    break;
                }
                e = build_table(slot);
            } else {
                if (!o.split) {
                    // The caller wants the large entry whole, even though the
                    // range covers only part of what it maps.
                    if (!fn(pte_ref(slot, level), va)) {
                        return false;
                    }
                    skip = true;
                    break;
                }
                e = break_leaf(slot, level, e);
                res.split = true;
            }
            if (!e) {
                return false;
            }
        }
        if (skip) {
            continue;
        }

        bool covers = va >= r.start && va + step <= r.end;
        auto *child = table_of(e);
        range under = {va > r.start ? va : r.start,
                       va + step < r.end ? va + step : r.end};
        if (!walk_table(child, level - 1, under, o, fn, res)) {
            return false;
        }

        if (o.reclaim && level == 1 && covers && table_is_empty(child)) {
            pte expected = e;
            if (slot->compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                res.retire(pte_table_addr(e));
            }
        }
    }
    return true;
}

template <typename Fn>
static walk_result walk_range(range r, const walk_opts &o, Fn fn)
{
    walk_result res;
    if (r.empty()) {
        return res;
    }

    auto *root = root_slot(r.start);
    pte e = root->load(std::memory_order_acquire);
    if (pte_empty(e)) {
        if (!o.create) {
            return res;
        }
        e = build_table(root);
        if (!e) {
            res.complete = false;
            return res;
        }
    }
    res.complete = walk_table(table_of(e), levels - 1, r, o, fn, res);
    return res;
}

}
}

#endif /* MEM_MAPPING_INTERNAL_HH */
