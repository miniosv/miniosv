/*
 * A maple tree over the reserved memory regions, after Linux's Maple tree (Howlett and Wilcox).
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "index.hh"
#include <osv/mem/frames.hh>
#include <osv/mutex.h>
#include <osv/align.hh>
#include <osv/debug.hh>
#include <atomic>
#include <algorithm>
#include <string.h>
#include "../linear.hh"

namespace mem {
namespace vspace {
namespace index {

namespace {

constexpr unsigned slots = 16;
constexpr uintptr_t entry_tag = 1;

// Bound the tree depth to catch stale reads.
constexpr unsigned max_depth = 16;

// Optimistic attempts before a reader falls back to the writer's mutex.
constexpr unsigned optimistic_tries = 5;

struct entry {
    range span;
    region *r;
    entry *next_free;
};

struct node {
    std::atomic<uintptr_t> pivot[slots]; // upper bound address covered by slot[i]
    std::atomic<uintptr_t> slot[slots]; // the entry or child for pivot[i]
    uintptr_t gap[slots]; // largest free range below slot[i]
    node *parent;
    unsigned char parent_slot;
    std::atomic<unsigned char> count;
    bool leaf;
};

mutex write_mutex;
std::atomic<uint64_t> version;
std::atomic<node *> tree_root;

node *spare_nodes;
entry *spare_entries;
// Entries retired outside the mutex, taken back the next time one is wanted.
std::atomic<entry *> retired_entries;
std::atomic<size_t> live_count;
std::atomic<size_t> live_bytes;

/* Accessing node metadata */

uintptr_t tag_of(entry *e)
{
    return reinterpret_cast<uintptr_t>(e) | entry_tag;
}

uintptr_t tag_of(node *n)
{
    return reinterpret_cast<uintptr_t>(n);
}

bool holds_entry(uintptr_t v)
{
    return v & entry_tag;
}

entry *to_entry(uintptr_t v)
{
    return reinterpret_cast<entry *>(v & ~entry_tag);
}

node *to_child(uintptr_t v)
{
    return reinterpret_cast<node *>(v);
}

uintptr_t slot_of(node *n, unsigned i)
{
    return n->slot[i].load(std::memory_order_acquire);
}

void set_slot(node *n, unsigned i, uintptr_t v)
{
    n->slot[i].store(v, std::memory_order_release);
}

uintptr_t pivot_of(node *n, unsigned i)
{
    return n->pivot[i].load(std::memory_order_relaxed);
}

void set_pivot(node *n, unsigned i, uintptr_t v)
{
    n->pivot[i].store(v, std::memory_order_relaxed);
}

unsigned count_of(node *n)
{
    return n->count.load(std::memory_order_relaxed);
}

void set_count(node *n, unsigned c)
{
    n->count.store(c, std::memory_order_release);
}

node *root_of()
{
    return tree_root.load(std::memory_order_acquire);
}

/* Node pools */

void *carve_page()
{
    void *page = frames::to_linear(frames::alloc());
    if (!page) {
        abort("vspace: out of memory for the region index");
    }
    memset(page, 0, frames::page_size);
    return page;
}

node *alloc_node(bool leaf)
{
    if (!spare_nodes) {
        char *page = static_cast<char *>(carve_page());
        for (size_t off = 0; off + sizeof(node) <= frames::page_size; off += sizeof(node)) {
            node *n = reinterpret_cast<node *>(page + off);
            n->parent = spare_nodes;
            spare_nodes = n;
        }
    }
    node *n = spare_nodes;
    spare_nodes = n->parent;
    memset(n, 0, sizeof(node));
    n->leaf = leaf;
    return n;
}

// The node stays readable and stays a node; only its contents stop meaning
// anything. The free list is threaded through `parent`, which no reader reads.
void free_node(node *n)
{
    set_count(n, 0);
    n->parent = spare_nodes;
    spare_nodes = n;
}

entry *alloc_entry()
{
    if (!spare_entries) {
        spare_entries = retired_entries.exchange(nullptr, std::memory_order_acquire);
    }
    if (!spare_entries) {
        char *page = static_cast<char *>(carve_page());
        for (size_t off = 0; off + sizeof(entry) <= frames::page_size; off += sizeof(entry)) {
            entry *e = reinterpret_cast<entry *>(page + off);
            e->next_free = spare_entries;
            spare_entries = e;
        }
    }
    entry *e = spare_entries;
    spare_entries = e->next_free;
    return e;
}

