# OSv makefile
#
# Copyright (C) 2015 Cloudius Systems, Ltd.
# This work is open source software, licensed under the terms of the
# BSD license as described in the LICENSE file in the top-level directory.

# Delete the builtin make rules, as if "make -r" was used.
.SUFFIXES:

# Ask make to not delete "intermediate" results, such as the .o in the chain
# .cc -> .o -> .so. Otherwise, during the first build, make considers the .o
# to be intermediate, and deletes it, but the newly-created ".d" files lists
# the ".o" as a target - so it needs to be created again on the second make.
# See commit fac05c95 for a longer explanation.
.SECONDARY:

# Deleting partially-build targets on error should be the default, but it
# isn't, for historical reasons, so we need to turn it on explicitly...
.DELETE_ON_ERROR:

###########################################################################

# Build configuration. OSv is a single-app kernel with one frozen
# configuration, so the options are baked in here (this replaces the former
# kconfig output and the conf/*.mk value files). Arch/mode-independent defaults:
conf_preempt=1
conf_tracing=0
conf_debug_memory=0
# debug level logging (enabled automatically in mode=debug)
conf_logger_debug=0
conf_debug_elf=0
conf_linker_extra_options =
conf_cxx_level=gnu++20
conf_lazy_stack=0
conf_lazy_stack_invariant=0

# The build mode defaults to "release" (optimized build), the other option
# is "debug" (unoptimized build). In the latter the optimizer interferes
# less with the debugging, but the release build is fully debuggable too.
mode=release
ifeq ($(mode),release)
conf_compiler_opt = -O2 -DNDEBUG
else ifeq ($(mode),debug)
# -Wno-uninitialized is clang's spelling (GCC's -Wno-maybe-uninitialized, which
# the old conf/debug.mk used, is rejected by our clang-only toolchain).
conf_compiler_opt = -O0 -Wno-uninitialized
conf_logger_debug=1
else
$(error unsupported mode $(mode))
endif


# By default, detect HOST_CXX's architecture - x64 or aarch64.
# But also allow the user to specify a cross-compiled target architecture
# by setting either "ARCH" or "arch" in the make command line, or the "ARCH"
# environment variable.
HOST_CXX := clang++

detect_arch = $(word 1, $(shell { echo "x64        __x86_64__";  \
                                  echo "aarch64    __aarch64__"; \
                       } | $1 -E -xc - | grep -v '^#' | grep ' 1$$'))

host_arch := $(call detect_arch, $(HOST_CXX))

# As an alternative to setting ARCH or arch, let's allow the user to
# directly set the CROSS_PREFIX environment variable, and learn its arch.
# Pure-LLVM: probe with clang aimed at the triple, not the GNU cross gcc.
ifdef CROSS_PREFIX
    ARCH := $(call detect_arch, clang --target=$(CROSS_PREFIX:%-=%))
endif

ifndef ARCH
    ARCH := $(host_arch)
endif
arch := $(ARCH)

# ARCH_STR is like ARCH, but uses the full name x86_64 instead of x64
ARCH_STR := $(arch:x64=x86_64)

# Arch-specific compiler flags.
ifeq ($(arch),x64)
conf_compiler_cflags = -msse2
else ifeq ($(arch),aarch64)
# -mstrict-align: without it, unaligned access to a stack attr variable with
# stp faults. -ftls-model=local-exec: emit direct TPREL accesses off TPIDR_EL0
# and never TLSDESC relocations, avoiding mixed-TLS-model address mismatches
# (clang, our only compiler, rejects the GCC-only -mtls-dialect= on aarch64).
conf_compiler_cflags = -mstrict-align -ftls-model=local-exec -DAARCH64_PORT_STUB
else
$(error unsupported architecture $(arch))
endif

CROSS_PREFIX ?= $(if $(filter-out $(arch),$(host_arch)),$(ARCH_STR)-linux-gnu-)
# Pure-LLVM toolchain: one clang/clang++ that cross-compiles by target triple
# (no per-arch GNU gcc). When building for a non-host arch, point clang at the
# target with --target=<triple> derived from CROSS_PREFIX (strip trailing '-').
# CROSS_PREFIX only supplies that triple string now; no GNU cross package is
# needed (the build is -nostdinc and links its own archives, so clang never
# consumes the cross GCC's headers/crt/libgcc even when one is installed).
ifneq ($(CROSS_PREFIX),)
CROSS_TARGET = --target=$(CROSS_PREFIX:%-=%)
endif
CXX=clang++ $(CROSS_TARGET)
CC=clang $(CROSS_TARGET)
# Linker is LLVM lld for every arch (no GNU binutils): one multi-target linker
# that auto-detects the ELF arch from its inputs and honours the kernel's GNU-ld
# linker scripts. --no-dependent-libraries ignores the autolink "-lpthread/-ldl"
# hints libc++ embeds for a *-linux-gnu target (OSv has no such libraries; ld.bfd
# silently ignored them, lld errors without this).
LD=ld.lld --no-dependent-libraries
export STRIP=$(shell which llvm-strip 2>/dev/null || which llvm-strip-20 2>/dev/null || echo strip)
OBJCOPY=$(shell which llvm-objcopy 2>/dev/null || which llvm-objcopy-20 2>/dev/null || echo objcopy)
READELF=$(shell which llvm-readelf 2>/dev/null || which llvm-readelf-20 2>/dev/null || echo readelf)

