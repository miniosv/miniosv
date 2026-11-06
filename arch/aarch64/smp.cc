/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <smp.hh>
#include <osv/debug.h>
#include <osv/sched.hh>
#include <osv/prio.hh>
#include <osv/aligned_new.hh>
#include <osv/export.h>
#include <osv/kernel_config.h>
#include "processor.hh"
#include "psci.hh"
#include "drivers/acpi.hh"

extern "C" { /* see boot.S */
    extern init_stack *smp_stack_free;
    extern u64 start_secondary_cpu();
}

OSV_LIBSOLARIS_API
volatile unsigned smp_processors = 1;

sched::cpu* smp_initial_find_current_cpu()
{
    for (auto c : sched::cpus) {
        if (c->arch.mpid == processor::read_mpidr())
            return c;
    }
    abort();
}

void secondary_bringup(sched::cpu* c)
{
    __sync_fetch_and_add(&smp_processors, 1);
    c->idle_thread->start();
    c->on_cpu_up();  
}

void smp_init()
{
    // Enumerate CPUs from the ACPI MADT GIC CPU Interface (GICC) entries, the
    // same way arch/x64/smp.cc walks the MADT for local APICs.
    auto madt = reinterpret_cast<const acpi::madt*>(acpi::find_table(ACPI_SIG_MADT));
    if (!madt) {
        abort("smp_init: no MADT table - cannot enumerate CPUs.\n");
    }

    auto subtable = reinterpret_cast<const char*>(madt + 1);
    auto madt_end = reinterpret_cast<const char*>(madt) + madt->header.length;
    int nr_cpus = 0;
    while (subtable < madt_end) {
        auto s = reinterpret_cast<const acpi::madt_subtable*>(subtable);
        if (s->type == acpi::MADT_GICC) {
            auto gicc = reinterpret_cast<const acpi::madt_gicc*>(s);
            if (gicc->flags & acpi::MADT_ENABLED) {
                auto c = new sched::cpu(nr_cpus);
                c->arch.mpid = gicc->mpidr;
                c->arch.smp_idx = nr_cpus;
                c->arch.initstack.next = smp_stack_free;  /* setup thread stack */
                smp_stack_free = &c->arch.initstack;
                sched::cpus.push_back(c);
                nr_cpus++;
            }
        }
        subtable += s->length;
    }
    if (nr_cpus < 1) {
        abort("smp_init: no enabled CPUs in MADT.\n");
    }
    debugf("%d CPUs detected\n", nr_cpus);
    sched::current_cpu = sched::cpus[0];

    for (auto c : sched::cpus) {
        c->incoming_wakeups = aligned_array_new<sched::cpu::incoming_wakeup_queue>(sched::cpus.size());
    }
}

void smp_launch()
{
#if CONF_logger_debug
    debug_early_entry("smp_launch");
#endif
    for (auto c : sched::cpus) {
        auto name = std::string("balancer") + std::to_string(c->id);
        if (c->arch.smp_idx == 0) {
            sched::thread::current()->_detached_state->_cpu = c;
            // c->init_on_cpu() already done in main().
            c->init_idle_thread();
            c->idle_thread->start();
            c->on_cpu_up(); 
            continue;
        }
        sched::thread::attr attr;
        attr.stack(81920).pin(c).name(name);
        c->init_idle_thread();
        c->bringup_thread = new sched::thread([=] { secondary_bringup(c); }, attr, true);
        psci::_psci.cpu_on(c->arch.mpid,
                mmu::virt_to_phys(reinterpret_cast<void *>(start_secondary_cpu)));
    }
    while (smp_processors != sched::cpus.size())
        barrier();
}

void smp_main()
{
#if CONF_logger_debug
    debug_early_entry("smp_main");
#endif
    sched::cpu* cpu = smp_initial_find_current_cpu();
    assert(cpu);
    cpu->init_on_cpu();
    cpu->bringup_thread->_detached_state->_cpu = cpu;
    cpu->bringup_thread->switch_to_first();
}

static inter_processor_interrupt smp_stop_cpu_ipi { IPI_SMP_STOP, [] {
    while (true) {
        arch::halt_no_interrupts();
    }
}};

void smp_crash_other_processors()
{
    smp_stop_cpu_ipi.send_allbutself();
}