void free_entry(entry *e)
{
    entry *head = retired_entries.load(std::memory_order_relaxed);
    do {
        e->next_free = head;
    } while (!retired_entries.compare_exchange_weak(head, e,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed));
}

/* Looking up in the tree */

// The entry a walk from the root arrives at, or null. Correct only if the
// caller then proves it, either against `version` or under the entry's lock.
// `lost` says the walk gave up rather than concluding anything.
entry *walk(uintptr_t addr, bool &lost)
{
    lost = false;
    node *n = root_of();
    for (unsigned depth = 0; n; depth++) {
        if (depth == max_depth) {
            lost = true;
            return nullptr;
        }
        unsigned c = std::min<unsigned>(count_of(n), slots);
        unsigned i = 0;
        while (i < c && pivot_of(n, i) < addr) {
            i++;
        }
        if (i == c) {
            return nullptr;
        }
        uintptr_t v = slot_of(n, i);
        if (!v) {
            return nullptr;
        }
        if (holds_entry(v)) {
            return to_entry(v);
        }
        n = to_child(v);
    }
    return nullptr;
}

// The same, with the writer's mutex held, so it is simply the answer.
entry *at(uintptr_t addr)
{
    bool lost = false;
    entry *e = walk(addr, lost);
    return e && e->span.contains(addr) ? e : nullptr;
}

uint64_t read_begin()
{
    return version.load(std::memory_order_acquire);
}

bool read_valid(uint64_t seq)
{
    return !(seq & 1) && version.load(std::memory_order_acquire) == seq;
}

void write_begin()
{
    version.fetch_add(1, std::memory_order_acq_rel);
}

void write_end()
{
    version.fetch_add(1, std::memory_order_release);
}

/* Inserting/deleting nodes */

// Lookup the first address this node covers.
uintptr_t node_lo(node *n)
{
    while (n->parent) {
        if (n->parent_slot) {
            return pivot_of(n->parent, n->parent_slot - 1) + 1;
        }
        n = n->parent;
    }
    return 0;
}

// The first slot reaching addr, or count if the node ends below it.
unsigned slot_for(node *n, uintptr_t addr)
{
    unsigned i = 0;
    while (i < count_of(n) && pivot_of(n, i) < addr) {
        i++;
    }
    return i;
}

void refresh_pivots(node *n)
{
    for (unsigned i = 0; i < count_of(n); i++) {
        if (n->leaf) {
            set_pivot(n, i, to_entry(slot_of(n, i))->span.end - 1);
        } else {
            node *c = to_child(slot_of(n, i));
            set_pivot(n, i, pivot_of(c, count_of(c) - 1));
        }
    }
}

void refresh_gaps(node *n)
{
    uintptr_t lo = node_lo(n);
    for (unsigned i = 0; i < count_of(n); i++) {
        if (n->leaf) {
            entry *e = to_entry(slot_of(n, i));
            n->gap[i] = e->span.start - lo;
            lo = e->span.end;
        } else {
            node *c = to_child(slot_of(n, i));
            uintptr_t widest = 0;
            for (unsigned j = 0; j < count_of(c); j++) {
                widest = std::max(widest, c->gap[j]);
            }
            n->gap[i] = widest;
            lo = pivot_of(n, i) + 1;
        }
    }
}

void refresh(node *n)
{
    refresh_pivots(n);
    refresh_gaps(n);
}

// Pivots first for both paths, then gaps: a node's gaps are measured from its
// left neighbour's last pivot, so every pivot has to be right before any gap is.
void fix_up(node *a, node *b)
{
    for (node *n = a; n; n = n->parent) {
        refresh_pivots(n);
    }
    for (node *n = b; n; n = n->parent) {
        refresh_pivots(n);
    }
    for (node *n = a; n; n = n->parent) {
        refresh_gaps(n);
    }
    for (node *n = b; n; n = n->parent) {
        refresh_gaps(n);
    }
}

