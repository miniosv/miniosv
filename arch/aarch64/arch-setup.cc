/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
#include <osv/kernel_config.h>
#include "arch-setup.hh"
#include <osv/sched.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/phys.hh>
#include <osv/elf.hh>
#include <osv/types.h>
#include <string.h>
#include <osv/boot.hh>
#include <osv/debug.hh>
#include <osv/commands.hh>

#include <mem.hh>
#include "gic-v2.hh"
#include "gic-v3.hh"
#include "drivers/acpi.hh"

#include "drivers/console.hh"
#include "drivers/pl011.hh"
#include "early-console.hh"
#if CONF_drivers_pci
#include <osv/pci.hh>
#endif
#include "drivers/mmio-isa-serial.hh"

#include <osv/boot-info.hh>
#include <osv/prio.hh>
#include <alloca.h>

// Physical pointer to the hand-off structure filled by the UEFI stub; stored
// by uefi_entry in boot.S.
osv::boot_info* osv_boot_info;

// Kernel command line (set from the UEFI LoadOptions in uefi_memory_setup).
char *cmdline;

// Wall-clock at boot (UEFI GetTime), nanoseconds since the Unix epoch. Captured
// early because boot_info lives in low RAM that is unmapped once the runtime
// page tables are installed, before the clock constructor runs.
u64 osv_boot_unixtime_ns;

// Snapshot of the usable RAM ranges from the UEFI memory map, taken in the
// early uefi_memory_setup() constructor and consumed later by
// arch_setup_free_memory(). This mirrors the x86_64 port
// (arch/x64/arch-setup.cc): the boot_info memory map lives in low RAM that is
// unmapped once the runtime page tables are installed, so we copy the ranges
// we need into the kernel image up front.
namespace {
struct usable_range { u64 addr; u64 size; };
constexpr unsigned max_usable_ranges = 512;
usable_range usable_ranges[max_usable_ranges];
unsigned usable_range_count;

void snapshot_usable_ranges(osv::boot_info* bi)
{
    auto mm = reinterpret_cast<osv::boot_memmap_entry*>(bi->memmap_addr);
    usable_range_count = 0;
    for (u32 i = 0; i < bi->memmap_count && usable_range_count < max_usable_ranges; i++) {
        if (mm[i].type == osv::boot_mem_type::usable) {
            usable_ranges[usable_range_count++] = { mm[i].addr, mm[i].size };
        }
    }
}

// Free the part of [rstart, rend) that lies within [lo, hi) and outside the
// kernel image [kstart, kend), handing it to the page allocator.
void free_usable(u64 rstart, u64 rend, u64 lo, u64 hi, u64 kstart, u64 kend)
{
    u64 s = rstart < lo ? lo : rstart;
    u64 e = rend > hi ? hi : rend;
    if (s >= e) {
        return;
    }
    if (e <= kstart || s >= kend) {          // entirely outside the kernel
        mem::frames::add_region(s, e - s);
        return;
    }
    if (s < kstart) {                        // part below the kernel
        mem::frames::add_region(s, kstart - s);
    }
    if (e > kend) {                          // part above the kernel
        mem::frames::add_region(kend, e - kend);
    }
}
}

