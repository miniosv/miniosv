/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "core/mem/heap/histogram.hh"
#include <osv/drivers_config.h>
#include <osv/power.hh>
#include <osv/debug.hh>
#include <smp.hh>
#include <processor.hh>
#include <arch.hh>

#if CONF_drivers_acpi
#include <drivers/acpi.hh>
#endif

namespace osv {

void halt(void)
{
    smp_crash_other_processors();
    while (true) {
        arch::halt_no_interrupts();
    }
}

void poweroff(void)
{
    mem::heap::histogram_dump();

#if CONF_drivers_acpi
    // Power off cleanly via ACPI S5 when the firmware describes it. This does
    // not return on success.
    acpi::power_off();
#endif
    // On hypervisors that do not support ACPI like firecracker we resort to a
    // reset using the 8042 PS/2 Controller ("keyboard controller") as a way to
    // shutdown the VM.
    processor::outb(0xfe, 0x64);
    // If the above did not work which would be the case on qemu microvm,
    // then cause triple fault by loading a broken IDT and triggering an interrupt.
    processor::lidt(processor::desc_ptr(0, 0));
    __asm__ __volatile__("int3");

    // We shouldn't get here on x86.
    halt();
}

static void pci_reboot(void) {
    u8 v = processor::inb(0x0cf9) & ~6;
    processor::outb(v|2, 0x0cf9); // request hard reset
    usleep(50);
    processor::outb(v|6, 0x0cf9); // actually do the reset
    usleep(50);
}

static void kbd_reboot(void) {
    while (processor::inb(0x64) & 0x02); // clear all keyboard buffers
    processor::outb(0xfe, 0x64);
    usleep(50);
}

void reboot(void)
{
    // Method 1: "fast reset" via System Control Port A (port 0x92)
    processor::outb(1, 0x92);
    // Method 2: Reset using the 8042 PS/2 Controller ("keyboard controller")
    kbd_reboot();
    // Method 3: PCI reboot
    pci_reboot();
    // Method 4: Cause triple fault by loading a broken IDT and triggering an
    // interrupt.
    processor::lidt(processor::desc_ptr(0, 0));
    __asm__ __volatile__("int3");
    // If we're still here, none of the above methods worked...
}

} /* namespace osv */