void adopt(node *n)
{
    if (n->leaf) {
        return;
    }
    for (unsigned i = 0; i < count_of(n); i++) {
        node *c = to_child(slot_of(n, i));
        c->parent = n;
        c->parent_slot = i;
    }
}

// The slot is written before the count grows, so a reader that sees the wider
// node sees something real in the new place.
void insert_slot(node *n, unsigned at_i, uintptr_t value)
{
    unsigned c = count_of(n);
    for (unsigned i = c; i > at_i; i--) {
        set_slot(n, i, slot_of(n, i - 1));
        set_pivot(n, i, pivot_of(n, i - 1));
        n->gap[i] = n->gap[i - 1];
    }
    set_slot(n, at_i, value);
    set_count(n, c + 1);
    adopt(n);
}

// Slots past the count keep their old values on purpose: a reader that is
// still looking at the wider node finds the region rather than nothing.
void erase_slot(node *n, unsigned at_i)
{
    unsigned c = count_of(n);
    for (unsigned i = at_i; i + 1 < c; i++) {
        set_slot(n, i, slot_of(n, i + 1));
        set_pivot(n, i, pivot_of(n, i + 1));
        n->gap[i] = n->gap[i + 1];
    }
    set_count(n, c - 1);
    adopt(n);
}

// A full node keeps its lower half and hands the rest to a new sibling on its
// right, which the parent takes in -- splitting in turn if that fills it.
//  The sibling is published before the old node is narrowed, so the entries that
// moved are reachable through one node or the other throughout.
void split(node *n)
{
    node *right = alloc_node(n->leaf);
    unsigned half = slots / 2;
    unsigned c = count_of(n);
    for (unsigned i = half; i < c; i++) {
        set_slot(right, i - half, slot_of(n, i));
        set_pivot(right, i - half, pivot_of(n, i));
        right->gap[i - half] = n->gap[i];
    }
    set_count(right, c - half);
    adopt(right);

    if (!n->parent) {
        node *fresh = alloc_node(false);
        set_slot(fresh, 0, tag_of(n));
        set_slot(fresh, 1, tag_of(right));
        set_count(fresh, 2);
        adopt(fresh);
        set_count(n, half);
        adopt(n);
        refresh(n);
        refresh_pivots(fresh);
        refresh(right);
        tree_root.store(fresh, std::memory_order_release);
        refresh_gaps(fresh);
        return;
    }

    node *parent = n->parent;
    insert_slot(parent, n->parent_slot + 1, tag_of(right));
    set_count(n, half);
    adopt(n);
    refresh(n);
    refresh_pivots(parent);
    refresh(right);
    refresh(parent);
    if (count_of(parent) == slots) {
        split(parent);
    }
}

// Drop an empty node, and whatever becomes empty above it. Returns the lowest
// node that survived, or null if the tree is now empty.
node *prune(node *n)
{
    while (n->parent && count_of(n) == 0) {
        node *parent = n->parent;
        unsigned at_i = n->parent_slot;
        free_node(n);
        erase_slot(parent, at_i);
        n = parent;
    }
    if (count_of(n) == 0) {
        tree_root.store(nullptr, std::memory_order_release);
        free_node(n);
        return nullptr;
    }
    while (n == root_of() && !n->leaf && count_of(n) == 1) {
        node *only = to_child(slot_of(n, 0));
        only->parent = nullptr;
        only->parent_slot = 0;
        tree_root.store(only, std::memory_order_release);
        free_node(n);
        n = only;
    }
    return n;
}

// The leaf holding the next region in address order. Its first gap is measured
// from the end of this leaf's last region, so it goes stale whenever that end
// moves.
node *next_leaf(node *n)
{
    while (n->parent && n->parent_slot + 1u >= count_of(n->parent)) {
        n = n->parent;
    }
    if (!n->parent) {
        return nullptr;
    }
    n = to_child(slot_of(n->parent, n->parent_slot + 1));
    while (!n->leaf) {
        n = to_child(slot_of(n, 0));
    }
    return n;
}

node *leaf_for(uintptr_t addr)
{
    node *n = root_of();
    while (n && !n->leaf) {
        unsigned i = slot_for(n, addr);
        n = to_child(slot_of(n, i == count_of(n) ? count_of(n) - 1 : i));
    }
    return n;
}