// Derive the physical memory layout and ELF extents from the UEFI memory map.
// Runs as an early constructor (init_prio::dtb); elf_header, kernel_vm_shift
// and osv_boot_info were already set by uefi_entry.
void __attribute__((constructor(init_prio::dtb))) uefi_memory_setup()
{
    auto bi = osv_boot_info;
    auto mm = reinterpret_cast<osv::boot_memmap_entry*>(bi->memmap_addr);
    u64 kbase = bi->kernel_phys_base;

    // Re-mark the boot identity map: any usable RAM below 4 GiB must be Normal
    // cacheable, not Device. The static map in boot.S marks the low 1 GiB Device
    // because on QEMU/AWS/GCP that range is pure MMIO and RAM starts at 1 GiB -
    // but Azure places RAM in the low 1 GiB, and running RAM (atomics, unaligned
    // access) through a Device mapping faults. ident_pt_l2_0_ttbr0 is the base
    // of four contiguous 512-entry L2 tables (entry i maps physical i*2 MiB over
    // 0-4 GiB); rewrite the blocks covered by usable RAM to Normal (attr index
    // 4 = 0x411), leaving MMIO blocks Device. This runs before any heap/atomic
    // use (init_prio::dtb, the first post-banner constructor) and while we are
    // still on the boot page tables, so the tables are writable.
    extern u64 ident_pt_l2_0_ttbr0[];
    constexpr u64 BLOCK = 0x200000;            // 2 MiB
    constexpr u64 NR_BLOCKS_4G = 0x100000000ull / BLOCK;  // 2048
    for (u32 i = 0; i < bi->memmap_count; i++) {
        if (mm[i].type != osv::boot_mem_type::usable)
            continue;
        u64 first = mm[i].addr / BLOCK;
        u64 last = (mm[i].addr + mm[i].size + BLOCK - 1) / BLOCK;  // exclusive
        if (last > NR_BLOCKS_4G)
            last = NR_BLOCKS_4G;
        for (u64 b = first; b < last; b++)
            ident_pt_l2_0_ttbr0[b] = (b * BLOCK) | 0x411;  // Normal cacheable block
    }
    mem::mapping::flush_all();

    // mem_addr is the 2MB-aligned base the boot page tables map the kernel
    // window onto. The UEFI stub loaded the kernel here with AllocatePages.
    mem::frames::ram_base = kbase & ~((u64)0x200000 - 1);

    // Record every usable RAM range the firmware reports and total them up.
    // The old code grew a single contiguous run upward from the kernel base and
    // stopped at the first gap in the memory map, discarding all RAM above it.
    // On a large guest that throws away most of memory: e.g. a 192 GiB QEMU
    // virt guest, whose usable RAM has a small reserved gap ~62 GiB up, exposed
    // only ~62 GiB. We instead expose every usable range; arch_setup_free_memory()
    // frees each one individually, so a gap between ranges (e.g. that reserved
    // hole, or the AWS Nitro low/high RAM split) is never spanned - it just
    // separates two ranges we free independently, which was the concern that
    // motivated the single-range approach.
    snapshot_usable_ranges(bi);
    if (usable_range_count == 0) {
        abort("uefi_memory_setup: firmware reported no usable memory.\n");
    }
    mem::frames::phys_mem_size = 0;
    for (unsigned i = 0; i < usable_range_count; i++) {
        mem::frames::phys_mem_size += usable_ranges[i].size;
    }

    // Command line and wall-clock base provided by the UEFI stub.
    cmdline = reinterpret_cast<char*>(bi->cmdline_addr);
    osv_boot_unixtime_ns = bi->boot_unixtime_ns;

    // Compute the ELF extents (physical/virtual start and size).
    u64 edata;
    asm volatile ("adrp %0, .edata" : "=r"(edata));
    extern elf::Elf64_Ehdr *elf_header;
    extern size_t elf_size;
    extern void *elf_start;
    extern u64 kernel_vm_shift;
    mem::frames::elf_phys_start = reinterpret_cast<void *>(elf_header);
    elf_start = static_cast<char *>(mem::frames::elf_phys_start) + kernel_vm_shift;
    elf_size = (u64)edata - (u64)elf_start;
    // phys_mem_size is the true total of usable RAM (reported via
    // sysconf(_SC_PHYS_PAGES)); the kernel image's pages are simply not handed
    // to the allocator in arch_setup_free_memory(), rather than subtracted here.
}

void setup_temporary_phys_map()
{
    // duplicate 1:1 mapping into the lower part of phys_mem
    u64 *pt_ttbr0 = reinterpret_cast<u64*>(processor::read_ttbr0());
    pt_ttbr0[mem::mapping::pt_index(mem::linear, 3)] = pt_ttbr0[0];
    mem::mapping::flush_all();
}

