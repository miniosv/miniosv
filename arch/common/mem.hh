/*
 * The memory operations every architecture provides.
 *
 * The entry format, the walk geometry and the boot layout differ and are in
 * <mem/hw.hh>, which this includes; the declarations below are the same
 * whichever one that turns out to be, so the generic code has one set of names
 * to call.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_COMMON_MEM_HH
#define ARCH_COMMON_MEM_HH

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include <mem/hw.hh>

struct exception_frame;

namespace mem {
namespace mapping {

// returns a pointer to the root table entry for "va"
std::atomic<pte> *root_slot(uintptr_t va);

// Invalidate on the cpu that runs this.
void tlb_flush_page(uintptr_t va);
void tlb_flush_pages(const uintptr_t *va, size_t count);
void tlb_flush_local();

// Invalidate wherever the translation could be cached. Where invalidation is
// already broadcast (tlb_is_broadcast) these are the ones above; where it is
// not, each costs an IPI to every other cpu and a wait for it to answer.
void tlb_flush_all();
void tlb_flush_pages_all(const uintptr_t *va, size_t count);

// Index into the table at "level" for "va".
inline unsigned pt_index(void *va, unsigned level)
{
    return (reinterpret_cast<uintptr_t>(va) >> level_shift(level)) &
           (entries_per_table - 1);
}

// Leave the boot tables behind for the ones the kernel built.
void switch_to_runtime_page_tables();

#ifdef __aarch64__
// Settle tracks_writes from the cpu, before anything is mapped.
void detect_hw_dirty();
#endif

// Fault handling helpers.
bool is_page_fault_insn(unsigned int err);
bool is_page_fault_write(unsigned int err);
bool fast_sigsegv_check(uintptr_t addr, exception_frame *ef);

}
}

namespace mem {
namespace frames {

// Where the kernel ELF image is loaded.
extern void *elf_phys_start;

}
}

#endif /* ARCH_COMMON_MEM_HH */