// The first region reaching at or past addr, with the writer's mutex held.
entry *next_at_or_after(uintptr_t addr)
{
    node *n = root_of();
    while (n) {
        unsigned i = slot_for(n, addr);
        if (i < count_of(n)) {
            if (n->leaf) {
                return to_entry(slot_of(n, i));
            }
            n = to_child(slot_of(n, i));
            continue;
        }
        while (n->parent && n->parent_slot + 1u >= count_of(n->parent)) {
            n = n->parent;
        }
        if (!n->parent) {
            return nullptr;
        }
        n = to_child(slot_of(n->parent, n->parent_slot + 1));
        while (!n->leaf) {
            n = to_child(slot_of(n, 0));
        }
        return to_entry(slot_of(n, 0));
    }
    return nullptr;
}

/* Finding room  */

uintptr_t fit(uintptr_t gs, uintptr_t ge, size_t size, size_t align,
              uintptr_t low, uintptr_t high)
{
    gs = std::max(gs, low);
    ge = std::min(ge, high);
    if (ge <= gs) {
        return 0;
    }
    uintptr_t a = align_up(gs, align);
    if (a < gs || a >= ge || ge - a < size) {
        return 0;
    }
    return a;
}

// Walk the subtree in address order, skipping anything whose largest free run
// is too small. `lo` follows the end of the last region seen.
bool search(node *n, uintptr_t &lo, size_t size, size_t align,
            uintptr_t low, uintptr_t high, uintptr_t &out)
{
    for (unsigned i = 0; i < count_of(n); i++) {
        if (lo >= high) {
            return false;
        }
        if (n->leaf) {
            entry *e = to_entry(slot_of(n, i));
            out = fit(lo, e->span.start, size, align, low, high);
            if (out) {
                return true;
            }
            lo = e->span.end;
        } else if (pivot_of(n, i) >= low && n->gap[i] >= size) {
            if (search(to_child(slot_of(n, i)), lo, size, align, low, high, out)) {
                return true;
            }
        } else {
            lo = pivot_of(n, i) + 1;
        }
    }
    return false;
}

uintptr_t find_gap(size_t size, size_t align, uintptr_t low, uintptr_t high)
{
    uintptr_t lo = 0;
    uintptr_t out = 0;
    node *root = root_of();
    if (root && search(root, lo, size, align, low, high, out)) {
        return out;
    }
    return fit(lo, high, size, align, low, high);
}

/* Modifying the tree */

// The range must be free.
void insert_entry(entry *e)
{
    if (!root_of()) {
        tree_root.store(alloc_node(true), std::memory_order_release);
    }
    node *leaf = leaf_for(e->span.start);
    node *after = next_leaf(leaf);
    unsigned at_i = slot_for(leaf, e->span.start);
    insert_slot(leaf, at_i, tag_of(e));
    // The new slot has no pivot or gap of its own yet, and a split would copy
    // whatever was left there.
    refresh(leaf);
    if (count_of(leaf) == slots) {
        split(leaf);
    }
    fix_up(leaf, after);
    live_count.fetch_add(1, std::memory_order_relaxed);
    live_bytes.fetch_add(e->span.size(), std::memory_order_relaxed);
}

void erase_entry(entry *e)
{
    node *leaf = leaf_for(e->span.start);
    node *after = next_leaf(leaf);
    unsigned at_i = slot_for(leaf, e->span.start);
    erase_slot(leaf, at_i);
    node *dirty = count_of(leaf) ? leaf : prune(leaf);
    fix_up(dirty, after);
    live_count.fetch_sub(1, std::memory_order_relaxed);
    live_bytes.fetch_sub(e->span.size(), std::memory_order_relaxed);
}

