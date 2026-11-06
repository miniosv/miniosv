/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
#include "arch.hh"
#include "arch-cpu.hh"
#include "arch-setup.hh"
#include <osv/mem/frames.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/phys.hh>
#include "processor.hh"
#include "processor-flags.h"
#include "msr.hh"
#include <osv/elf.hh>
#include <osv/types.h>
#include <alloca.h>
#include <string.h>
#include <osv/boot.hh>
#include "dmi.hh"
#if CONF_drivers_acpi
#include "drivers/acpi.hh"
#endif

// Physical pointer to the hand-off structure filled by the UEFI stub; stored
// by start64 in boot.S.
osv::boot_info* osv_boot_info;

// Wall-clock time (ns since the Unix epoch) at boot, from the UEFI firmware's
// GetTime() captured by the stub. Used as the wall-clock base by time sources
// that have no paravirtual wall clock of their own (e.g. the Hyper-V clock);
// 0 when the firmware did not report a time. Mirrors the aarch64 global.
u64 osv_boot_unixtime_ns;

void setup_temporary_phys_map()
{
    // duplicate 1:1 mapping into phys_mem
    u64 cr3 = processor::read_cr3();
    auto pt = reinterpret_cast<u64*>(cr3);
    pt[mem::mapping::pt_index(mem::linear, 3)] = pt[0];
}

// A copy of the UEFI memory map taken before we switch page tables. The map
// itself lives in low RAM the UEFI stub allocated; arch_setup_free_memory walks
// it several times, including after switch_to_runtime_page_tables() which no
// longer maps that low memory. Snapshotting it into the (always-mapped) kernel
// image keeps the later passes valid. (matches the old e820 alloca-copy.)
static osv::boot_memmap_entry memmap_snapshot[512];
static u32 memmap_snapshot_count;

static void snapshot_memmap()
{
    auto mm = reinterpret_cast<osv::boot_memmap_entry*>(osv_boot_info->memmap_addr);
    u32 n = osv_boot_info->memmap_count;
    if (n > 512) {
        n = 512;
    }
    for (u32 i = 0; i < n; i++) {
        memmap_snapshot[i] = mm[i];
    }
    memmap_snapshot_count = n;
}

// Iterate the usable physical RAM ranges (from the snapshot taken above).
void for_each_usable_range(void (*f)(mem_range e))
{
    for (u32 i = 0; i < memmap_snapshot_count; i++) {
        if (memmap_snapshot[i].type == osv::boot_mem_type::usable) {
            f(mem_range{memmap_snapshot[i].addr, memmap_snapshot[i].size});
        }
    }
}

bool intersects(const mem_range& ent, u64 a)
{
    return a > ent.addr && a < ent.addr + ent.size;
}

mem_range truncate_below(mem_range ent, u64 a)
{
    u64 delta = a - ent.addr;
    ent.addr += delta;
    ent.size -= delta;
    return ent;
}

mem_range truncate_above(mem_range ent, u64 a)
{
    u64 delta = ent.addr + ent.size - a;
    ent.size -= delta;
    return ent;
}

extern elf::Elf64_Ehdr* elf_header;
extern size_t elf_size;
extern void* elf_start;
extern boot_time_chart boot_time;