#if CONF_drivers_pci
// Locate the PCIe ECAM config space from the ACPI MCFG table and map it. Like
// Nanos, this is the only PCI information we take from firmware: the BARs are
// left exactly as UEFI programmed them (arch_add_bar reads them as-is, so the
// io/mem allocation windows are never needed), and device interrupts are
// delivered as MSI-X through the GIC ITS, so there is no INTx routing table to
// parse. Called from arch_init_drivers, after the ACPI tables are parsed.
void arch_setup_pci()
{
    auto mcfg = reinterpret_cast<const acpi::mcfg*>(acpi::find_table(ACPI_SIG_MCFG));
    if (!mcfg) {
        return;   // no PCIe host bridge described - leave PCI disabled
    }

    // Use the first configuration-space allocation (segment 0).
    auto alloc = reinterpret_cast<const acpi::mcfg_alloc*>(
        reinterpret_cast<const char*>(mcfg) + sizeof(acpi::mcfg));
    u64 ecam_base = alloc->base_address;
    size_t ecam_len = (static_cast<size_t>(alloc->end_bus - alloc->start_bus + 1)) << 20;

    pci::set_pci_ecam(true);
    pci::set_pci_cfg(ecam_base, ecam_len);
    mem::map_phys_at((void *)ecam_base, (mem::frames::phys_addr)ecam_base, ecam_len, mem::mapping::page_size, mem::mattr::dev);
}
#endif

