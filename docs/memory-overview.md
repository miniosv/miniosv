# The memory subsystem

miniOSv runs one application in one address space. Memory is managed by three
primitive layers, each responsible for exactly one thing. 
We also provide two "clients" of these layers, the heap allocator and the page cache
that provide higher-level functionalities built upon the memory primitives.
*Note*: we call clients any module/piece of software that uses the memory subsystem primitives

| Layer | Manages | Header |
|---|---|---|
| `vspace` | which virtual ranges are reserved | `include/osv/mem/vspace.hh` |
| `frames` | which physical frames are free | `include/osv/mem/frames.hh` |
| `mapping` | the page table | `include/osv/mem/mapping.hh` |

The interface rationale is that reserving virtual memory, allocating a frame and 
mapping one onto the other are three independent steps. Our interface provides applications
that want to precisely manage their memory minimal and predictable primitives.

```
                  +-------------+
                  | application |
                  +------+------+
                         |
             +-----------+-----------+
             v                       v
       +-----------+         +-----------+      +-------+
       |   heap    |         | pagecache |<---->| store |
       +-----+-----+         +-----+-----+      +-------+
             |                     |
             +----------+----------+
                        |
          +-------------+-------------+
          v             v             v
    +----------+   +----------+   +----------+
    |  vspace  |   | mapping  |-->|  frames  |
    +----------+   +----------+   +----------+
```

Each layer is described in [memory-primitives.md](memory-primitives.md).
The heap and the page cache are described in [memory-clients.md](memory-clients.md).
The tests live in `test/os-memory-primitives.cc` and `test/os-memory.cc`.

## Address space

| Range | Use |
|---|---|
| `[0x2000_0000_0000, 0x4000_0000_0000)` | `vspace::app_window()`: every reservation lands here |
| `[0x4000_0000_0000, 0x4000_0000_0000 + 2^44)` | the linear map: frame at physical `p` is at `mem::linear + p` |

The linear map is internal to the memory subsystem (`core/mem/linear.hh`).
Drivers reach physical memory through `mem::map_phys()`; everything else uses
the heap.

## Units

| Term | Meaning |
|---|---|
| `range` | virtual addresses `[start, end)`, end excluded |
| `phys_addr` | a `uint64_t` physical address; `0` means none |
| frame | 4 KiB of physical memory |
| block | 2 MiB of contiguous frames, the largest unit `frames` allocates as one |
| leaf | a page-table entry that maps memory (4 KiB or 2 MiB) rather than a table |
| `perm` | `perm_read`, `perm_write`, `perm_exec`; the hardware bits |
| `mattr` | `normal` or `dev` memory; only aarch64 distinguishes them |

## Boot order

1. `frames::add_region()` collects usable RAM; `frames::boot_alloc()` and
   `early::alloc()` serve allocations until an allocator exists.
2. `frames::init()` builds the frame allocator from what is still unused.
3. `heap::init()` reserves the heap window; `malloc` switches from `early` to
   `heap` once `heap::ready()`.
4. `frames::enable_percpu()` once the scheduler runs on every cpu; until then
   allocation uses core 0.

## Clients

| Client | Header | Built on |
|---|---|---|
| `heap` | `mem/heap.hh` | vspace, frames, mapping |
| `early` | `mem/early.hh` | frames, before `heap` exists |
| `pagecache` | `mem/pagecache.hh` | all three, plus a `mem::store` |
| `phys` | `mem/phys.hh` | mapping, frames: permanent maps for drivers |
| `fault` | `mem/fault.hh` | vspace: routes a fault to the owning region |

`heap` and `early` share one interface (`takes`, `alloc`, `free`, `size_of`,
`owns`), so the libc `malloc` treats them alike.

## libc support

The memory subsystem integrates with the standard libc/libc++ to facilitate the porting effort. 
`malloc`/`calloc`/`new` and other memory allocation functions are routed to the heap allocator.
`mmap` only supports anonymous memory mappings (with `fd` = -1). It also eagerly allocates the frames for the virtual memory region and maps them at creation time.
