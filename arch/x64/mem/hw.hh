/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * x86-64 specific memory operations and formats.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_MEM_HW_HH
#define ARCH_MEM_HW_HH

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include <osv/mem/types.hh>

namespace mem {
namespace mapping {

using pte = uint64_t;
constexpr unsigned levels = 4;
constexpr unsigned entries_per_table = 512;

constexpr unsigned max_leaf_level = 1; // only 2MiB for now

// Software available bits, starting at bit 53.
constexpr unsigned sw_bits = 10;

constexpr unsigned level_shift(unsigned level)
{
    return 12 + 9 * level;
}

constexpr size_t level_size(unsigned level)
{
    return size_t(1) << level_shift(level);
}

constexpr unsigned level_index(uintptr_t va, unsigned level)
{
    return (va >> level_shift(level)) & (entries_per_table - 1);
}

enum : pte {
    pte_p    = pte(1) << 0,    // present
    pte_w    = pte(1) << 1,    // writable
    pte_u    = pte(1) << 2,    // reachable from ring 3
    pte_a    = pte(1) << 5,    // accessed
    pte_d    = pte(1) << 6,    // dirty
    pte_hp   = pte(1) << 7,    // Huge page flag

    pte_none = pte(1) << 51,   // reserved bit, (present entry with no permissions)
    pte_nx   = pte(1) << 63,   // not executable
};

constexpr unsigned addr_bits = 51; // number of bits in a physical address

constexpr pte addr_mask(bool large)
{
    return ((pte(1) << addr_bits) - 1) & ~pte(0xfff) & ~(pte(large) << 12);
}

inline bool pte_empty(pte e) { return !e; }
inline bool pte_present(pte e) { return e & pte_p; }

inline bool pte_is_leaf(pte e, unsigned level)
{
    return level == 0 || (level <= max_leaf_level && (e & pte_hp));
}

inline frames::phys_addr pte_addr(pte e, unsigned level)
{
    return e & addr_mask(level > 0 && pte_is_leaf(e, level));
}

inline frames::phys_addr pte_table_addr(pte e) { return e & addr_mask(false); }

inline unsigned pte_perm(pte e)
{
    if (!(e & pte_p) || (e & pte_none)) {
        return 0;
    }
    return perm_read | ((e & pte_w) ? perm_write : 0) | ((e & pte_nx) ? 0 : perm_exec);
}

inline pte pte_make_table(frames::phys_addr p)
{
    return p | pte_p | pte_w | pte_u | pte_a;
}

inline pte pte_make_leaf(frames::phys_addr p, unsigned perm, unsigned level, mattr)
{
    pte e = p | pte_u | pte_a | pte_d;
    if (perm) {
        e |= pte_p;
    }
    if (perm & perm_write) {
        e |= pte_w;
    }
    if (!(perm & perm_exec)) {
        e |= pte_nx;
    }
    if (level > 0) {
        e |= pte_hp;
    }
    return e;
}

// Change permissions on an existing entry.
// Possible: none, read, read+write, read+exec, read+write+exec.
inline pte pte_with_perm(pte e, unsigned perm)
{
    e |= pte_p;
    e = (perm & perm_write) ? e | pte_w : e & ~pte_w;
    e = (perm & perm_exec) ? e & ~pte_nx : e | pte_nx;
    return perm ? e & ~pte_none : e | pte_none;
}

// A permission taken away can still be cached; one handed out cannot.
inline bool pte_perm_change_needs_flush(unsigned old, unsigned neu)
{
    return old & ~neu;
}

constexpr bool tracks_writes = true;

inline pte pte_set_present(pte e, bool v) { return v ? e | pte_p : e & ~pte_p; }
inline bool pte_accessed(pte e) { return e & pte_a; }
inline bool pte_dirty(pte e) { return e & pte_d; }
inline pte pte_set_accessed(pte e, bool v) { return v ? e | pte_a : e & ~pte_a; }
inline pte pte_set_dirty(pte e, bool v) { return v ? e | pte_d : e & ~pte_d; }
inline bool pte_sw_bit(pte e, unsigned n) { return (e >> (53 + n)) & 1; }

inline pte pte_set_sw_bit(pte e, unsigned n, bool v)
{
    pte bit = pte(1) << (53 + n);
    return v ? e | bit : e & ~bit;
}

// The entry one level down that maps the `index`th slice of what `e` maps at
// `level`. Bit 12 carries an address bit in a small leaf and selects a page
// attribute in a large one; nothing here uses page attributes on x86-64.
inline pte pte_demote(pte e, unsigned level, unsigned index)
{
    frames::phys_addr a = pte_addr(e, level) + (frames::phys_addr(index) << level_shift(level - 1));
    pte flags = (e & ~addr_mask(false)) & ~pte_hp;
    if (level - 1 > 0) {
        flags |= pte_hp;
    }
    return flags | a;
}

inline void pte_barrier() {} // x64 is TSO so no barrier is needed.

// invlpg runs on one cpu, so invalidating everywhere needs an IPI.
// TODO: change if we support INVLPGB (AMD)
constexpr bool tlb_is_broadcast = false;

}
}

namespace mem {
namespace mapping {

// Widths reported by CPUID.
constexpr uint8_t max_phys_bits = addr_bits;

}
}

#endif /* ARCH_MEM_HW_HH */