bool check_subtree(node *n, uintptr_t &lo, size_t &seen)
{
    bool ok = count_of(n) > 0 && count_of(n) < slots;
    for (unsigned i = 0; i < count_of(n); i++) {
        if (n->leaf) {
            entry *e = to_entry(slot_of(n, i));
            ok = ok && !e->span.empty() && e->span.start >= lo;
            ok = ok && pivot_of(n, i) == e->span.end - 1;
            ok = ok && n->gap[i] == e->span.start - lo;
            ok = ok && e->r && e->r->span.start == e->span.start &&
                 e->r->span.end == e->span.end;
            ok = ok && at(e->span.start) == e;
            ok = ok && at(e->span.end - 1) == e;
            lo = e->span.end;
            seen++;
        } else {
            node *c = to_child(slot_of(n, i));
            ok = ok && c->parent == n && c->parent_slot == i;
            uintptr_t widest = 0;
            for (unsigned j = 0; j < count_of(c); j++) {
                widest = std::max(widest, c->gap[j]);
            }
            ok = ok && n->gap[i] == widest;
            ok = ok && check_subtree(c, lo, seen);
            ok = ok && pivot_of(n, i) == pivot_of(c, count_of(c) - 1);
        }
    }
    return ok;
}

}

bool insert(region &r, size_t bytes, size_t align, range within)
{
    SCOPE_LOCK(write_mutex);
    uintptr_t a = find_gap(bytes, align, within.start, within.end);
    if (!a) {
        return false;
    }
    entry *e = alloc_entry();
    r.span = {a, a + bytes};
    write_begin();
    e->span = r.span;
    e->r = &r;
    insert_entry(e);
    write_end();
    return true;
}

bool insert_at(region &r, range at_range)
{
    SCOPE_LOCK(write_mutex);
    entry *clash = next_at_or_after(at_range.start);
    if (clash && clash->span.start < at_range.end) {
        return false;
    }
    entry *e = alloc_entry();
    r.span = at_range;
    write_begin();
    e->span = at_range;
    e->r = &r;
    insert_entry(e);
    write_end();
    return true;
}

void remove(region &r)
{
    entry *e = nullptr;

    WITH_LOCK(write_mutex) {
        entry *found = at(r.span.start);
        if (!found || found->r != &r) {
            return;
        }
        e = found;
        write_begin();
        erase_entry(e);
        write_end();
    }
    free_entry(e);
}

region *find(uintptr_t addr)
{
    for (unsigned tries = 0; tries < optimistic_tries; tries++) {
        uint64_t seq = read_begin();
        if (seq & 1) {
            continue;
        }
        bool lost = false;
        entry *e = walk(addr, lost);
        region *r = (!lost && e && e->span.contains(addr)) ? e->r : nullptr;
        if (!lost && read_valid(seq)) {
            return r;
        }
    }
    SCOPE_LOCK(write_mutex);
    entry *e = at(addr);
    return e ? e->r : nullptr;
}

bool covered(range r)
{
    if (r.empty()) {
        return false;
    }
    for (unsigned tries = 0; tries < optimistic_tries; tries++) {
        uint64_t seq = read_begin();
        if (seq & 1) {
            continue;
        }
        bool lost = false;
        bool whole = true;
        uintptr_t a = r.start;
        while (a < r.end) {
            entry *e = walk(a, lost);
            if (lost || !e || !e->span.contains(a) || e->span.end <= a) {
                whole = false;
                break;
            }
            a = e->span.end;
        }
        if (!lost && read_valid(seq)) {
            return whole;
        }
    }

    SCOPE_LOCK(write_mutex);
    uintptr_t a = r.start;
    while (a < r.end) {
        entry *e = at(a);
        if (!e) {
            return false;
        }
        a = e->span.end;
    }
    return true;
}

size_t reserved_bytes()
{
    return live_bytes.load(std::memory_order_relaxed);
}

size_t count()
{
    return live_count.load(std::memory_order_relaxed);
}

void for_each(void (*fn)(const region &, void *), void *arg)
{
    SCOPE_LOCK(write_mutex);
    uintptr_t a = 0;
    for (;;) {
        entry *e = next_at_or_after(a);
        if (!e || e->span.end <= a) {
            return;
        }
        fn(*e->r, arg);
        a = e->span.end;
    }
}

bool self_check()
{
    SCOPE_LOCK(write_mutex);
    node *root = root_of();
    if (!root) {
        return !count() && !reserved_bytes();
    }
    uintptr_t lo = 0;
    size_t seen = 0;
    bool ok = check_subtree(root, lo, seen);
    return ok && !root->parent && seen == count();
}

}
}
}
