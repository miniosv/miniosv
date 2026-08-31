/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

SECTIONS
{
	/* Set the initial program counter to one page beyond the minimal
	 * aligned segment size.  This allows the ELF header to reside with
	 * the text segment, which is useful since we need the ELF header
	 * to link against libraries later on.
	 *
	 * We can't export the ELF header base as a symbol, because ld
	 * insists on moving stuff around if we do.
	 */
    . = OSV_KERNEL_VM_BASE + 0x1000;
    .text : AT(ADDR(.text) - OSV_KERNEL_VM_SHIFT) {
        HIDDEN(text_start = .);
        *(.text.hot .text.hot.*)
        *(.text.unlikely .text.*_unlikely)
        *(.text.fixup)
        *(.text.startup .text.startup.*)
        *(.text .text.*)
        HIDDEN(text_end = .);
    } :text
    . = ALIGN(8);
    .fixup : AT(ADDR(.fixup) - OSV_KERNEL_VM_SHIFT) {
        HIDDEN(fault_fixup_start = .);
        *(.fixup)
        HIDDEN(fault_fixup_end = .);
    KEEP(*(.fixup)) } :text

    . = ALIGN(8);
    .memcpy_decode : AT(ADDR(.memcpy_decode) - OSV_KERNEL_VM_SHIFT) {
        HIDDEN(memcpy_decode_start = .);
        *(.memcpy_decode)
        HIDDEN(memcpy_decode_end = .);
    KEEP(*(.memcpy_decode)) } :text

    /* .note.gnu.property sections should be discarded */
    /DISCARD/ : {
       *(.note.gnu.property)
    }

    .eh_frame : AT(ADDR(.eh_frame) - OSV_KERNEL_VM_SHIFT) { *(.eh_frame) KEEP(*(.eh_frame)); } : text
    .rodata : AT(ADDR(.rodata) - OSV_KERNEL_VM_SHIFT) { *(.rodata*) } :text
    .eh_frame : AT(ADDR(.eh_frame) - OSV_KERNEL_VM_SHIFT) { *(.eh_frame) KEEP(*(.eh_frame)); } :text
    .eh_frame_hdr : AT(ADDR(.eh_frame_hdr) - OSV_KERNEL_VM_SHIFT) { *(.eh_frame_hdr) KEEP(*(.eh_frame_hdr)); } :text :eh_frame
    .note : AT(ADDR(.note) - OSV_KERNEL_VM_SHIFT) { *(.note*) } :text :note
    .gcc_except_table : AT(ADDR(.gcc_except_table) - OSV_KERNEL_VM_SHIFT) { *(.gcc_except_table) *(.gcc_except_table.*) } : text
    .tracepoint_patch_sites ALIGN(8) : AT(ADDR(.tracepoint_patch_sites) - OSV_KERNEL_VM_SHIFT) {
        HIDDEN(__tracepoint_patch_sites_start = .);
        *(.tracepoint_patch_sites)
        HIDDEN(__tracepoint_patch_sites_end = .);
    KEEP(*(.tracepoint_patch_sites)) } : text
    .data.rel.ro : AT(ADDR(.data.rel.ro) - OSV_KERNEL_VM_SHIFT) { *(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*) *(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*) } : text
    .data : AT(ADDR(.data) - OSV_KERNEL_VM_SHIFT) { *(.data) *(.data.*) } :text
    . = ALIGN(8);
    HIDDEN(__rela_iplt_start = .);
    .rela.iplt : AT(ADDR(.rela.iplt) - OSV_KERNEL_VM_SHIFT) {
        *(.rela.iplt) *(.rela.dyn) *(.rela.plt)
    } :text
    HIDDEN(__rela_iplt_end = .);
    /* Deferred constructors: init_priority(100) is the highest the toolchain
     * allows, and libc++ uses it for the global that initializes the standard
     * streams (std::cout/cin/cerr). With the application linked into the kernel,
     * that constructor would otherwise run first in premain()'s init-array loop,
     * before OSv's allocator/scheduler/console exist, and crash. Capturing
     * .init_array.100 into a separate array (outside _init_array_start..end, so
     * premain skips it) lets us run it later, from the main thread once the
     * runtime is up - see _init_array_late in loader.cc. Listed before the main
     * .init_array so the linker assigns these inputs here first. */
    HIDDEN(_init_array_late_start = .);
    .init_array_late : AT(ADDR(.init_array_late) - OSV_KERNEL_VM_SHIFT) {
        KEEP(*(.init_array.100))
        /* Defer the application's own constructors for the same reason. */
        KEEP(*app/*(.init_array .init_array.* .ctors .ctors.*))
    } : text
    HIDDEN(_init_array_late_end = .);
    HIDDEN(_init_array_start = .);
    .init_array : AT(ADDR(.init_array) - OSV_KERNEL_VM_SHIFT) {
        *(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*))
        *(.init_array .ctors) KEEP(*(.init_array .init_array.* .ctors.* .ctors))
    } : text
    HIDDEN(_init_array_end = .);
    . = ALIGN(4096);
    .percpu : AT(ADDR(.percpu) - OSV_KERNEL_VM_SHIFT) {
        HIDDEN(_percpu_start = .);
        *(.percpu)
        . = ALIGN(4096);
        HIDDEN(_percpu_end = .);
    }
    .percpu_workers : AT(ADDR(.percpu_workers) - OSV_KERNEL_VM_SHIFT) {
        HIDDEN(_percpu_workers_start = .);
        *(.percpu_workers)
        HIDDEN(_percpu_workers_end = .);
    }
    . = ALIGN(64);
    .tdata : AT(ADDR(.tdata) - OSV_KERNEL_VM_SHIFT) { *(.tdata .tdata.* .gnu.linkonce.td.*) } :tls :text
    .tbss : AT(ADDR(.tbss) - OSV_KERNEL_VM_SHIFT) {
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        /* Keep the TLS template a multiple of 64 bytes; setup_tcb() asserts it. */
        . = ALIGN(64);
    } :tls :text
    HIDDEN(.tls_template_size = SIZEOF(.tdata) + SIZEOF(.tbss));
    .bss : AT(ADDR(.bss) - OSV_KERNEL_VM_SHIFT) { *(.bss .bss.*) } :text
    . = ALIGN(64);
    HIDDEN(tcb0 = .);
    . = . + .tls_template_size + 256;
    HIDDEN(.edata = .);
    .debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_srcinfo 0 : { *(.debug_srcinfo) }
    .debug_sfnames 0 : { *(.debug_sfnames) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_frame 0 : { *(.debug_frame) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_ranges 0 : { *(.debug_ranges) }
    .debug_line 0 : { *(.debug_line) }
    .debug_loc 0 : { *(.debug_loc) }
    .debug_str 0 : { *(.debug_str) }
    .debug_macinfo 0 : { *(.debug_macinfo) }
    .debug_typenames 0 : { *(.debug_typenames) }
    .debug_varnames 0 : { *(.debug_varnames) }
    .debug_weaknames 0 : { *(.debug_weaknames) }
    .gdb_index 0 : { *(.gdb_index) }
    .comment : { *(.comment) }
}
PHDRS {
	text PT_LOAD FILEHDR PHDRS;
	tls PT_TLS;
	eh_frame PT_GNU_EH_FRAME;
	note PT_NOTE;
}
/* The ELF entry point is the kernel's UEFI entry. The UEFI stub reads this
 * (virtual) address and translates it to the physical entry it jumps to. */
ENTRY(uefi_entry);