void arch_setup_free_memory()
{
    extern u64 kernel_vm_shift;
    static ulong edata, edata_phys;
    asm ("movl $.edata, %0" : "=rm"(edata));
    edata_phys = edata - kernel_vm_shift;

    // Copy the UEFI memory map out of low RAM before we touch the page tables.
    snapshot_memmap();

    for_each_usable_range([] (mem_range ent) {
        mem::frames::phys_mem_size += ent.size;
    });
    constexpr u64 initial_map = 1 << 30; // 1GB mapped by startup code

    auto c = processor::cpuid(0x80000000);
    if (c.a >= 0x80000008) {
        c = processor::cpuid(0x80000008);
        mem::mapping::phys_bits = c.a & 0xff;
        mem::mapping::virt_bits = (c.a >> 8) & 0xff;
        if(mem::mapping::phys_bits > mem::mapping::max_phys_bits){
            mem::mapping::phys_bits = mem::mapping::max_phys_bits;
        }
    }

    setup_temporary_phys_map();

    // setup all memory up to 1GB.  We can't free any more, because no
    // page tables have been set up, so we can't reference the memory being
    // freed.
    for_each_usable_range([] (mem_range ent) {
        // can't free anything below edata_phys, it's core code.
        // can't free anything below kernel at this moment
        if (ent.addr + ent.size <= edata_phys) {
            return;
        }
        if (intersects(ent, edata_phys)) {
            ent = truncate_below(ent, edata_phys);
        }
        // ignore anything above 1GB, we haven't mapped it yet
        if (intersects(ent, initial_map)) {
            ent = truncate_above(ent, initial_map);
        } else if (ent.addr >= initial_map) {
            return;
        }
        mem::frames::add_region(ent.addr, ent.size);
    });
    mem::map_phys_at(mem::linear, 0, initial_map, initial_map);
    // Map the core, loaded by the boot loader
    // In order to properly setup mapping between virtual
    // and physical we need to take into account where kernel
    // is loaded in physical memory - elf_phys_start - and
    // where it is linked to start in virtual memory - elf_start
    static mem::frames::phys_addr elf_phys_start = reinterpret_cast<mem::frames::phys_addr>(elf_header);
    // Publish the kernel image's physical base for the phys/virt helpers in
    // core/mmu.cc (the kernel is loaded at a firmware-chosen base, not a fixed
    // one). There is a simple invariant between elf_phys_start and elf_start as
    // expressed by the assignment below.
    mem::frames::elf_phys_start = reinterpret_cast<void*>(elf_phys_start);
    elf_start = reinterpret_cast<void*>(elf_phys_start + kernel_vm_shift);
    elf_size = edata_phys - elf_phys_start;
    mem::map_phys_at(elf_start, elf_phys_start, elf_size, OSV_KERNEL_BASE);
    // now that we have some free memory, we can start mapping the rest
    mem::mapping::switch_to_runtime_page_tables();
    for_each_usable_range([] (mem_range ent) {
        //
        // Free the memory below elf_phys_start which we could not before
        if (ent.addr < (u64)elf_phys_start) {
            auto ent_below_kernel = ent;
            if (ent.addr + ent.size >= (u64)elf_phys_start) {
                ent_below_kernel = truncate_above(ent, (u64) elf_phys_start);
            }
            mem::frames::add_region(ent_below_kernel.addr, ent_below_kernel.size);
            // If there is nothing left below elf_phys_start return
            if (ent.addr + ent.size <= (u64)elf_phys_start) {
               return;
            }
        }
        //
        // Ignore memory already freed above
        if (ent.addr + ent.size <= initial_map) {
            return;
        }
        if (intersects(ent, initial_map)) {
            ent = truncate_below(ent, initial_map);
        }
        mem::map_phys_at(mem::linear + ent.addr, ent.addr, ent.size, ~0);
        mem::frames::add_region(ent.addr, ent.size);
    });
}

void arch_setup_tls(void *tls, const elf::tls_data& info)
{
    struct thread_control_block *tcb;
    memcpy(tls, info.start, info.filesize);
    memset(static_cast<char*>(tls) + info.filesize, 0, info.size - info.filesize);
    tcb = reinterpret_cast<struct thread_control_block *>(static_cast<char*>(tls) + info.size);
    tcb->self = tcb;
    processor::wrmsr(msr::IA32_FS_BASE, reinterpret_cast<uint64_t>(tcb));
}

static inline void disable_pic()
{
    processor::outb(0xff, 0x21);
    processor::outb(0xff, 0xa1);
}

void arch_init_premain()
{
#if CONF_drivers_acpi
    // The UEFI stub passes the ACPI RSDP it read from the firmware config table.
    acpi::pvh_rsdp_paddr = osv_boot_info->acpi_rsdp;
#endif

    // Publish the firmware wall-clock base before the clock constructors run
    // (this runs in premain, ahead of the .init_array). Set it for both arches
    // here; the Hyper-V clock uses it as its wall-clock base.
    osv_boot_unixtime_ns = osv_boot_info->boot_unixtime_ns;

    disable_pic();
}

#include "drivers/driver.hh"

extern bool opt_pci_disabled;
void arch_init_drivers()
{
#if CONF_drivers_pci
    if (!opt_pci_disabled) {
        // Enumerate PCI devices
        pci::pci_device_enumeration();
        boot_time.event("pci enumerated");
    }
#endif

    // Initialize all drivers
    hw::driver_manager* drvman = hw::driver_manager::instance();
    boot_time.event("drivers probe");
    drvman->load_all();
    drvman->list_drivers();
}

#include "drivers/console.hh"
#include "drivers/isa-serial.hh"
#include "early-console.hh"
#include <osv/mem/mapping.hh>

void arch_init_early_console()
{
    console::isa_serial_console::early_init();
}

bool arch_setup_console(std::string opt_console)
{
    // Serial-only console (no VGA). "vga" is no longer a valid choice.
    if (opt_console.compare("serial") == 0 || opt_console.compare("all") == 0) {
        console::console_driver_add(&console::arch_early_console);
    } else {
        return false;
    }
    return true;
}