# Our makefile puts all compilation results in a single directory, $(out),
# instead of mixing them with the source code. This allows us to compile
# different variants of the code - for different mode (release or debug)
# or arch (x86 or aarch64) side by side. It also makes "make clean" very
# simple, as all compilation results are in $(out) and can be removed in
# one fell swoop.
out = build/$(mode).$(arch)
outlink = build/$(mode)
outlink2 = build/last

# The kernel configuration is frozen: there is no kconfig, no .config, and no
# generation step. To change a value, edit it here. To remove a subsystem,
# delete its line below and the corresponding source/Makefile wiring. The
# conf_drivers_* variables decide which driver objects are linked below; the
# matching CONF_drivers_* macros for source code live in
# include/osv/drivers_config.h.

# --- tracepoints -----------------------------------------------------------
conf_tracepoints=1
conf_tracepoints_sampler=1
conf_tracepoints_strace=1

# --- core ------------------------------------------------------------------
conf_core_c_wrappers=1
conf_core_commands_runscript=1
conf_core_rcu_defer_queue_size=2000
conf_core_debug_buffer_size=0xc800
conf_core_dynamic_percpu_size=65536

# --- memory ----------------------------------------------------------------
conf_memory_l1_pool_size=512
conf_memory_page_batch_size=32

# --- filesystem ------------------------------------------------------------
conf_fs_max_file_descriptors=0x4000

# --- threads / stacks ------------------------------------------------------
conf_threads_default_kernel_stack_size=65536
conf_threads_default_pthread_stack_size=0x100000
conf_interrupt_stack_size=0x1000

# --- device drivers --------------------------------------------------------
conf_drivers_acpi=1
conf_drivers_pci=1

ifneq ($(MAKECMDGOALS),clean)
$(info Building into $(out))
endif

###########################################################################

# This makefile wraps all commands with the $(quiet) or $(very-quiet) macros
# so that instead of half-a-screen-long command lines we short summaries
# like "CC file.cc". These macros also keep the option of viewing the
# full command lines, if you wish, with "make V=1".
quiet = $(if $V, $1, @echo " $2"; $1)
very-quiet = $(if $V, $1, @$1)

# Both architectures boot via UEFI: the kernel ELF is wrapped into an EFI
# application (loader.efi), then a bootable GPT/ESP disk image (loader.img) is
# built around it - the artifact the clouds ingest and that scripts/run.py and
# the smoke test boot. Building loader.img needs mtools + gdisk (see mkuefi.sh).
all: $(out)/loader.img links
.PHONY: all

links:
	$(call very-quiet, ln -nsf $(notdir $(out)) $(outlink))
	$(call very-quiet, ln -nsf $(notdir $(out)) $(outlink2))
.PHONY: links

# Boot the real bootable disk image (loader.img) as an NVMe disk under QEMU +
# UEFI firmware (OVMF for x64, AAVMF for aarch64) and check it reaches the
# kernel's early banner. This exercises the actual GPT/ESP image the clouds
# ingest, at >= 2 GiB RAM (override with SMOKE_MEM) - unlike the old vvfat-at-
# 512 MiB path, which hid the high-memory boot bugs. Override firmware with
# OVMF_CODE/OVMF_VARS or AAVMF_CODE/AAVMF_VARS; SMOKE_TIMEOUT sets the wait
# (default 30s). Exits non-zero if firmware/QEMU is missing or it fails.
smoke-test: $(out)/loader.img
	scripts/smoke-test.sh $(arch) $(out)/loader.img $(SMOKE_TIMEOUT)
.PHONY: smoke-test

# Remember that "make clean" needs the same parameters that set $(out) in
# the first place, so to clean the output of "make mode=debug" you need to
# do "make mode=debug clean".
clean:
	rm -rf $(out)
	rm -f $(outlink) $(outlink2)
.PHONY: clean

# Manually listing recompilation dependencies in the Makefile (such as which
# object needs to be recompiled when a header changed) is antediluvian.
# Even "makedepend" is old school! The best modern technique for automatic
# dependency generation, which we use here, works like this:
# We note that before the first compilation, we don't need to know these
# dependencies at all, as everything will be compiled anyway. But during
# this compilation, we pass to the compiler a special option (-MD) which
# causes it to also output a file with suffix ".d" listing the dependencies
# discovered during the compilation of that source file. From then on,
# on every compilation we "include" all the ".d" files generated in the
# previous compilation, and create new ".d" when a source file changed
# (and therefore got recompiled).
ifneq ($(MAKECMDGOALS),clean)
include $(shell test -d $(out) && find $(out) -name '*.d')
endif

# Before we can try to build anything in $(out), we need to make sure the
# directory exists. Unfortunately, this is not quite enough, as when we
# compile somedir/somefile.c to $(out)/somedir/somefile.o, we also need
# to make sure $(out)/somedir exists. This is why we have $(makedir) below.
# I wonder if there's a better way of doing this with dependencies, so make
# will only call mkdir for each directory once.
$(out)/%: | $(out)
$(out):
	mkdir -p $(out)

# "tags" is the default output file of ctags, "TAGS" is that of etags
tags TAGS:
	rm -f -- "$@"
	find . -name "*.cc" -o -name "*.hh" -o -name "*.h" -o -name "*.c" |\
		xargs $(if $(filter $@, tags),ctags,etags) -a
.PHONY: tags TAGS

cscope:
	find -name '*.[chS]' -o -name "*.cc" -o -name "*.hh" | cscope -bq -i-
	@echo cscope index created
