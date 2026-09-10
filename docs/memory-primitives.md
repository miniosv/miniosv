# The primitive layers

What each function of `vspace`, `frames` and `mapping` does. This document aims to be descriptive enough to enable using these primitives without reading their implementations.

**Note**: We purposefully stray from POSIX naming for these layers to make a distinction between the different types of memory. Virtual memory is reserved/released and physical memory is allocated/freed.

---

# vspace

`#include <osv/mem/vspace.hh>`. Tracks which virtual ranges are reserved.
Nothing here maps memory or costs a frame.

```cpp
struct region_ops { bool (*fault)(region &r, uintptr_t addr, unsigned error); };
struct region     { range span; unsigned perm; const region_ops *ops; };
enum class resa_result { success, no_space, already_reserved };
```

A `region` is caller-owned storage. The layer populates `span` and nothing else:
`perm` is kept for the caller's reference and not enforced, and `ops->fault`
is what `mem::vm_fault()` calls when an address inside `span` faults. The
`region` must outlive its reservation.

The index tracking the regions behind the layer is abstracted behind a generic interface (`core/mem/vspace/index.hh`).
The current implementation includes a maple tree inspired by what Linux uses.

*Note*: Reservations are page-granular. sizes and alignments are rounded up to `frames::page_size` (4 KiB).

| Function | Does |
|---|---|
| `range app_window()` | The range every reservation lands in: `[0x2000_0000_0000, 0x4000_0000_0000)`. |
| `resa_result reserve(region &r, size_t bytes, size_t align)` | Finds a free run of `bytes` bytes at an alignment of `align`, records it in `r.span`. Which address is chosen is up to the index. Returns `success` or `no_space` if `bytes` is 0 or nothing fits. |
| `resa_result reserve_at(region &r, range at)` | Reserves exactly `at`, rounded outward to whole pages. `no_space` if that rounding leaves an empty range, `already_reserved` if any of it is taken. |
| `void release(region &r)` | Frees `r.span`. `r` must be currently reserved. |
| `region *lookup(uintptr_t addr)` | The region containing `addr`, or `nullptr`. Not synchronized: the pointer is only as durable as the caller's own protection against a concurrent `release`. |
| `bool reserved(range r)` | Whether every address of `r` is reserved, by one region or several. |
| `size_t reserved_bytes()` | Total bytes in live reservations. |
| `size_t count()` | Number of live regions. |
| `void for_each(void (*fn)(const region &, void *), void *arg)` | Calls `fn` once per live region, in no particular order. |
| `bool self_check()` | Checks the index's own invariants. O(n); for tests. |

---

# frames

