# The heap allocator and the page cache

The two in-tree clients of the primitive layers. See
[memory-primitives.md](memory-primitives.md) for what they are built on.

---

# heap

`#include <osv/mem/heap.hh>`. Bump allocator inspired by BareHeap[DaMoN'26].

The general-purpose allocator behind `malloc`, `new` and `aligned_alloc`.

| Function | Does |
|---|---|
| `void init()` | Reserves the window and the page descriptors. Registers with `frames` for memory pressure. |
| `bool ready()` | Whether `init()` has run. |
| `bool takes(size_t bytes, size_t alignment)` | Whether `alloc()` can serve this: `alignment` at most 2 MiB. Any size. |
| `void *alloc(size_t bytes, size_t alignment)` | An object of at least `bytes`, aligned to at least `alignment`. `nullptr` if memory ran out. |
| `void free(void *p)`, `void free(void *p, size_t bytes)` | Returns an object. |
| `size_t size_of(void *p)` | The bytes an object actually has: its size class, or for a large object what was asked for. |
| `bool owns(void *p)` | Whether `alloc()` handed out `p`. |

## Layout

One `vspace` reservation of 512 GiB, carved into 2 MiB pages. Each page has a
64-byte descriptor holding how many objects it still has live, the size of
those objects, and the frame behind it. A page holds objects of one size.

### Paths

`alloc()` takes one of two paths.

**Paged.** For `bytes <= 1 MiB` when `alignment <= 2048`, or for anything
where `max(bytes, alignment) <= 8 KiB`. The request is rounded up to a size
class: powers of two from 16 bytes to 8 KiB, then four classes per power of two
up to 1 MiB. Each cpu keeps a bump cursor per class into a page it owns;
allocation is a decrement of that cursor, and only refilling it takes a lock.
`free()` decrements the page's live count and returns the page to the ready
list when it reaches zero.

**Large.** Everything else. The object gets a `vspace` reservation of its own,
populated with 2 MiB leaves once `bytes >= 2 MiB`, 4 KiB ones below. The
memory is not zeroed. A one-frame record beside the reservation holds what was
asked for.

### Memory pressure

Empty pages stay mapped on a ready list so they can be reused without a page
walk. When `frames` reports pressure, the heap's watcher unmaps up to 16 of
them per call and returns their frames.

## Use

Call `malloc`. Direct calls to `heap::alloc()` are for callers that need to
know the object is a heap object, such as `size_of()` users.

---

# Page Cache

`#include <osv/mem/pagecache.hh>`. Memory-mapped cache inspired by uCache[FAST'26].

Maps a backing store into virtual memory and pages it in on demand: a fault on an address inside the mapping reads the corresponding part of the store into a frame and maps it. Eviction writes dirty frames back.

## The store

Anything mapped must implement `mem::store` (`include/osv/mem/store.hh`):

```cpp
struct io;   // one transfer in flight; opaque to the cache

struct store {
    virtual ~store() = default;

    virtual uint64_t size() = 0;                  // bytes in the store, non-zero
    virtual size_t granularity() { return 4096; } // transfer unit, at most 4096

    // Start a transfer and return its handle.
    virtual io *read(void *buf, uint64_t offset, size_t bytes) = 0;
    virtual io *write(const void *buf, uint64_t offset, size_t bytes) = 0;
    virtual bool done(io *req) = 0;     // has it finished
    virtual int64_t wait(io *req) = 0;  // block until it has: bytes moved, or -errno

    // Completion callback. Optional; the default says it is not supported.
    virtual bool subscribe(io *, void (*fn)(void *), void *arg) { return false; }

    // Synchronous helpers over read/write and wait.
    int64_t read_now(void *buf, uint64_t offset, size_t bytes);
    int64_t write_now(const void *buf, uint64_t offset, size_t bytes);
};
```

Two implementations exist in `modules/miniext`: `file_store`, a file in the
ext4 image, and `raw_store`, a raw NVMe namespace.

## Functions

| Function | Does |
|---|---|
| `void *map(store &s, const policy &p = defaults(), size_t limit = 0)` | Reserves `s.size()` rounded up to a page, 2 MiB aligned, and returns its start. `s` and `p` are kept by reference and must outlive the mapping. `limit` is the most frames the mapping may hold; 0 for no cap of its own. `nullptr` if the store is empty, its granularity is over 4096, or the reservation fails. |
| `void unmap(void *addr)` | Writes back every dirty buffer, evicts everything, releases the reservation. |
| `int64_t sync(void *addr, size_t bytes)` | Writes back every dirty buffer overlapping the range. Returns bytes written, or `-EINVAL` if `addr` is not in a mapping. |
| `bool resident(void *addr)` | Whether the page holding `addr` is mapped right now. |
| `size_t fetch(void *addr, size_t bytes)` | Brings in every buffer overlapping the range that is not resident. Returns how many bytes of the range are resident afterwards. |
| `void stats_dump()`, `void stats_reset()` | Print and clear the counters. Compiled in with `conf_pagecache_stats=1`. |

Reads and writes through the returned pointer are ordinary loads and stores.
Addresses past `s.size()` but inside the page rounding fault to the last byte;
addresses past the mapping raise `SIGSEGV`.

## Capabilities

The page cache offers several notable guarantees compared to POSIX `mmap`:
- Buffers may take any size, and each buffer size in a region may be different. The cache guarantees that operations happen at the buffer granularity (from one frame up to 1 GiB). Per-buffer size is supplied by a customizable policy (4KiB buffer by default).
- A cache region can have its own memory limit and custom policies.
- Passive prefetching happens at fault-time, guided by a policy. A prefetched buffer is installed later without a read.
- The cache tries to use as many big (2 MiB) frames where a buffer allows it. 
- Counters and fault timings, compiled in on demand.

## A fault

1. The policy's `fault_extent()` names the tile containing the offset: a
   start and a length, between 4 KiB and 1 GiB, and a pure function of the
   offset so every cpu computes the same tile. Neither needs to be page
   aligned; a page two tiles share is held by whichever came first. The
   default is the one 4 KiB page.
2. `make_room()` evicts within the mapping's `limit`, and runs one pass over
   every mapping if `frames` reports pressure.
3. Frames are allocated for the tile, as 2 MiB blocks where the tile allows,
   and the reads are started. A read past the end of the store zero-fills.
4. Another cpu faulting the same tile spins until the entries are published.
   Prefetches the policy asked for are started in the same batch and parked;
   a later fault on one installs it without a read.
5. The reads are waited for and the entries installed.

A fault that cannot get a frame fails, and the access raises `SIGSEGV`.

## `limit`

With a limit, the mapping evicts its own buffers to stay under it, and a fault
never fails for lack of memory as long as something can be evicted. With
`limit` 0 the mapping only gives memory back when `frames` reports pressure,
a fault that then cannot allocate fails. Set a limit for any store larger 
than the memory you can spare for it.

## Policies

A `policy` (`pagecache.hh`) decides tile size, eviction order, prefetch and
dirtiness:

| Field | Does |
|---|---|
| `bytes_per_buffer` | Private bytes the cache keeps beside each buffer, reached through `policy_data()`. |
| `create(store_size)`, `destroy(state)` | Called at `map()` and `unmap()`. The only two points a policy may allocate. `state` is passed to every other call. |
| `fault_extent(state, off, &start, &len)` | The tile containing `off`. Nullptr for one page. |
| `prefetch(state, b, also)`, `prefetch_depth` | Offsets to read in the same batch as `b`; how many buffers ahead. Nullptr and 0 for none. |
| `evict(state, bytes, victims)` | Fill `victims` with at least `bytes` worth of buffers to take. `victims` is fixed-capacity, at most 64. |
| `on_fault(state, b)`, `on_evicted(state, b)` | Called when a buffer is installed and when it is taken. |
| `is_dirty(state, b)` | Whether `b` must be written back before eviction. Nullptr to use the hardware dirty bit. |

The helpers `offset()`, `size()`, `data()`, `accessed()`, `clear_accessed()`,
`dirty()`, `prefetched()` and `clear_prefetched()` read a buffer's state.

The eviction policy default uses S3-FIFO[SOSP'23].

## Use

```cpp
mem::store *s = ...;                                   // a file_store or raw_store
void *base = mem::pagecache::map(*s, mem::pagecache::defaults(), 256 << 20);
auto *bytes = static_cast<uint8_t *>(base);
bytes[offset] = 1;                                     // faults the tile in
mem::pagecache::sync(base, s->size());                 // write back
mem::pagecache::unmap(base);
```

## Statistics

With `conf_pagecache_stats=1`, `stats_dump()` reports per-fault time split
into claiming frames, waiting for the device, evicting, spinning on another
cpu's load and adopting a parked prefetch, plus counts of faults, hits, bytes
read, evictions, prefetches and their outcomes, and faults that got no memory.