.PHONY: cscope

###########################################################################

local-includes =
INCLUDES = $(local-includes) -Iarch/$(arch) -I. -Iinclude  -Iarch/common
#
# C++ standard headers come from our own libc++ (CXX_INCLUDES is set below).
# There is no GNU toolchain and no sysroot on either arch: C headers come from
# OSv's own include/api, C++ headers from our libc++, and <unwind.h> from
# libunwind (already on the include path). The aarch64 cross build compiles
# -nostdinc so it does not pick up the host's headers.
ifeq ($(arch),aarch64)
  standard-includes-flag = -nostdinc
else
  standard-includes-flag =
endif

# --- libc++ replaces GNU libstdc++ (LLVM_LIBC_PLAN.md, Phase 8) ---
# Compile the entire C++ surface (kernel + app + vendored boost) against our
# libc++ headers instead of the system GNU C++ headers; the libc++ c++/v1 dir
# itself is added C++-only in CXXFLAGS (with -nostdinc++), so it does not shadow
# C headers (e.g. its wchar.h wrapper) for C compiles.
# libunwind's unwind.h/libunwind.h are needed by backtrace.cc (C++ only, but
# harmless for C) and libc++abi's cxxabi.h is installed alongside under c++/v1.
#
# _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE: libc++'s narrow ctype facet needs a ctype
# "rune table" (the alpha/digit/space... mask bits). Normally libc++ derives the
# layout from the platform libc (__GLIBC__, newlib, BSD, ...). We dropped
# __GLIBC__ and use llvm-libc, which is none of those, so we tell libc++ to use
# its OWN platform-independent table instead (the same default it auto-selects
# for __LLVM_LIBC__). This MUST match the flag in scripts/build-libcxx.sh: the
# mask type/bit values appear in both libc++.a (locale.cpp's classic_table) and
# every consumer's ctype<char>, so the two have to agree.
CXX_INCLUDES = -isystem external/llvm-project/libunwind/include
libcxx-includes = -nostdinc++ -isystem build/libcxx/$(arch)/include/c++/v1 \
                  -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE


INCLUDES += $(boost-includes)
# Starting in Gcc 6, the standard C++ header files (which we do not change)
# must precede in the include path the C header files (which we replace).
# This is explained in https://gcc.gnu.org/bugzilla/show_bug.cgi?id=70722.
# So we are forced to list here (before include/api) the system's default
# C++ include directories, though they are already in the default search path.
INCLUDES += $(CXX_INCLUDES)
INCLUDES += $(pre-include-api)
INCLUDES += -isystem include/api
INCLUDES += -isystem include/api/$(arch)
INCLUDES += -isystem $(out)/gen/include
INCLUDES += $(post-includes-bsd)


ifneq ($(werror),0)
	CFLAGS_WERROR = -Werror
endif
# $(call compiler-flag, -ffoo, option)
#     returns option if the compiler accepts -ffoo (probed against an empty
#     translation unit), empty otherwise
compiler-flag = $(shell $(CXX) $(CFLAGS_WERROR) $1 -x c++ -o /dev/null -c /dev/null > /dev/null 2>&1 && echo $2)

# clang rejects __attribute__((cold)) on a label, so HAVE_ATTR_COLD_LABEL is
# never defined and ATTR_COLD_LABEL (include/osv/compiler.h) expands to nothing.
compiler-specific :=
fno-instrument-functions := $(call compiler-flag, -fno-instrument-functions, -fno-instrument-functions)

# Flags that exist in GCC but not Clang - probed so the build works with either compiler.
wno-class-memaccess       := $(call compiler-flag, -Wno-class-memaccess,       -Wno-class-memaccess)
wno-stringop-truncation   := $(call compiler-flag, -Wno-stringop-truncation,   -Wno-stringop-truncation)
wno-dangling-pointer      := $(call compiler-flag, -Wno-dangling-pointer,      -Wno-dangling-pointer)
wno-unused-private-field  := $(call compiler-flag, -Wno-unused-private-field,  -Wno-unused-private-field)
# GCC: $(wno-maybe-uninitialized); Clang equivalent: -Wno-uninitialized
wno-maybe-uninitialized := $(call compiler-flag, -Wno-maybe-uninitialized, -Wno-maybe-uninitialized)
ifeq ($(wno-maybe-uninitialized),)
wno-maybe-uninitialized := $(call compiler-flag, -Wno-uninitialized, -Wno-uninitialized)
endif

source-dialects = -D_GNU_SOURCE

# libc has its own source dialect control
$(out)/libc/%.o: source-dialects =

# do not hide symbols in libc because it has its own hiding mechanism

kernel-defines = -D_KERNEL $(source-dialects)

# This play the same role as "_KERNEL", but _KERNEL unfortunately is too
# overloaded. A lot of files will expect it to be set no matter what, specially
# in headers. "userspace" inclusion of such headers is valid, and lacking
# _KERNEL will make them fail to compile. That is specially true for the BSD
# imported stuff like ZFS commands.
#
# To add something to the kernel build, you can write for your object:
#
#   mydir/*.o COMMON += <MY_STUFF>
#
# To add something that will *not* be part of the main kernel, you can do:
#
#   mydir/*.o EXTRA_FLAGS = <MY_STUFF>
ifeq ($(arch),x64)
EXTRA_FLAGS = -D__OSV_CORE__ -DOSV_KERNEL_BASE=$(kernel_base) -DOSV_KERNEL_VM_BASE=$(kernel_vm_base) \
	-DOSV_KERNEL_VM_SHIFT=$(kernel_vm_shift)