extern bool opt_pci_disabled;
void arch_setup_free_memory()
{
    mem::mapping::detect_hw_dirty();
    setup_temporary_phys_map();

    /* import from loader.cc */
    extern size_t elf_size;
    extern elf::Elf64_Ehdr* elf_header;

    // The kernel image (boot trampoline + DTB copy + ELF) occupies
    // [mem_addr, addr); everything else in the usable ranges is free RAM.
    mem::frames::phys_addr addr = (mem::frames::phys_addr)elf_header + elf_size;
    const u64 kernel_start = mem::frames::ram_base;
    const u64 kernel_end = addr;

    // linear_map() allocates its own page-table pages from the page allocator
    // and writes them through the linear-map window. Before the real mappings
    // below exist, that window is backed only by the boot page tables (the low
    // 4 GiB identity map that setup_temporary_phys_map() cloned) - so we must
    // seed the allocator with already-mapped low RAM first, map everything, and
    // only then free the high RAM. Freeing all of it up front (the obvious
    // order) lets linear_map() grab a page-table page from still-unmapped high
    // RAM and fault on the write. This mirrors the x86_64 port's use of its
    // 1 GiB initial_map.
    constexpr u64 initial_map = 4ull << 30;   // low 4 GiB mapped by boot.S

    // Step 1: free usable RAM below initial_map (already mapped) to bootstrap
    // the page-table allocator.
    for (unsigned i = 0; i < usable_range_count; i++) {
        u64 rstart = usable_ranges[i].addr;
        free_usable(rstart, rstart + usable_ranges[i].size,
                    0, initial_map, kernel_start, kernel_end);
    }

    /* linear_map [TTBR1] */
    // Step 2: install the real linear mappings for every usable range in each
    // identity-mapped area (page tables come from the low RAM freed above). The
    // kernel-occupied pages are mapped here too (as on x86_64); the kernel also
    // keeps its separate OSV_KERNEL_VM_BASE window mapped below.
    //
    // Round each range out to 2 MiB (huge-page) boundaries so the mapping is all
    // aligned 2 MiB blocks. A range that ends mid-2 MiB-page (firmware splits RAM
    // at 4 KiB boundaries) would otherwise back that page with a 4 KiB L3 table;
    // ACPI's map_phys() later linear_map()s the same page (a table in the reserved
    // hole just past the range) as a 2 MiB block, and on aarch64 that table->block
    // swap without break-before-make is a TLB conflict that hangs the boot. With
    // everything block-aligned, map_phys() only ever rewrites an identical block.
    // The extra reserved bytes pulled in by rounding are never freed (steps 1/3
    // free the exact ranges only).
    for (unsigned i = 0; i < usable_range_count; i++) {
        u64 rstart = usable_ranges[i].addr;
        u64 rend = rstart + usable_ranges[i].size;
        u64 mstart = rstart & ~(mem::mapping::huge_page_size - 1);
        u64 mend = (rend + mem::mapping::huge_page_size - 1) & ~(mem::mapping::huge_page_size - 1);
        mem::map_phys_at(mem::linear + mstart, mstart, mend - mstart);
    }

    /* linear_map [TTBR0 - boot, DTB and ELF] */
    /* physical memory layout - relative to the 2MB-aligned address PA stored in mem::frames::ram_base
       PA +     0x0 - PA + 0x80000: boot
       PA + 0x80000 - PA + 0x90000: DTB copy
       PA + 0x90000 -       [addr]: kernel ELF */
    mem::map_phys_at((void *)(OSV_KERNEL_VM_BASE - 0x80000), (mem::frames::phys_addr)mem::frames::ram_base,
                    addr - mem::frames::ram_base);

    if (console::PL011_Console::active) {
        /* linear_map [TTBR0 - UART] */
        addr = (mem::frames::phys_addr)console::aarch64_console.pl011.get_base_addr();
        mem::map_phys_at((void *)addr, addr, 0x1000, mem::mapping::page_size,
                        mem::mattr::dev);
    }

    // The GIC is discovered from the ACPI MADT and constructed in a constructor
    // at init_prio::gic (see init_gic_acpi below), which runs after the ACPI
    // tables are parsed (init_prio::acpi) and before the timer needs it. PCI is
    // set up later in arch_init_drivers, once ACPI (MCFG) is available.

    // Strip console= options from the command line before memory is unmapped.
    // The slim kernel does not turn the cmdline into commands (the app is
    // statically linked in), so there is no parse_cmdline step.
    console::mmio_isa_serial_console::clean_cmdline(cmdline);

    mem::mapping::switch_to_runtime_page_tables();

    // Step 3: hand the high RAM (>= initial_map) to the allocator. This must run
    // AFTER the switch: the per-range linear maps built above live in the
    // runtime page tables (the mapping layer's root_slot() names
    // page_table_root[], not the boot tables), and the page allocator writes
    // bookkeeping - the page_range header
    // and a trailing back-pointer - at the START and END of each freed range
    // through the linear-map window. Those addresses only resolve once the
    // runtime tables are active; the boot temporary map only covers the low
    // contiguous region freed in step 1.
    for (unsigned i = 0; i < usable_range_count; i++) {
        u64 rstart = usable_ranges[i].addr;
        free_usable(rstart, rstart + usable_ranges[i].size,
                    initial_map, ~0ull, kernel_start, kernel_end);
    }

    console::mmio_isa_serial_console::memory_map();
}

// Not inlined into arch_setup_tls: clang CSEs "mrs tpidr_el0" within a
// function, so a TLS read inlined after the msr that installs the
// thread pointer may use a stale base (see also arch-switch.hh).
static __attribute__((noinline)) void check_boot_tls()
{
    /* check that the tls variable preempt_counter is correct */
    assert(sched::get_preempt_counter() == 1);
}

void arch_setup_tls(void *tls, const elf::tls_data& info)
{
    struct thread_control_block *tcb;
    memset(tls, 0, sizeof(*tcb) + info.size);

    tcb = (thread_control_block *)tls;
    tcb[0].tls_base = &tcb[1];

    memcpy(&tcb[1], info.start, info.filesize);
    asm volatile ("msr tpidr_el0, %0; isb; " :: "r"(tcb) : "memory");

    check_boot_tls();
}

