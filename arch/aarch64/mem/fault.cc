/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/debug.h>
#include <osv/irqlock.hh>
#include <osv/kernel_config.h>
#include <osv/mem/mapping.hh>
#include <osv/mem/fault.hh>
#include <osv/sched.hh>
#include <osv/trace.hh>

#include "arch-cpu.hh"
#include "exceptions.hh"

#define ACCESS_FLAG_FAULT(esr)            ((esr & 0b0111100) == 0x08)
#define ACCESS_FLAG_FAULT_WHEN_WRITE(esr) (ACCESS_FLAG_FAULT(esr) && (esr & 0x40))

TRACEPOINT(trace_mmu_vm_access_flag_fault, "addr=%p", void *);

// Set the accessed and dirty bits in the page table entry for the faulting address.
static void handle_access_flag_fault(exception_frame *ef, u64 addr)
{
    trace_mmu_vm_access_flag_fault((void*)addr);

    auto entry = mem::mapping::find(addr);
    if (!entry) {
        return;
    }
    // Only while the entry is still the one that faulted: a page evicted under
    // this fault must not be brought back. Dropping it refaults for real.
    auto e = entry.read();
    while (mem::mapping::pte_present(e)) {
        auto want = mem::mapping::pte_set_accessed(e, true);
        if (ACCESS_FLAG_FAULT_WHEN_WRITE(ef->esr)) {
            want = mem::mapping::pte_set_dirty(want, true);
        }
        if (entry.compare_exchange(e, want)) {
            break;
        }
    }
    mem::mapping::barrier();
}

void page_fault(exception_frame *ef)
{
    sched::fpu_lock fpu;
    SCOPE_LOCK(fpu);
#if CONF_logger_debug
    debug_early_entry("page_fault");
#endif
    u64 addr;
    asm volatile ("mrs %0, far_el1" : "=r"(addr));
#if CONF_logger_debug
    debug_early_u64("faulting address ", (u64)addr);
    debug_early_u64("elr exception ra ", (u64)ef->elr);
#endif

    if (fixup_fault(ef)) {
#if CONF_logger_debug
        debug_early("fixed up with fixup_fault\n");
#endif
        return;
    }

    if (!ef->elr) {
        abort("trying to execute null pointer");
    }

    if (ACCESS_FLAG_FAULT(ef->esr)) {
        return handle_access_flag_fault(ef, addr);
    }

    /* vm_fault might sleep, so check that the thread is preemptable,
     * and that interrupts in the saved pstate are enabled.
     * Then enable interrupts for the vm_fault.
     */
    assert(sched::preemptable());
    assert(!(ef->spsr & processor::daif_i));

    DROP_LOCK(irq_lock) {
        mem::vm_fault(addr, ef);
    }

#if CONF_logger_debug
    debug_early("leaving page_fault()\n");
#endif
}

namespace mem {
namespace mapping {

bool is_page_fault_insn(unsigned int esr) {
    unsigned int ec = esr >> 26;
    return ec == 0x20 || ec == 0x21;
}

bool is_page_fault_write(unsigned int esr) {
    unsigned int ec = esr >> 26;
    return (ec == 0x24 || ec == 0x25) && (esr & 0x40);
}

// Every fault takes the slow path in aarch64, because the hardware does not
// provide a way to distinguish between hard fault and a permission fault.
bool fast_sigsegv_check(uintptr_t addr, exception_frame* ef) {
    return false;
}

}
}