else
EXTRA_FLAGS = -D__OSV_CORE__ -DOSV_KERNEL_VM_BASE=$(kernel_vm_base)
endif
EXTRA_LIBS =
COMMON = $(autodepend) -g -Wall -Wno-pointer-arith $(CFLAGS_WERROR) -Wformat=0 -Wno-format-security \
	-D __BSD_VISIBLE=1 -U _FORTIFY_SOURCE -fno-stack-protector $(INCLUDES) \
	$(kernel-defines) \
	-fno-omit-frame-pointer $(compiler-specific) \
	$(conf_compiler_cflags) $(conf_compiler_opt) $(tracing-flags) \
	-D__OSV__ -DARCH_STRING=$(ARCH_STR) $(EXTRA_FLAGS)
COMMON += $(standard-includes-flag)
COMMON += $(wno-unused-private-field)

tracing-flags-0 =
tracing-excl := $(shell $(CXX) $(CFLAGS_WERROR) -finstrument-functions-exclude-file-list=x -x c++ -o /dev/null -c /dev/null > /dev/null 2>&1 && \
    echo '-finstrument-functions-exclude-file-list=c++,trace.cc,trace.hh,align.hh,mmintrin.h')
tracing-flags-1 = -finstrument-functions $(tracing-excl)
tracing-flags = $(tracing-flags-$(conf_tracing))




gcc-opt-Og := $(call compiler-flag, -Og, -Og)

# The kernel (with the app statically linked in) is a single fixed-address
# executable, so use the local-exec TLS model: thread-local accesses become
# immediate offsets resolved at link time rather than GOT-based TPOFF64 dynamic
# relocations that would need processing at boot.
tls-model = -ftls-model=local-exec
CXXFLAGS = -std=$(conf_cxx_level) $(libcxx-includes) $(COMMON) $(tls-model)
CFLAGS = -std=gnu99 $(COMMON) $(tls-model)

# should be limited to files under libc/ eventually
CFLAGS += -I libc/stdio -I libc/internal -I libc/arch/$(arch) \
	-Wno-missing-braces -Wno-parentheses -Wno-unused-but-set-variable
# musl uses "str"+int idiom to skip first character conditionally
$(out)/libc/%.o: CFLAGS += -Wno-string-plus-int
# musl calls fabs() on a long double in a few spots; suppress Clang's narrowing
# warning rather than patch the vendored musl source (probed; GCC lacks the flag)
wno-absolute-value := $(call compiler-flag, -Wno-absolute-value, -Wno-absolute-value)
$(out)/libc/%.o: CFLAGS += $(wno-absolute-value)

ASFLAGS = -g $(autodepend) -D__ASSEMBLY__
# Assembly files use COMMON (no -std=gnu++XX) so Clang does not apply C++
# lexing to assembly comments (which breaks on apostrophes like "let's").
# Many COMMON flags (optimization, -isystem, defines) don't apply to assembly;
# Clang flags them under -Wunused-command-line-argument, so silence that.
wno-unused-cli-arg := $(call compiler-flag, -Wno-unused-command-line-argument, -Wno-unused-command-line-argument)
ASCOMPILE = $(CXX) $(COMMON) $(wno-unused-cli-arg)

$(out)/fs/vfs/main.o: CXXFLAGS += -Wno-sign-compare -Wno-write-strings


makedir = $(call very-quiet, mkdir -p $(dir $@))


# Order-only dep on the libc++ build: every C++ TU includes our libc++ headers
# (libcxx-includes in CXXFLAGS), so they must exist before any .cc compiles. In
# a from-scratch -j build the libc++ build would otherwise race the kernel
# compiles (and fail with e.g. "'functional' file not found").
$(out)/%.o: %.cc | generated-headers $(out)/.libcxx-built
	$(makedir)
	$(call quiet, $(CXX) $(CXXFLAGS) -c -o $@ $<, CXX $*.cc)

$(out)/%.o: %.c | generated-headers
	$(makedir)
	$(call quiet, $(CC) $(CFLAGS) -c -o $@ $<, CC $*.c)

$(out)/%.o: %.S
	$(makedir)
	$(call quiet, $(ASCOMPILE) $(ASFLAGS) -c -o $@ $<, AS $*.S)

$(out)/%.o: %.s
	$(makedir)
	$(call quiet, $(ASCOMPILE) $(ASFLAGS) -c -o $@ $<, AS $*.s)


autodepend = -MD -MT $@ -MP

tools :=

$(out)/loader-stripped.elf: $(out)/loader.elf
	$(call quiet, $(STRIP) $(out)/loader.elf -o $(out)/loader-stripped.elf, STRIP loader.elf -> loader-stripped.elf )

# ---- UEFI boot image -------------------------------------------------------
# Wrap the kernel ELF into a freestanding PE/COFF EFI application (boot/uefi/)
# that the firmware loads from the EFI System Partition. This is the single
# boot path on every architecture and the only one the public clouds support.
# efi_target / efi_boot_name are set in the per-arch blocks below.
EFI_CXXFLAGS = --target=$(efi_target) -std=$(conf_cxx_level) -ffreestanding \
	-fno-stack-protector -fno-builtin -fshort-wchar -nostdinc++ \
	-fno-exceptions -fno-rtti -Wall -Werror -Iboot/uefi -Iinclude \
	-DOSV_KERNEL_VM_SHIFT=$(kernel_vm_shift) $(efi_arch_cxxflags)