`#include <osv/mem/frames.hh>`. Allocates physical frames. Backed by LLFree
(ATC'23, vendored in `external/llfree`). Addresses are `phys_addr` (`uint64_t`), not pointers;
`no_memory` (0) means none.

```cpp
constexpr size_t page_size       = 4096;
constexpr size_t max_block_bytes = 2 MiB;
```

*Note*: Allocations are page-granular. sizes and alignments are rounded up to `frames::page_size` (4 KiB).

### Allocation

| Function | Does |
|---|---|
| `phys_addr alloc(size_t bytes = page_size, size_t align = page_size)` | A physical region of at least `bytes`, aligned to `max(align, page_size)`. `no_memory` if `bytes` is 0 or nothing is free after reclaim. |
| `void free(phys_addr addr, size_t bytes = page_size)` | Returns a region. `bytes` must be what `alloc()` was given: the same rounding is applied to find the region true size. |

Rounding: `bytes` becomes a whole number of frames; up to one block (512
frames) that count is rounded up to a power of two, above it it is exact. A
3-frame request is served as 4 frames, a 513-frame request as 513.

When `alloc()` finds nothing free it calls `reclaim()` with the request size
and retries for as long as a round gives something back. When it succeeds under pressure it calls
`check_pressure()` before returning, so clients can start giving memory back
before the allocator is empty.

Before `init()`, `alloc()` is served from `boot_alloc()`.

### Accounting

| Function | Does |
|---|---|
| `size_t free_bytes()` | Bytes the allocator could hand out now. |
| `size_t total_available_bytes()` | Bytes the allocator was given at `init()`. Less than `phys_mem_size`, which is what the firmware reported. |
| `size_t phys_mem_size` | Usable RAM as the firmware reported it. Set during arch setup. |

*Note*: `phys_mem_size` is larger than `total_available_bytes` because some memory is already allocated before the allocator is initialized (including for drivers/console/etc).

### Memory pressure

A client that holds memory it could give back may register a watcher, a callback 
that is triggered by the allocator on low `free_bytes()`.

The allocator asks watchers in order of `order`, lowest first, every watcher of one
order before any of the next, and stops as soon as the request is covered
(*i.e.*, the page cache with order 10 is asked before the heap with order 20,
and the heap is not asked at all if the cache gave enough).

| Function | Does |
|---|---|
| `void watch_pressure(pressure_watcher &w, pressure_fn cb, unsigned order)` | Registers `cb`. `w` is caller-owned and must outlive the registration; there is no unregister. |
| `bool under_pressure()` | Whether `free_bytes()` is below `conf_memory_pressure_percent` of the total (Makefile parameter, default 10%). |
| `void check_pressure()` | If under pressure, calls `reclaim()` for what it takes to get back above the threshold. |
| `bool reclaim(size_t bytes)` | Asks the watchers, lowest order first, until `free_bytes()` has grown by `bytes` or every watcher has been asked once. Returns whether any reported giving memory back. Runs one caller at a time; a call from inside a watcher returns false. |

A watcher may call `alloc()` and `free()`.

---

# mapping

`#include <osv/mem/mapping.hh>`. Reads and writes the page tables. 
The page table architecture and format are supplied via the arch-specific headers (`arch/<arch>/mem/hw.hh`).

```cpp
constexpr size_t page_size      = 4 KiB;
constexpr size_t huge_page_size = 2 MiB;
constexpr size_t flush_batch    = 32;
extern uint8_t phys_bits, virt_bits;   // what the hardware reports
```

Every function taking a `range` first widens it to whole pages.

`pte_ref` is a struct used to hold a pointer to a leaf in the page table. It can be both last level (4KiB, Page Table Entry) or one-before-last level (2MiB, Page Directory Entry). Most clients and applications don't need to operate at this level so we omit it in this document.

### Translation

Read-only; nothing here changes an entry.

| Function | Does |
|---|---|
| `pte_ref find(uintptr_t addr)` | The leaf mapping `addr`, or an empty `pte_ref`. Valid until the tables under it change. |
| `phys_addr to_phys(uintptr_t addr)`, `to_phys(void *)` | The physical address `addr` maps to, or `no_memory`. |
| `bool is_contiguous(const void *addr, size_t bytes)` | Whether `[addr, addr + bytes)` maps one run of physical memory. |

### Building

`prepare` builds the tables down to a slot, `attach` maps frames the caller
holds, `populate` allocates them too. All refuse an already-mapped range,
except `attach_missing`.

| Function | Does |
|---|---|
| `pte_ref prepare(uintptr_t addr, size_t leaf_size = page_size)` | Builds the tables down to the level that holds a leaf of `leaf_size` and returns that slot, so the caller can install the entry with one `write()`. |
| `bool prepare(range r, size_t leaf_size = page_size)` | The same over a range. |
| `bool attach(range r, phys_addr phys, unsigned perm, mattr ma = normal)` | Maps `r` onto physical memory starting at `phys`, choosing 2 MiB leaves where both sides allow. `r` and `phys` must be page-aligned. False, and nothing changes, if any of `r` is already mapped. |
| `bool attach_missing(range r, phys_addr phys, unsigned perm, size_t slop = page_size, mattr ma = normal)` | Like `attach`, but skips what is already mapped. For callers whose ranges overlap by design and who guarantee the physical memory either side is theirs and contiguous: `map_phys()`, the ACPI driver. |
| `bool populate(range r, unsigned perm, size_t leaf_size = page_size, bool zero = true)` | Allocates a frame per leaf and maps it. False if any of `r` was mapped or a frame could not be had; leaves installed before the failure stay, and `depopulate()` takes them back. |

### Removing

`detach` clears entries, `depopulate` frees the frames too, `protect` changes
permissions in place. `detach_deferred` clears the entries and accumulate stale TLB entries.

| Function | Does |
|---|---|
| `void detach(range r)` | Clears the entries and flushes the TLB for `r` on every cpu. Frames are not freed. |
| `void detach_deferred(range r, pending_invalidation &stale)` | Same as `detach` but avoid TLB flushes. Accumulate stale TLB entries at the tail of `stale` (must be owned by the caller). |
| `void depopulate(range r)` | Detaches and frees the frames, in batches of `flush_batch`, flushing before each batch is freed. |
| `void protect(range r, unsigned perm)` | Rewrites the permissions of every mapped entry in `r`. Flushes only the entries whose change the arch says needs it. |
| `void split(range r)` | Breaks every 2 MiB leaf in `r` into 4 KiB leaves. |

### TLB and ordering

The flushes the functions above issue, for a caller that wrote entries itself.

| Function | Does |
|---|---|
| `void flush_local(range r)` | Flushes `r` on this cpu. |
| `void flush_range(range r)` | Flushes `r` on every cpu. |
| `void flush_all()` | Flushes everything on every cpu and advances the epoch. |
| `uint64_t flush_epoch()` | Completed global flushes so far. |
| `void barrier()` | Makes entries the caller wrote visible to the page-table walker. Needed after writing through a `pte_ref`. |

### Deferred invalidation

Clearing an entry leaves stale copies in the TLBs. `pending_invalidation`
collects cleared addresses to flush them in one go.

```cpp
struct pending_invalidation {
    uintptr_t va[flush_batch]; 
    unsigned count;
    bool all; // list is full
    uint64_t epoch; // when the TLB entries became stale
    void add(uintptr_t addr);
    void invalidate();
};
```

`add()` records an address, or sets `all` if past `flush_batch`.

`invalidate()` ensures that the virtual memory do not have stale TLB entries in any core.
This can happen in two cases:
1. `invalidate()` actually flushes them on every cpu using an IPI (expensive).
2. a global flush of the TLBs on all cores happened after `epoch`, in which case the entries are already gone and it does nothing. *Note*: the current epoch (available via `flush_epoch()`) must be strictly superior to `epoch`+1 to avoid TLB invalidation.


### Accessed and dirty bits

Set by the hardware on the entries it uses; read and reset here.

| Function | Does |
|---|---|
| `bool accessed(range r)`, `bool dirty(range r)` | Whether any entry in `r` has the bit set. |
| `void clear_accessed(range r)`, `void clear_dirty(range r)` | Clears the bit on every entry, flushing. |
| `void clear_dirty(range r, pending_invalidation &stale)` | The same, with the flush deferred. |

---

# Misc

Other memory subsystem related functions that do not belong into one primitive.

| Function | Does |
|---|---|
| `void *mem::map_phys(phys_addr pa, size_t bytes, mattr ma = normal)` | A linear-map pointer to physical `[pa, pa + bytes)`, mapping it read-write if it is not already. Mapping the same frames twice is allowed. Never unmapped. |
| `void mem::map_phys_at(void *virt, phys_addr pa, size_t bytes, size_t slop = 4096, mattr ma = normal)` | The same at a caller-chosen virtual address, outside the linear map. For drivers. |
| `void mem::vm_fault(uintptr_t addr, exception_frame *ef)` | Finds the region owning `addr` and calls its `ops->fault`. Raises `SIGSEGV` if there is none or the handler returns false. |