void arch_init_premain()
{
#if CONF_drivers_acpi
    // Hand the RSDP the UEFI stub found to the ACPI layer before its early_init
    // constructor runs - exactly as x86 does in its arch_init_premain.
    acpi::pvh_rsdp_paddr = osv_boot_info->acpi_rsdp;
#endif
}

#if CONF_drivers_acpi
// Discover the GIC from the ACPI MADT and construct the matching driver. This
// walks the MADT subtables the same way arch/x64/smp.cc does, so both arches
// share one ACPI parsing model. It runs after acpi::early_init (init_prio::acpi)
// and before the generic timer, which needs the GIC (init_prio::gic).
static void __attribute__((constructor(init_prio::gic))) init_gic_acpi()
{
    auto madt = reinterpret_cast<const acpi::madt*>(acpi::find_table(ACPI_SIG_MADT));
    if (!madt) {
        abort("arch-setup: no MADT table - cannot locate the GIC.\n");
    }

    u64 gicd = 0, gits = 0, gicc_cpuif = 0;
    u8 version = 0;

    // Collect redistributor regions in both forms the MADT may use: contiguous
    // GICR discovery ranges (one subtable, many frames - QEMU/AWS) and per-CPU
    // gicr_base_address in each GICC entry (one frame each - Azure). Keep them
    // separate and prefer the GICR subtables; fall back to the GICC form only
    // when no GICR subtable is present (the two are mutually exclusive in
    // practice, and a system using GICR subtables sets gicr_base_address to 0).
    mem::frames::phys_addr gicr_base[MAX_GICR_REGIONS];
    size_t    gicr_len[MAX_GICR_REGIONS];
    int       nr_gicr = 0;
    mem::frames::phys_addr gicc_redist_base[MAX_GICR_REGIONS];
    int       nr_gicc_redist = 0;

    auto subtable = reinterpret_cast<const char*>(madt + 1);
    auto madt_end = reinterpret_cast<const char*>(madt) + madt->header.length;
    while (subtable < madt_end) {
        auto s = reinterpret_cast<const acpi::madt_subtable*>(subtable);
        switch (s->type) {
        case acpi::MADT_GICD: {
            auto d = reinterpret_cast<const acpi::madt_gicd*>(s);
            gicd = d->physical_base_address;
            version = d->gic_version;
            break;
        }
        case acpi::MADT_GICR: {
            auto r = reinterpret_cast<const acpi::madt_gicr*>(s);
            if (nr_gicr < MAX_GICR_REGIONS) {
                gicr_base[nr_gicr] = r->discovery_range_base_address;
                gicr_len[nr_gicr] = r->discovery_range_length;
                nr_gicr++;
            }
            break;
        }
        case acpi::MADT_GIC_ITS: {
            auto i = reinterpret_cast<const acpi::madt_gic_its*>(s);
            gits = i->physical_base_address;
            break;
        }
        case acpi::MADT_GICC: {
            auto c = reinterpret_cast<const acpi::madt_gicc*>(s);
            // GICv2 takes the CPU interface base from here; GICv3 publishes a
            // per-cpu redistributor here when there is no GICR subtable.
            if (!gicc_cpuif) {
                gicc_cpuif = c->physical_base_address;
            }
            if (c->gicr_base_address && nr_gicc_redist < MAX_GICR_REGIONS) {
                gicc_redist_base[nr_gicc_redist++] = c->gicr_base_address;
            }
            break;
        }
        default:
            break;
        }
        subtable += s->length;
    }

    // The MADT carries base addresses but not region sizes; use the
    // architectural defaults (the GICR discovery-range length comes from the
    // MADT; per-CPU GICC redistributors are one 0x20000 frame each).
    constexpr size_t GICD_LEN = 0x10000;
    constexpr size_t GITS_LEN = 0x20000;
    constexpr size_t GICR_FRAME_LEN = 0x20000;

    if (nr_gicr == 0) {
        for (int i = 0; i < nr_gicc_redist; i++) {
            gicr_base[i] = gicc_redist_base[i];
            gicr_len[i] = GICR_FRAME_LEN;
        }
        nr_gicr = nr_gicc_redist;
    }

    if (version == 3 || nr_gicr) {
        gic::gic = new gic::gic_v3_driver(gicd, GICD_LEN, gicr_base, gicr_len,
                                          nr_gicr, gits, gits ? GITS_LEN : 0);
    } else if (gicd && gicc_cpuif) {
        gic::gic = new gic::gic_v2_driver(gicd, GICD_LEN, gicc_cpuif, 0x2000, 0, 0);
    } else {
        abort("arch-setup: MADT has no usable GIC description.\n");
    }
}
#endif