$(out)/boot/uefi/stub.o: boot/uefi/stub.cc boot/uefi/efi.hh include/osv/boot-info.hh
	$(makedir)
	$(call quiet, clang++ $(EFI_CXXFLAGS) -c -o $@ $<, CXX-EFI stub.cc)

$(out)/boot/uefi/kernel-blob.o: boot/uefi/kernel-blob.S $(out)/loader-stripped.elf
	$(makedir)
	$(call quiet, clang --target=$(efi_target) -c \
		-DKERNEL_ELF_FILE='"$(out)/loader-stripped.elf"' -o $@ $<, AS-EFI kernel-blob.S)

$(out)/loader.efi: $(out)/boot/uefi/stub.o $(out)/boot/uefi/kernel-blob.o
	$(call quiet, clang --target=$(efi_target) -fuse-ld=lld -nostdlib \
		-Xlinker -entry:efi_main -Xlinker -subsystem:efi_application \
		-o $@ $^, LINK loader.efi)

$(out)/loader.img: $(out)/loader.efi scripts/mkuefi.sh
	$(call quiet, scripts/mkuefi.sh $@ $(out)/loader.efi $(efi_boot_name), MKUEFI $@)

ifeq ($(arch),x64)

# kernel_base is the fixed physical address the UEFI stub loads the kernel ELF
# at (it matches the hard-coded boot page tables); kernel_vm_base is its link
# address in virtual memory.
kernel_base := 0x200000
kernel_vm_base := 0x40200000

kernel_vm_shift := $(shell printf "0x%X" $(shell expr $$(( $(kernel_vm_base) - $(kernel_base) )) ))

# UEFI image parameters: PE target triple and the firmware removable-media
# default filename for this architecture.
efi_target := x86_64-unknown-windows
efi_boot_name := BOOTX64.EFI
efi_arch_cxxflags := -mno-red-zone

endif # x64

ifeq ($(arch),aarch64)

kernel_vm_base := 0xfc0080000 #63GB

# Fixed physical load base and shift for the UEFI stub. The boot page tables
# anchor the kernel window at the 63rd GiB (0xfc0000000), so the shift that
# relates a virtual address to the physical address the stub loads it at is
# (0xfc0000000 - kernel_base), with kernel_base 2 MiB-aligned in low RAM.
kernel_base := 0x40200000
kernel_vm_shift := $(shell printf "0x%X" $(shell expr $$(( 0xfc0000000 - $(kernel_base) )) ))

efi_target := aarch64-unknown-windows
efi_boot_name := BOOTAA64.EFI
efi_arch_cxxflags :=

endif # aarch64


# bsd headers are also included from non-bsd code; suppress the attribute/
# extern-C mismatches those pull in (probed, since GCC lacks the flag names).
wno-extern-c-compat    := $(call compiler-flag, -Wno-extern-c-compat,   -Wno-extern-c-compat)
wno-ignored-attributes := $(call compiler-flag, -Wno-ignored-attributes, -Wno-ignored-attributes)
wno-sometimes-uninitialized := $(call compiler-flag, -Wno-sometimes-uninitialized, -Wno-sometimes-uninitialized)
COMMON += $(wno-extern-c-compat) $(wno-ignored-attributes) $(wno-sometimes-uninitialized)



drivers :=
drivers += core/mmu.o
drivers += arch/$(arch)/early-console.o
drivers += drivers/console.o
drivers += drivers/console-multiplexer.o
drivers += drivers/console-driver.o
drivers += drivers/clock.o
drivers += drivers/isa-serial-base.o
drivers += drivers/random.o
drivers += drivers/device.o
ifeq ($(conf_drivers_pci),1)
drivers += drivers/pci-generic.o
drivers += drivers/pci-device.o
drivers += drivers/pci-function.o
drivers += drivers/pci-bridge.o
drivers += drivers/msi.o
endif
drivers += drivers/driver.o

# ACPI is the device-discovery model on both architectures under UEFI boot.
ifeq ($(conf_drivers_acpi),1)
drivers += drivers/acpi.o
endif

ifeq ($(arch),x64)
drivers += drivers/isa-serial.o
drivers += arch/$(arch)/pvclock-abi.o

drivers += drivers/kvmclock.o
endif # x64

ifeq ($(arch),aarch64)
drivers += drivers/mmio-isa-serial.o
drivers += drivers/pl011.o
endif # aarch64

ifeq ($(conf_tracepoints),1)
objects += arch/$(arch)/arch-trace.o
endif
# The application is statically linked into the kernel image and entered via
# osv_app_main(). There is no separate app .so or filesystem image.
#
# The build mode select which application is linked in. Each application lives
# in its own directory with a Makefile fragment that lists its objects in
# $(app-objects); the kernel compiles and links them with its own flags.
#   make            -> the user application   (app/)
#   make app=test   -> the test application   (test/)
app ?= app
include $(app)/Makefile
objects += $(app-objects)

# Record the selected app mode so that switching between `make` and
# `make app=tests` forces the kernel to relink (the set of linked-in app
# objects changes, which plain timestamp checking would not notice).
app_mode_dep = $(out)/app_mode.last
.PHONY: app_mode_phony
$(app_mode_dep): app_mode_phony
	$(call very-quiet, $(makedir))
	@if [ "$$(cat $(app_mode_dep) 2>/dev/null)" != "$(app)" ]; then \
		echo -n "$(app)" > $(app_mode_dep); \
	fi
