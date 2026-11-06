/*
 * Page table mapping primitives.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_MAPPING_HH
#define OSV_MEM_MAPPING_HH

#include <osv/mem/types.hh>
#include <mem.hh>

namespace mem {
namespace mapping {

constexpr size_t page_size = level_size(0);
constexpr size_t huge_page_size = level_size(1);
constexpr unsigned page_size_shift = level_shift(0);

// Widths the hardware reports, for whoever has to build an address by hand.
extern uint8_t phys_bits, virt_bits;

constexpr unsigned level_of(size_t leaf_size)
{
    return leaf_size >= huge_page_size ? 1 : 0;
}

constexpr size_t flush_batch = 32;

// Pointer to a PTE with additional metadata for management.
class pte_ref {
public:
    pte_ref() = default;
    pte_ref(std::atomic<pte> *slot, unsigned level) : _slot(slot), _level(level) {}

    explicit operator bool() const { return _slot; }
    unsigned level() const { return _level; }
    size_t size() const { return level_size(_level); }

    pte read() const { return _slot->load(std::memory_order_relaxed); }
    void write(pte e) { _slot->store(e, std::memory_order_release); }
    pte exchange(pte e) { return _slot->exchange(e, std::memory_order_acq_rel); }
    bool compare_exchange(pte &expected, pte e)
    {
        return _slot->compare_exchange_strong(expected, e, std::memory_order_acq_rel);
    }

    bool empty() const { return pte_empty(read()); }
    bool present() const { return pte_present(read()); }
    frames::phys_addr addr() const { return pte_addr(read(), _level); }
    unsigned perm() const { return pte_perm(read()); }

    // The entry this slot needs to map that frame.
    pte leaf_for(frames::phys_addr p, unsigned perm, mattr ma = mattr::normal) const
    {
        return pte_make_leaf(p, perm, _level, ma);
    }

private:
    std::atomic<pte> *_slot = nullptr;
    unsigned _level = 0;
};

// Find the leaf (PTE or PDE) for "addr", if any.
pte_ref find(uintptr_t addr);

// The physical address "addr" resolves to, or no_memory if nothing is mapped
frames::phys_addr to_phys(uintptr_t addr);
frames::phys_addr to_phys(void *addr);

// Whether [addr, addr + bytes) is one run of physical memory
bool is_contiguous(const void *addr, size_t bytes);

// Populate the table down to the level that can hold a leaf of "leaf_size",
// and returns a pointer to that leaf.
pte_ref prepare(uintptr_t addr, size_t leaf_size = page_size);
bool prepare(range r, size_t leaf_size = page_size);

// Attach physical memory to a virtual range.
// Assumes range and physical address are aligned to page_size.
// Returns false if the range is already mapped.
bool attach(range r, frames::phys_addr phys, unsigned perm, mattr ma = mattr::normal);

// Attach physical memory to whatever part of the range is not attached yet.
// Only used by callers whose ranges overlap by design, and who guarantee that
// the physical memory either side is their own and contiguous (ACPI driver).
bool attach_missing(range r, frames::phys_addr phys, unsigned perm, size_t slop = page_size,
                    mattr ma = mattr::normal);

/*
 * Virtual address that have been cleared but that may be stale in a TLB.
 * invalidate() flushes the TLB.
 *
 * The list has flush_batch entries at max.
 * "epoch" is flush_epoch() as of the moment the last of these entries was
 * cleared; invalidate() returns directly if a global flush has begun and
 * finished since. Leaving it alone means flushing.
 */
constexpr uint64_t never_flushed = ~uint64_t(0);

struct pending_invalidation {
    uintptr_t va[flush_batch];
    unsigned count = 0;
    bool all = false;
    uint64_t epoch = never_flushed;

    void add(uintptr_t addr);
    void invalidate();
};

// Detach physical memory from a virtual range.
void detach(range r); // Flushes the TLB for the range.

// Defers the flush to the caller, recording in "stale" the addresses that need
// it and the epoch they were cleared at.
// Stale entries are added to, so the caller can accumulate multiple detach_deferred() calls before flushing.
void detach_deferred(range r, pending_invalidation &stale);

// Apply new permissions to a virtual range. Flushes the TLB if any permissions changed (arch-specific).
void protect(range r, unsigned perm);

// Helpers (arch-agnostic)
bool accessed(range r);
bool dirty(range r);
void clear_accessed(range r);
void clear_dirty(range r);
void clear_dirty(range r, pending_invalidation &stale);

// Break every large leaf in "r" into leaves of the smallest size.
void split(range r);

void flush_local(range r);
void flush_range(range r);
void flush_all();

// Counts completed global invalidations. The currency of detach_deferred().
uint64_t flush_epoch();

// Make entries the caller wrote itself visible to the page-table walker.
inline void barrier() { pte_barrier(); }

// Helpers for allocating physical memory and mapping it to the virtual range in a single call.
// Returns false if the allocation failed or the range is already mapped.
bool populate(range r, unsigned perm, size_t leaf_size = page_size, bool zero = true);
// Unmap and free physical memory for the virtual range. Flushes the TLB.
void depopulate(range r);

}
}

#endif /* OSV_MEM_MAPPING_HH */