#include "drivers/driver.hh"
#include <osv/mem/mapping.hh>

void arch_init_drivers()
{
    extern boot_time_chart boot_time;

#if CONF_drivers_pci
    if (!opt_pci_disabled) {
        // Discover the ECAM config space from the ACPI MCFG (available now that
        // the ACPI tables are parsed), then enumerate. Device interrupts are
        // MSI-X via the GIC ITS, so there is no INTx irqmap to set up.
        arch_setup_pci();

        size_t pci_cfg_len;
        if (pci::get_pci_cfg(&pci_cfg_len)) {
            pci::pci_device_enumeration();
            boot_time.event("pci enumerated");
        }
    }
#endif

    // Initialize all drivers
    hw::driver_manager* drvman = hw::driver_manager::instance();
    boot_time.event("drivers probe");
    drvman->load_all();
    drvman->list_drivers();
}

void arch_init_early_console()
{
    // Like the x86 early console (a fixed COM port), the aarch64 early console
    // uses a compiled-in default UART. UEFI hands off no device tree, and ACPI
    // is not parsed this early, so we rely on the PL011 driver's default base
    // address and IRQ (the ARM PL011 at the standard platform address, as used
    // by QEMU's virt machine and typical ARM firmware).
    console::mmio_isa_serial_console::_phys_mmio_address = 0;

    // The ACPI SPCR table (parsed by the UEFI stub and passed in boot_info)
    // describes the firmware console UART. Its Interface Type tells us which
    // driver to use: a PL011/SBSA (type 3 or 0x0e) at the QEMU virt address, or
    // a 16550-compatible UART (type 0/1) such as the one AWS Graviton exposes at
    // 0x90a0000 with 32-bit registers (mmio32, reg-shift 2). Picking the wrong
    // driver pokes meaningless register offsets - which on a 16550 garbles the
    // output (writes land on the data register by luck) and hangs on the PL011
    // FR/TXFF read (offset 0x18 decodes to nothing). Default to PL011 when SPCR
    // gave us nothing (QEMU virt without an SPCR).
    bool is_16550 = osv_boot_info && osv_boot_info->uart_base &&
                    osv_boot_info->uart_type <= 2;
    if (is_16550) {
        new (&console::aarch64_console.isa_serial)
            console::mmio_isa_serial_console();
        int width = osv_boot_info->uart_access_width
                        ? osv_boot_info->uart_access_width : 1;
        console::mmio_isa_serial_console::early_init_polled(
            osv_boot_info->uart_base, width);
        console::arch_early_console = console::aarch64_console.isa_serial;
    } else {
        new (&console::aarch64_console.pl011) console::PL011_Console();
        if (osv_boot_info && osv_boot_info->uart_base) {
            console::aarch64_console.pl011.set_base_addr(osv_boot_info->uart_base);
        }
        console::arch_early_console = console::aarch64_console.pl011;
        console::PL011_Console::active = true;
    }
}

bool arch_setup_console(std::string opt_console)
{
    if (opt_console.compare("pl011") == 0) {
        console::console_driver_add(&console::arch_early_console);
    } else if (opt_console.compare("all") == 0) {
        console::console_driver_add(&console::arch_early_console);
    } else {
        return false;
    }
    return true;
}