# Minimal boot-time self-relocator (replaces the relocation half of the old
# ELF loader). Per-arch: the relocation-type switch differs (x64 vs aarch64).
objects += arch/$(arch)/relocate.o
objects += arch/$(arch)/arch-setup.o
objects += arch/$(arch)/signal.o
objects += arch/$(arch)/arch-cpu.o
objects += arch/$(arch)/backtrace.o
objects += arch/$(arch)/smp.o
objects += arch/$(arch)/tlsdesc.o
objects += arch/$(arch)/entry.o
objects += arch/$(arch)/mmu.o
objects += arch/$(arch)/exceptions.o
objects += arch/$(arch)/dump.o
objects += arch/$(arch)/cpuid.o
objects += arch/$(arch)/firmware.o
objects += arch/$(arch)/hypervisor.o
objects += arch/$(arch)/interrupt.o
ifeq ($(conf_drivers_pci),1)
objects += arch/$(arch)/pci.o
objects += arch/$(arch)/msi.o
endif
objects += arch/$(arch)/power.o
objects += arch/$(arch)/feexcept.o

ifeq ($(arch),aarch64)
objects += arch/$(arch)/psci.o
objects += arch/$(arch)/arm-clock.o
objects += arch/$(arch)/gic-common.o
objects += arch/$(arch)/gic-v2.o
objects += arch/$(arch)/gic-v3.o
# cpuid.cc uses array designators ([HWCAP_BIT_FP] = ...) - a C99 extension in
# C++ that Clang flags under -Werror; the initializer order is deliberate.
$(out)/arch/aarch64/cpuid.o: CXXFLAGS += -Wno-c99-designator
objects += arch/$(arch)/sched.o
endif

ifeq ($(arch),x64)
objects += arch/x64/dmi.o
objects += arch/x64/ioapic.o
objects += arch/x64/apic.o
objects += arch/x64/apic-clock.o
objects += arch/x64/hyperv-clock.o
endif # x64

objects += core/spinlock.o
objects += core/lfmutex.o
objects += core/rwlock.o
objects += core/semaphore.o
objects += core/condvar.o
objects += core/debug.o
objects += core/rcu.o
objects += core/mempool.o
ifeq ($(conf_memory_tracker),1)
objects += core/alloctracker.o
endif
objects += core/printf.o
ifeq ($(conf_tracepoints_sampler),1)
objects += core/sampler.o
endif

objects += core/sched.o
objects += core/mmio.o
objects += core/kprintf.o
ifeq ($(conf_tracepoints),1)
objects += core/trace.o
ifeq ($(conf_tracepoints_strace),1)
objects += core/strace.o
endif
endif
objects += core/power.o
objects += core/percpu.o
objects += core/percpu-worker.o
objects += core/shutdown.o
objects += core/version.o
objects += core/waitqueue.o
objects += core/chart.o
objects += core/demangle.o

#include $(src)/libc/build.mk:
libc =

libc += internal/libc.o



libc += env/__environ.o




# Issue #867: Gcc 4.8.4 has a bug where it optimizes the trivial round-
# related functions incorrectly - it appears to convert calls to any
# function called round() to calls to a function called lround() -
# and similarly for roundf() and roundl().
# None of the specific "-fno-*" options disable this buggy optimization,
# unfortunately. The simplest workaround is to just disable optimization
# for the affected files.

libc += misc/getopt.o
libc += misc/getopt_long.o
libc += misc/backtrace.o




libc += prng/random.o
libc += random.o


libc += arch/$(arch)/setjmp/sigsetjmp.o
libc += signal/block.o
libc += signal/siglongjmp.o
ifeq ($(arch),x64)
libc += arch/$(arch)/ucontext/getcontext.o
libc += arch/$(arch)/ucontext/setcontext.o
libc += arch/$(arch)/ucontext/start_context.o
libc += arch/$(arch)/ucontext/ucontext.o
endif


$(out)/libc/stdio/vfprintf.o: COMMON += $(wno-maybe-uninitialized)
$(out)/libc/stdio/vfscanf.o: COMMON += $(wno-maybe-uninitialized)

libc += string/explicit_bzero.o
libc += string/strerror_r.o
libc += string/stresep.o




libc += unistd/sync.o
libc += unistd/getpgid.o
libc += unistd/setpgid.o
libc += unistd/getpgrp.o
libc += unistd/getppid.o
libc += unistd/getsid.o
libc += unistd/ttyname_r.o


libc += pthread.o
libc += pthread_barrier.o
libc += libc.o
libc += dlfcn.o
libc += io.o
# Stdio: the FILE implementation (printf/fopen/fread/scanf/...) comes from the
# llvm-libc archive; these are the OSv seam (console-backed std streams + FILE
# stubs) and the printf-extension stubs.
libc += stdio/llvm_stdio.o
libc += time.o
libc += signal.o
libc += mman.o
libc += sem.o
# pipe, af_local (unix sockets), mount, eventfd, timerfd, shm and inotify all
# depend on the (removed) file-descriptor table and filesystem.
libc += user.o
libc += resource.o
libc += cxa_thread_atexit.o
libc += cpu_set.o
libc += malloc_hooks.o




# There is no filesystem: the kernel has no VFS, no fd table and no on-disk or
# in-memory file systems. Minimal console-backed stdio lives in libc/io.cc.
objects += $(addprefix libc/, $(libc))


