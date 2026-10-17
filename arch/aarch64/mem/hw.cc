/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * aarch64 specific memory operations and formats.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mem/mapping.hh>

namespace mem {
namespace mapping {

bool tracks_writes;

// Read before the first mapping is made. boot.S has already put TCR_EL1 in
// step with the same register, on every cpu.
void detect_hw_dirty()
{
    uint64_t mmfr1;
    asm volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
    tracks_writes = (mmfr1 & 0xf) >= 2;
}

// Pseudo-entries at the root, holding the tables pointed by TTBR0 and TTBR1
// Bit 63 of an address picks between them; everything this kernel maps is
// in the low half, the second is only used during boot.
static std::atomic<pte> page_table_root[2];

std::atomic<pte> *root_slot(uintptr_t va)
{
    return &page_table_root[va >> 63];
}

void tlb_flush_page(uintptr_t va)
{
    asm volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
                 :: "r"(va >> 12) : "memory");
}

void tlb_flush_pages(const uintptr_t *va, size_t count)
{
    if (!count) {
        return;
    }
    asm volatile("dsb ishst" ::: "memory");
    for (size_t i = 0; i < count; i++) {
        asm volatile("tlbi vaae1is, %0" :: "r"(va[i] >> 12) : "memory");
    }
    asm volatile("dsb ish; isb" ::: "memory");
    // One barrier pair for the whole batch rather than one per address
}

void tlb_flush_local()
{
    asm volatile("dsb sy; tlbi vmalle1; dsb sy; isb" ::: "memory");
}

void tlb_flush_all()
{
    asm volatile("dsb sy; tlbi vmalle1is; dsb sy; isb" ::: "memory");
}

void tlb_flush_pages_all(const uintptr_t *va, size_t count)
{
    tlb_flush_pages(va, count);
}

}
}

extern "C" { /* see boot.S */
    extern u64 smpboot_ttbr0;
    extern u64 smpboot_ttbr1;
}

namespace mem {
namespace frames {

u64 ram_base;

}

}

namespace mem {
namespace mapping {

void switch_to_runtime_page_tables()
{
    auto low = root_slot(0)->load(std::memory_order_acquire);
    auto high = root_slot(~uintptr_t(0))->load(std::memory_order_acquire);
    auto pt_ttbr0 = smpboot_ttbr0 = pte_table_addr(low);
    auto pt_ttbr1 = smpboot_ttbr1 = pte_table_addr(high);
    asm volatile("msr ttbr0_el1, %0; isb;" ::"r" (pt_ttbr0));
    asm volatile("msr ttbr1_el1, %0; isb;" ::"r" (pt_ttbr1));
    flush_all();
}

}
}
