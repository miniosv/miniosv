/*
 * Physical frame allocation.
 * Backed by LLFree[ATC'23] (external/llfree).
 */

#ifndef OSV_MEM_FRAMES_HH
#define OSV_MEM_FRAMES_HH

#include <cstddef>
#include <cstdint>

#include <osv/mem/types.hh>

namespace mem {
namespace frames {

constexpr size_t page_size = 4096;
constexpr size_t max_block_bytes = page_size << 9;   // 2 MiB

/*
* Allocates a physical region of at least `bytes` bytes, aligned to `align`.
* Returns the physical address of the first byte, or no_memory if not enough free space.
*
* bytes is rounded up: to a whole block when it fits in one, to whole frames
* otherwise.
* free() repeats that calculation, so it must be given the size that
* was asked for.
*/
phys_addr alloc(size_t bytes = page_size, size_t align = page_size);
void free(phys_addr addr, size_t bytes = page_size);

size_t free_bytes();

// What the allocator holds (less than the RAM the firmware reported).
size_t total_available_bytes();

// Total usable RAM as the firmware reported it. Set during arch setup.
extern size_t phys_mem_size;

// Clients that hold memory they could give back.
using pressure_fn = bool (*)();

struct pressure_watcher {
    pressure_fn fn = nullptr;
    unsigned order = 0;
    pressure_watcher *next = nullptr;
};

// Simple policy to ask the cache to relinquish memory first
constexpr unsigned pressure_cache = 10;
constexpr unsigned pressure_heap = 20;

void watch_pressure(pressure_watcher &w, pressure_fn cb, unsigned order);

bool under_pressure();
void check_pressure();
// Ask the watchers, lowest order first, until free memory has grown by "bytes"
// or all have been asked. True if any gave something.
bool reclaim(size_t bytes);

// Boot. add_region() collects memory before there is an allocator to put it in;
// init() builds llfree and hands it everything still unused.
void add_region(phys_addr base, size_t bytes);
void init(size_t cores);
// Call once the scheduler runs everywhere: until then allocation uses core 0.
void enable_percpu();
bool ready();

// Pre-init allocation, from the regions collected by add_region().
void *boot_alloc(size_t bytes, size_t align, size_t offset = 0);
void *boot_alloc_page();
void boot_free_page(void *addr);

} // namespace frames
} // namespace mem

#endif