# All three runtime builds (compiler-rt, llvm-libc, libc++) share the one
# external/llvm-project checkout. Do the clone + sparse-checkout ONCE here, with
# the union of the paths they each need, so a parallel `make -j` cannot race
# several `git clone`s into the same directory (nor have the per-script
# sparse-checkout calls clobber each other). The build scripts keep their own
# clone/sparse logic for standalone use; once this stamp has populated the tree
# their guards see the directories and skip it. The tag must match
# scripts/build-{compiler-rt,llvm-libc,libcxx}.sh.
llvm_project_tag = llvmorg-22.1.7
llvm_project_stamp = external/llvm-project/.sparse-ready
$(llvm_project_stamp):
	@echo "  LLVM-PROJECT external/llvm-project ($(llvm_project_tag))"
	$(call very-quiet, if [ ! -d external/llvm-project/.git ]; then git clone --depth 1 --branch $(llvm_project_tag) --filter=blob:none --sparse https://github.com/llvm/llvm-project.git external/llvm-project; fi)
	$(call very-quiet, git -C external/llvm-project sparse-checkout set libc libcxx libcxxabi libunwind compiler-rt third-party runtimes cmake llvm/cmake llvm/utils)
	$(call very-quiet, touch $@)

# Compiler builtins (soft-int helpers like __umodti3) come from LLVM compiler-rt,
# not GNU libgcc - no GCC anywhere in the toolchain. The C++ exception unwinder
# that used to come from libgcc_eh.a is now libunwind (Phase 8.7), and the C++
# runtime that used to come from libstdc++.a is now libc++/libc++abi (Phase 8.7).
#
# The host clang package only ships the builtins for the host arch, so for a
# native build we just point at the installed archive, but a cross build
# (aarch64) builds them from the pinned llvm-project via scripts/build-compiler-rt.sh
# (auto-invoked, no manual step), exactly like llvm-libc and libc++.
ifeq ($(filter-out $(arch),$(host_arch)),)
compiler_rt_builtins := $(shell $(CC) --rtlib=compiler-rt --print-libgcc-file-name)
ifeq ($(filter /%,$(compiler_rt_builtins)),)
    $(error Error: LLVM compiler-rt builtins not found. Install the clang/compiler-rt runtime.)
endif
compiler_rt_dep =
else
compiler_rt_builtins = build/compiler-rt/$(arch)/lib/libclang_rt.builtins.a
$(out)/.compiler-rt-built: scripts/build-compiler-rt.sh | $(llvm_project_stamp)
	$(call quiet, scripts/build-compiler-rt.sh $(arch), COMPILER-RT $(arch))
	$(call very-quiet, touch $@)
$(compiler_rt_builtins): $(out)/.compiler-rt-built
compiler_rt_dep = $(compiler_rt_builtins)
endif

#Allow user specify non-default location of boost
# Boost is vendored under external/boost (a header-only subset, see that
# directory's README) so the build does not depend on a system-installed Boost.
# Boost.System is header-only in modern Boost, so no Boost library is linked.
boost-includes = -isystem external/boost
boost-libs :=

# nfs/ext null vfsops went with the filesystem.


# The OSv kernel is linked into an ordinary, non-PIE, executable, so there is no point in compiling
# with -fPIC or -fpie and objects that can be linked into a PIE. On the contrary, PIE-compatible objects
# have overheads and can cause problems (see issue #1112). Recently, on some systems gcc's
# default was changed to use -fpie, so we need to undo this default by explicitly specifying -fno-pie.
$(objects:%=$(out)/%) $(drivers:%=$(out)/%) $(out)/arch/$(arch)/boot.o $(out)/loader.o $(out)/runtime.o: COMMON += -fno-pie

stage1_targets = $(out)/arch/$(arch)/boot.o $(out)/loader.o $(out)/runtime.o $(drivers:%=$(out)/%) $(objects:%=$(out)/%)
stage1: $(stage1_targets) links
.PHONY: stage1


# The C++ runtime (libc++ / libc++abi / libunwind) is linked on demand inside a
# --start-group, not whole-archived: the slim kernel links its single app at
# build time and exports no dynamic C++ ABI, so only referenced runtime objects
# are needed. The group resolves the libc++ <-> libc++abi cycle, and pulling
# each symbol once avoids the multiple-definition clash from the libunwind
# objects that the build bundles into all three archives.
linker_archives_options = --no-whole-archive --start-group $(libcxx_archives) --end-group $(boost-libs) $(compiler_rt_builtins)

# LLVM libc (LLVM_LIBC_PLAN.md): llvm-libc is the generic libc, linked trailing
# so it supplies any symbol the kernel and the OSv libc objects do not define.
# The archive is a no-syscall baremetal libc built automatically by
# scripts/build-llvm-libc.sh (x86/arm only, no manual step). The functions
# llvm-libc 22.1.7 lacks (the stdio FILE glue, x87 long-double helpers, a few
# env/time/string/multibyte leaves) are now OSv-owned, musl-derived sources
# under libc/ - the musl submodule was deleted in Phase 8.13.
llvm_libc_libdir = build/llvm-libc/$(arch)/libc/lib
llvm_libc_archives = $(llvm_libc_libdir)/libc.a $(llvm_libc_libdir)/libm.a
$(out)/.llvm-libc-built: scripts/build-llvm-libc.sh external/llvm-libc-config/entrypoints.txt external/llvm-libc-config/config.json external/llvm-libc-config/headers.txt | $(llvm_project_stamp)
	$(call quiet, scripts/build-llvm-libc.sh $(arch), LLVM-LIBC $(arch))
	$(call very-quiet, touch $@)
$(llvm_libc_archives): $(out)/.llvm-libc-built
llvm_libc_dep = $(llvm_libc_archives)
# compiler-rt builtins repeat after the llvm-libc archives: their members need
# its soft-int helpers (e.g. __umodti3), and ld resolves archives in order.
linker_archives_options += $(llvm_libc_archives) $(compiler_rt_builtins)

# libc++ / libc++abi / libunwind replace the host GNU libstdc++.a and the libgcc
# exception unwinder (LLVM_LIBC_PLAN.md, Phase 8). Built by scripts/build-libcxx.sh
# (x86/arm only, no manual step) from the same pinned llvm-project, localization
# OFF. They are linked (in the --start-group in linker_archives_options above) in
# place of libstdc++.a/libgcc_eh.a; libunwind supplies the C++ exception unwinder
# (OSv uses C++ exceptions) and locates the kernel's .eh_frame via OSv's
# dl_iterate_phdr.
libcxx_libdir = build/libcxx/$(arch)/lib
libcxx_archives = $(libcxx_libdir)/libc++.a $(libcxx_libdir)/libc++abi.a $(libcxx_libdir)/libunwind.a
$(out)/.libcxx-built: scripts/build-libcxx.sh | $(llvm_project_stamp)
	$(call quiet, scripts/build-libcxx.sh $(arch), LIBCXX $(arch))
	$(call very-quiet, touch $@)
$(libcxx_archives): $(out)/.libcxx-built
libcxx_dep = $(libcxx_archives)

# Former musl gaps, now resolved (Phase 8.12). Most are supplied by llvm-libc
# (the narrow ctype family, and the bulk of <time.h>: asctime/ctime/gmtime/
# localtime/mktime/strftime/difftime and the _r variants) - just deleting the
# `musl +=` lines lets the trailing archive fill them. The handful llvm-libc
# lacks were vendored into libc/ below. Dropped as dead/unreferenced: the
# __ctype_*_loc glibc table accessors, the whole temp/ family (mkstemp/mkdtemp/
# mktemp/__randname - no filesystem), and time/{strptime,timegm,getdate,ftime,
# __secs_to_tm,__tm_to_secs}.
# x87/binary128 long-double survivors llvm-libc lacks (__fpclassifyl/__signbitl
# for vfprintf %Lf, logl). strchrnul + mbsinit now come from the llvm-libc
# archive (entrypoints libc.src.string.strchrnul / libc.src.wchar.mbsinit).
libc += math/longdouble.o
libc += string/strsignal.o
# OSv-owned env management (getenv/setenv/putenv/unsetenv) - getenv on baremetal
# is OS-coupled, so llvm-libc cannot supply it. time() routes to OSv's clock.
libc += env/env.o
libc += time/time_shims.o

ifeq ($(arch),aarch64)
def_symbols = --defsym=OSV_KERNEL_VM_BASE=$(kernel_vm_base)
else
def_symbols = --defsym=OSV_KERNEL_BASE=$(kernel_base) \
              --defsym=OSV_KERNEL_VM_BASE=$(kernel_vm_base) \
              --defsym=OSV_KERNEL_VM_SHIFT=$(kernel_vm_shift)
endif

$(out)/loader.elf: $(stage1_targets) arch/$(arch)/loader.ld $(app_mode_dep) $(llvm_libc_dep) $(libcxx_dep) $(compiler_rt_dep)
	$(call quiet, $(LD) -o $@ $(def_symbols) \
		-static --eh-frame-hdr -L$(out)/arch/$(arch) \
            $(patsubst %.ld,-T %.ld,$(filter-out $(app_mode_dep) $(llvm_libc_dep) $(libcxx_dep) $(compiler_rt_dep),$^)) \
	    $(linker_archives_options) $(conf_linker_extra_options), \
		LINK loader.elf)


################################################################################
# The dependencies on header files are automatically generated only after the
# first compilation, as explained above. However, header files generated by
# the Makefile are special, in that they need to be created even *before* the
# first compilation. Moreover, some (namely version.h) need to perhaps be
# re-created on every compilation. "generated-headers" is used as an order-
# only dependency on C compilation rules above, so we don't try to compile
# C code before generating these headers.
generated-headers: $(out)/gen/include/bits/alltypes.h perhaps-modify-version-h
.PHONY: generated-headers

# While other generated headers only need to be generated once, version.h
# should be recreated on every compilation. To avoid a cascade of
# recompilation, the rule below makes sure not to modify version.h's timestamp
# if the version hasn't changed.
# conf/Makefile used to pre-create $(out)/gen/include/osv when generating the
# kconfig headers; with kconfig gone the generated-headers rules create it.
perhaps-modify-version-h:
	$(call very-quiet, mkdir -p $(out)/gen/include/osv)
	$(call quiet, sh scripts/gen-version-header $(out)/gen/include/osv/version.h, GEN gen/include/osv/version.h)
.PHONY: perhaps-modify-version-h

# The CONF_drivers_* macros used by source code (e.g. arch-setup.cc) are frozen
# in the checked-in include/osv/drivers_config.h; nothing is generated here.

$(out)/gen/include/bits/alltypes.h: include/api/$(arch)/bits/alltypes.h.sh
	$(makedir)
	$(call quiet, sh $^ > $@, GEN $@)


################################################################################




