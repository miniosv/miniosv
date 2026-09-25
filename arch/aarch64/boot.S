/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
.text
.align 16
.globl uefi_entry
.hidden uefi_entry
.type uefi_entry , "function"
uefi_entry:
        /* Kernel entry from the UEFI stub (boot/uefi/stub.cc).
         * x0 = physical address of osv::boot_info. We arrive at EL1 with the
         * MMU enabled under the firmware's identity map (UEFI hands off that
         * way), so first disable the MMU to match the rest of the boot code,
         * which builds and enables OSv's own translation tables. Because the
         * firmware map is identity, the PC stays valid across the switch. */
        mov     x19, x0                    // preserve boot_info pointer
        ldr     x20, [x0, #40]             // boot_info->kernel_phys_base (offset per boot-info.hh)

        mrs     x0, sctlr_el1
        bic     x0, x0, #1                 // clear M (MMU enable)
        msr     sctlr_el1, x0
        isb

        bl      calculate_virt_offset
        adrp    x0, exception_vectors
        add     x0, x0, x6                 // apply virt-phys offset
        msr     vbar_el1, x0
        isb

        bl      validate_el
        bl      init_stack
        bl      zero_bss
        bl      init_cpu
        bl      fixup_boot_pt
        bl      init_boot_pt
        bl      init_mmu                   // returns operating in virtual memory

        adrp    x0, kernel_vm_shift
        str     x6, [x0]
        adrp    x1, elf_header             // kernel ELF physical base
        str     x20, [x1, #:lo12:elf_header]
        adrp    x1, osv_boot_info          // boot_info pointer (arch-setup.cc)
        str     x19, [x1, #:lo12:osv_boot_info]

        mov     x29, xzr
        bl      premain
        bl      main
        bl      halt

calculate_virt_offset:
        adr     x0, text_start // fetch physical address of text_start
        ldr     x6, =(OSV_KERNEL_VM_BASE + 0x20000) // calculate virtual address of text_start per loader.ld
        sub     x6, x6, x0
        ret

init_stack:
        mov     x0, #1     // select SP_ELx
        msr     spsel, x0
        isb

        adrp    x0, init_stack_top
        add     x0, x0, x6 // make it virtual address
        mov     sp, x0
        ret

validate_el:
        mrs     x0, currentel
        ubfm    x0, x0, #2, #3 // current EL[3:2] -> X0
        cmp     x0, #1
        b.ne    halt
        ret

halt:   wfi
        b       halt

zero_bss:
        adrp    x0, .bss
        adrp    x1, .edata
zero_bss_loop:
        stp     xzr, xzr, [x0], #16
        cmp     x0, x1
        b.lo    zero_bss_loop
        ret

init_cpu:
        ic      iallu
        tlbi    vmalle1
        dsb     sy

        mov     x0, #3 << 20
        msr     cpacr_el1, x0  // no trapping on FP/SIMD instructions
        msr     mdscr_el1, xzr // monitor debug: all disabled
        isb

        /* MAIR_EL1 contains 8 entries (one per byte) defining attributes
           which we will then reference from the page tables.
           We will currently use only Attr[4] for everything, but
           keep in mind that for direct device assignment we might need
           to use the others too, and adapt the pt_element API */
        ldr     x0, =0xff440c0400
        msr	mair_el1, x0
        isb

        /* TCR - Translation Control Register
         *
         * 63                  39   38   37  36   35    34   32
         * [        RES0         ] TBI1 TBI0 AS [RES0] [  IPS  ]
         *                           0    0   1    0     1 0 1
         *  31 30   29 28   27 26   25 24   23    22   21      16
         * [ TG1 ] [ SH1 ] [ORGN1] [IRGN1] EPD1   A1   [  T1SZ   ]
         *   1  0    1  1    0  1    0  1    0     0     010000
         *  15 14   13 12   11 10    9  8    7     6    5       0
         * [ TG0 ] [ SH0 ] [ORGN0] [IRGN0] EPD0 [RES0] [  T0SZ   ]
         *   0  0    1  1    0  1    0  1    0     0     010000
         */

        /* we choose 4K granularity, 48-bit addresses, two subranges:
         *
         * TTBR1_EL1 -> 0xffff_0000_0000_0000 to 0xffff_ffff_ffff_ffff
         * TTBR0_EL1 -> 0x0000_0000_0000_0000 to 0x0000_ffff_ffff_ffff
         * ASID = 16bit TTBR
         */
        ldr     x0, =0x15b5103510

        /* FEAT_HAFDBS (armv8.1): let the hardware maintain the access flag,
         * and the dirty state through the descriptor's DBM bit. Without it
         * both are software's job -- an access-flag fault per page, and every
         * eviction writing back. ID_AA64MMFR1_EL1.HAFDBS is 1 for AF, 2 for
         * AF and dirty. Every cpu runs this, so they all agree. */
        mrs     x1, id_aa64mmfr1_el1
        and     x1, x1, #0xf
        cmp     x1, #1
        b.lt    1f
        orr     x0, x0, #(1 << 39)      // TCR_EL1.HA
        cmp     x1, #2
        b.lt    1f
        orr     x0, x0, #(1 << 40)      // TCR_EL1.HD
1:
        msr     tcr_el1, x0
        isb
        ret

fixup_boot_pt:
        // x6 contains kernel phys/virt offset
        mov     x29, x30 // save link register so wa can call fixup_pt_entry

        /* we need to fix the addresses of the ident subtables to make them physical */
        /* as the virt-phys mapping structures use physical addresses during walk-up */

        // subtract p/v offset from the address of the subtable ident_pt_l3_ttbr0
        adrp    x0, ident_pt_l4_ttbr0
        bl      fixup_pt_entry

        // subtract p/v offset from the address of the subtable ident_pt_l2_0_ttbr0
        adrp    x0, ident_pt_l3_ttbr0
        bl      fixup_pt_entry
        // subtract p/v offset from the address of the subtable ident_pt_l2_1_ttbr0
        add     x0, x0, #8
        bl      fixup_pt_entry
        // subtract p/v offset from the address of the subtable ident_pt_l2_2_ttbr0
        add     x0, x0, #8
        bl      fixup_pt_entry
        // subtract p/v offset from the address of the subtable ident_pt_l2_3_ttbr0
        add     x0, x0, #8
        bl      fixup_pt_entry
        // subtract p/v offset from the address of the subtable kernel_pt_l2_63_ttbr0
        add     x0, x0, #480  //=60*8
        bl      fixup_pt_entry

        // fix 63-64 GB mapping which maps the kernel code

        // From https://www.kernel.org/doc/Documentation/arm64/booting.txt:
        //"The Image must be placed text_offset bytes from a 2MB aligned base
        // address anywhere in usable system RAM and called there."

        // Given the kernel can be loaded in any place of the allowed range of physical memory
        // we have to dynamically figure out the physical address so that we can correctly
        // initialize the pt entries of the 63th GB of the virtual memory which is where
        // the kernel is expected to be per loader.ld.

        // We use the physical address of the symbol text_start located in the 1st 2MB
        // of the kernel text to identify which 2MB of RAM the kernel ELF is loaded.
        // Then we use this information to map the 63-rd GB of virtual memory into
        // whatever 2 MB-aligned area of physical memory the kernel is loaded.

        // round-up to whole 2MB and store in x2
        adrp    x0, text_start
        lsr     x2, x0, #21
        lsl     x2, x2, #21

        // start with pt entry stored at kernel_pt_l2_63_ttbr0 (x0)
        adrp    x0, kernel_pt_l2_63_ttbr0
        mov     x7, #512
kernel_pt_loop:
        ldr     x1, [x0]   // load initial value of pt entry at address stored in x0
        add     x1, x1, x2 // add the physical address offset
        str     x1, [x0]   // store the fixed pt value

        add     x0, x0, #8 // fix another 2MB
        sub     x7, x7, #1
        cbnz    x7, kernel_pt_loop

        mov     x30, x29   // restore link register (x30) so we can properly return ret

fixup_pt_entry:
        ldr     x1, [x0]   // x0 contains address of the entry, x6 contains offset
        sub     x1, x1, x6 // apply kernel offset
        str     x1, [x0]   // store the fixed pt entry back into memory
        ret

init_boot_pt:
        adrp    x0, ident_pt_l4_ttbr0
        adrp    x1, ident_pt_l4_ttbr1
        ret

init_runtime_pt:
        adrp    x1, smpboot_ttbr0
        ldr     x0, [x1], #8
        ldr     x1, [x1]
        ret

        /* call with x0, x1 containing ttbr0, ttbr1 to set */
init_mmu:
        msr     ttbr0_el1, x0
        msr     ttbr1_el1, x1
        isb

        /* SCTRL_EL1 - System Control Register (EL1)
         *
         *   31 30 29 28 27   26   25  24  23 22 21 20  19   18   17  16
         *                  [UCI] [EE E0E]             [WXN][nTWE]   [nTWI]
         *    0  0  1  1  0    0    0   0   1  1  0  1   0    0    0   0
         *
         *   15  14  13 12 11 10   9    8    7    6    5   4    3  2  1  0
         * [UCT][DZE]   [I]      [UMA][SED][ITD][THE][CP][SA0][SA][C][A][M]
         *    0   0   0  1  1  0   0    1    1    0    0   1    1  1  0  1
         */
        ldr     x0, =0x30d0199d
        msr     sctlr_el1, x0
        isb

        // Apply offset to switch to the kernel VM address so that we jump
        // into virtual memory space of where kernel is mapped (63-rd GB)
        add     x30, x30, x6
        ret

switch_to_runtime_pt:
        msr     ttbr0_el1, x0
        msr     ttbr1_el1, x1
        isb

        dsb sy  //Flush TLB
        tlbi vmalle1
        dsb sy
        isb

        ret

.align 16
.globl start_secondary_cpu
.hidden start_secondary_cpu
.type  start_secondary_cpu , "function"
start_secondary_cpu:
        bl      calculate_virt_offset
        /* x6 contains virt-phys offset */
        adrp    x0, exception_vectors
        add     x0, x0, x6    // apply virt-phys offset to make it virtual address
        msr     vbar_el1, x0
        isb

        bl      init_cpu
        bl      init_boot_pt  // use the boot mapping tables fixed by uefi_entry on primary CPU
        bl      init_mmu
        /* after we returned from init_mmu we are operating in the virtual memory space */
        /* but on secondary CPU we can subsequently switch to the runtime mapping tables */
        bl      init_runtime_pt
        bl      switch_to_runtime_pt

        ldr     x0, =smp_stack_free    /* ptr */
1:
        ldxr    x1, [x0]               /* read current value AND set exclusive monitor */
        ldr     x2, [x1, #4096]        /* fetch next value */
        stxr    w4, x2, [x0]           /* write new value AND clear monitor */
        cbnz    w4, 1b                 /* retry if stxr above failed = w4 == 1 */

        dmb     ish
        mov     x0, #1     // select SP_ELx
        msr     spsel, x0
        isb
        add     x1, x1, #4096
        mov     sp, x1
        mov     x29, xzr
        bl      smp_main
        bl      halt

.align 12
ident_pt_l4_ttbr0:
        .quad ident_pt_l3_ttbr0 + 0x3
        .rept 511
        .quad 0
        .endr
ident_pt_l3_ttbr0:
        .quad ident_pt_l2_0_ttbr0 + 0x3 // Map 0GB-1GB one-to-one
        .quad ident_pt_l2_1_ttbr0 + 0x3 // Map 1GB-2GB one-to-one
        .quad ident_pt_l2_2_ttbr0 + 0x3 // Map 2GB-3GB one-to-one
        .quad ident_pt_l2_3_ttbr0 + 0x3 // Map 3GB-4GB one-to-one
        // Map 4GB-63GB one-to-one with 1GB block descriptors (valid at this
        // level). The 0-4GB range above keeps 2MB granularity for early device
        // access; the rest only needs to make physical RAM reachable during
        // early boot, before arch_setup_free_memory builds the linear map -
        // guests with more than 4GB RAM place stacks/allocations up here and
        // would otherwise fault on first touch.
        index = 4
        .rept 59
        .quad (index << 30) + 0x411
        index = index + 1
        .endr
        .quad kernel_pt_l2_63_ttbr0 + 0x3 // Map 63GB-64GB -> 1GB-2GB
        .rept 448
        .quad 0
        .endr
// ident_pt_l2_0_ttbr0 .. ident_pt_l2_3_ttbr0 are four contiguous 512-entry L2
// tables covering physical 0-4 GiB in 2 MiB blocks (entry i maps i*2MiB). Made
// global so the early C++ (uefi_memory_setup) can re-mark the blocks that are
// actually RAM as Normal cacheable - the static map below assumes the low 1 GiB
// is pure MMIO (true on QEMU/AWS/GCP), which is wrong on Azure where RAM lives
// in the low 1 GiB.
.globl ident_pt_l2_0_ttbr0
.hidden ident_pt_l2_0_ttbr0
ident_pt_l2_0_ttbr0:
        // The low 1GB is platform MMIO (GIC, UART, PCIe ECAM, ...) - RAM starts
        // at 1GB on QEMU virt and AWS Graviton alike. Map it as Device memory
        // (attr index 0 -> 0x401) rather than Normal cacheable (0x411): the
        // early console writes the UART through this mapping, and cacheable
        // MMIO drops/reorders those writes on real hardware (garbled output).
        index = 0
        offset = 0x00000000
        .rept 512
        .quad offset + (index << 21) + 0x401
        index = index + 1
        .endr
ident_pt_l2_1_ttbr0:
        index = 0
        offset = 0x40000000
        .rept 512
        .quad offset + (index << 21) + 0x411
        index = index + 1
        .endr
ident_pt_l2_2_ttbr0:
        index = 0
        offset = 0x80000000
        .rept 512
        .quad offset + (index << 21) + 0x411
        index = index + 1
        .endr
ident_pt_l2_3_ttbr0:
        index = 0
        offset = 0xC0000000
        .rept 512
        .quad offset + (index << 21) + 0x411
        index = index + 1
        .endr
kernel_pt_l2_63_ttbr0:
        index = 0
        offset = 0
        .rept 512
        .quad offset + (index << 21) + 0x411
        index = index + 1
        .endr
.align 12
ident_pt_l4_ttbr1:
        .rept 512
        .quad 0
        .endr

.bss
.align 16
init_stack_bottom = .
. = . + 4096*4
init_stack_top = .

.globl smp_stack_free
.hidden smp_stack_free
.type smp_stack_free , "object"
smp_stack_free: .quad 0

.globl smpboot_ttbr0
.hidden smpboot_ttbr0
.type smpboot_ttbr0 , "object"
.hidden smpboot_ttbr1
.globl smpboot_ttbr1
.type smpboot_ttbr1 , "object"

.align 12
smpboot_ttbr0:  .quad 0
smpboot_ttbr1:  .quad 0

.hidden kernel_vm_shift
.globl kernel_vm_shift
.type kernel_vm_shift, "object"
.align 16
kernel_vm_shift: .quad 0

/* hmm should we provide an interrupt stack?
. = . + 4096*10
.global interrupt_stack_top
interrupt_stack_top = .
*/
