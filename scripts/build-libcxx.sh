#!/usr/bin/env bash
# Build LLVM's C++ runtime (libc++ + libc++abi + libunwind) as static archives
# for the OSv kernel, replacing the host GNU libstdc++.a.
#
# Phase 8 of LLVM_LIBC_PLAN.md, the libstdc++ -> libc++ swap. Built against the
# same pinned llvm-project as llvm-libc (external/llvm-project).
#
# Localization is ON, but narrow-only: a single static "C"/US locale, no wide
# characters, no Unicode, no <filesystem>. This is what gives us <iostream>
# (std::cout/std::endl) and the num_put/num_get/ctype facets. libc++'s locale
# backend is the plain POSIX one (__locale_dir/support/linux.h, selected by the
# linux triple): it maps every facet operation onto standard *_l functions
# (toupper_l, strtod_l, strcoll_l, strftime_l, ...) plus newlocale/uselocale/
# localeconv, all of which llvm-libc provides natively. There is no glibc compat
# shim and no musl locale machinery underneath - just llvm-libc.
#
# Wide characters / Unicode stay OFF on purpose: those are the only consumers of
# the wide-ctype families, and we don't need them. Keeping them off keeps the
# locale surface to the narrow "C" locale.
#
# Produces, for the requested arch:
#   build/libcxx/<arch>/lib/{libc++.a,libc++abi.a,libunwind.a}
# Invoked automatically by the Makefile (a prerequisite of the kernel link).
# Only x86_64 ("x64") and aarch64 ("arm") are supported.

set -euo pipefail

# Run a command, tee to a log file, and display each output line by overwriting
# the current terminal line.  On failure, print the tail of the log.
_progress_run() {
    local log="$1"; shift
    "$@" 2>&1 | tee "$log" | while IFS= read -r line; do
        printf '\r\033[K  %.*s' "$(( ${COLUMNS:-120} - 4 ))" "$line" >&2
    done
    local rc=${PIPESTATUS[0]}
    printf '\r\033[K' >&2
    [ "$rc" -eq 0 ] || tail -30 "$log" >&2
    return "$rc"
}

osv_arch="${1:-x64}"
case "$osv_arch" in
    x64|x86_64|x86)    osv_arch=x64;     llvm_arch=x86_64 ;;
    aarch64|arm|arm64) osv_arch=aarch64; llvm_arch=aarch64 ;;
    *)
        echo "build-libcxx.sh: unsupported arch '$osv_arch' (only x86/arm)" >&2
        exit 2 ;;
esac

LLVM_TAG=llvmorg-22.1.7
OSV_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_DIR="$OSV_ROOT/external/llvm-project"
BUILD_DIR="$OSV_ROOT/build/libcxx/$osv_arch"

# 1. fetch (shares the llvm-libc checkout; just widen the sparse set)
if [ ! -d "$LLVM_DIR/libcxx" ]; then
    if [ ! -d "$LLVM_DIR/.git" ]; then
        git clone --depth 1 --branch "$LLVM_TAG" --filter=blob:none --sparse \
            https://github.com/llvm/llvm-project.git "$LLVM_DIR"
    fi
    git -C "$LLVM_DIR" sparse-checkout add libcxx libcxxabi libunwind runtimes cmake \
        llvm/cmake llvm/utils libc
fi

# OSv has no syscall ABI. libc++abi's __cxa_guard uses the mutex (not futex)
# implementation here, but its thread-id helper (used only for recursive-init
# detection) still calls syscall(SYS_gettid). Force the no-syscall nullptr
# branch so libc++abi references no syscall() at all. Idempotent.
GUARD="$LLVM_DIR/libcxxabi/src/cxa_guard_impl.h"
if grep -q '^#elif defined(SYS_gettid) && _LIBCPP_HAS_THREAD_API_PTHREAD' "$GUARD"; then
    sed -i 's@^#elif defined(SYS_gettid) && _LIBCPP_HAS_THREAD_API_PTHREAD@#elif 0 // OSv: no gettid syscall; force PlatformThreadID = nullptr@' "$GUARD"
fi

# Same reason on the unwinder: on aarch64 (et al.) libunwind enables Linux
# sigreturn-frame detection, which calls syscall() to read the trampoline. OSv
# has no Linux signal frames on the stack to unwind through, and no syscall ABI,
# so disable it - the generic setInfoForSigReturn() returns false (no syscall).
UNWCURSOR="$LLVM_DIR/libunwind/src/UnwindCursor.hpp"
if grep -q '^#define _LIBUNWIND_CHECK_LINUX_SIGRETURN 1' "$UNWCURSOR"; then
    sed -i 's@^#define _LIBUNWIND_CHECK_LINUX_SIGRETURN 1@// OSv: no Linux signal frames / no syscall ABI - disable sigreturn unwinding@' "$UNWCURSOR"
fi

# 2. configure + build (static, narrow "C"-locale localization, exceptions+RTTI
#    on, llvm unwinder)
# Codegen must match the kernel (see COMMON in the top-level Makefile).
#
# Header environment must also match the kernel: put OSv's own libc headers
# (include/api, musl-derived) ahead of the host's so the standard C headers
# resolve to ours, exactly as the kernel compiles its C++. Without this the
# build picks up the host glibc <stdlib.h>, whose C23 mode (_GNU_SOURCE ->
# _ISOC23_SOURCE) redirects strtol/strtoul/strtoll/strtoull to __isoc23_* and
# leaves those undefined references in libc++.a (the last glibc dependency).
# include/api also supplies unistd/pthread/dlfcn/link/elf/sys headers that
# libc++abi and libunwind need; the host headers stay only as a fallback.
# include/api/stdint.h et al. pull the generated bits/alltypes.h; generate it
# into a private dir (same recipe as the kernel Makefile) so this script stays
# self-contained and independent of the kernel out-dir.
GEN_INC="$BUILD_DIR/gen/include"
mkdir -p "$GEN_INC/bits"
sh "$OSV_ROOT/include/api/$osv_arch/bits/alltypes.h.sh" > "$GEN_INC/bits/alltypes.h"
# atomic.cpp needs <linux/futex.h> for std::atomic's wait backend.
# On systems without Linux kernel headers (e.g. Nix devShell) this isn't on
# the default include path.  GEN_INC is already -isystem'd, so a stub there
# is found without affecting the kernel's own header search order.
mkdir -p "$GEN_INC/linux"
cat > "$GEN_INC/linux/futex.h" << 'EOF'
#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_WAIT_PRIVATE   (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE   (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_CLOCK_REALTIME 256
#endif
EOF
OSV_HEADERS="-isystem $OSV_ROOT/include/api -isystem $OSV_ROOT/include/api/$osv_arch -isystem $GEN_INC"
# _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE: use libc++'s own platform-independent
# ctype rune table (alpha/digit/space... mask bits) rather than a platform
# libc's. We dropped __GLIBC__ and use llvm-libc (which libc++ doesn't recognise
# as a rune-table provider here), so without this libc++'s <__locale> hits its
# "unknown rune table for this platform" #error. The kernel build defines the
# same macro (Makefile, libcxx-includes) so locale.cpp's classic_table here and
# every ctype<char> consumer there agree on the mask layout.
KERNEL_FLAGS="-fno-pie -fno-stack-protector -ftls-model=local-exec -fno-omit-frame-pointer -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE $OSV_HEADERS"

# Pure-LLVM cross-compile when targeting a non-host arch: one clang driven at the
# target triple (no GNU cross toolchain). cmake needs the cross vars explicitly.
# CMAKE_*_COMPILER_TARGET supplies --target for C/CXX/ASM, so we do NOT also put
# it in *_FLAGS. TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY makes cmake's compiler
# check compile-only (we have no GNU sysroot / crt*.o to link an executable, and
# don't want one - the runtimes are freestanding static archives).
CROSS_CMAKE=
if [ "$llvm_arch" != "$(uname -m)" ]; then
    CROSS_CMAKE="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$llvm_arch \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_C_COMPILER_TARGET=$llvm_arch-linux-gnu \
        -DCMAKE_CXX_COMPILER_TARGET=$llvm_arch-linux-gnu \
        -DCMAKE_ASM_COMPILER_TARGET=$llvm_arch-linux-gnu"

    # libc++'s atomic.cpp pulls <linux/futex.h> for the std::atomic wait backend,
    # which needs a few <asm/*> headers. Those are the only arch-specific Linux
    # UAPI headers, shipped per-arch under /usr/include/<triple>/asm - present for
    # the host but not for the cross target (we install no GNU cross sysroot). On
    # aarch64 (a "generic" kernel arch, LP64) they are thin forwarders to the
    # arch-neutral asm-generic/* (which lives in plain /usr/include, on the path
    # for every target). Generate the forwarders so the futex backend compiles;
    # everything else resolves to OSv's include/api (e.g. <sys/syscall.h> ->
    # OSv's bits/syscall.h, so no host asm/unistd is pulled).
    mkdir -p "$GEN_INC/asm"
    echo '#include <asm-generic/types.h>'       > "$GEN_INC/asm/types.h"
    echo '#include <asm-generic/posix_types.h>' > "$GEN_INC/asm/posix_types.h"
    printf '#define __BITS_PER_LONG 64\n#include <asm-generic/bitsperlong.h>\n' \
        > "$GEN_INC/asm/bitsperlong.h"
fi

mkdir -p "$BUILD_DIR"
echo "  LIBCXX configure..."
_progress_run "$BUILD_DIR-configure.log" \
    cmake -S "$LLVM_DIR/runtimes" -B "$BUILD_DIR" \
        $CROSS_CMAKE \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_PROJECT_INCLUDE="$OSV_ROOT/scripts/require-clang.cmake" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
        -DLIBCXX_ENABLE_SHARED=OFF \
        -DLIBCXXABI_ENABLE_SHARED=OFF \
        -DLIBUNWIND_ENABLE_SHARED=OFF \
        -DLIBCXX_ENABLE_STATIC=ON \
        -DLIBCXXABI_ENABLE_STATIC=ON \
        -DLIBUNWIND_ENABLE_STATIC=ON \
        -DLIBCXX_ENABLE_LOCALIZATION=ON \
        -DLIBCXX_ENABLE_UNICODE=OFF \
        -DLIBCXX_ENABLE_FILESYSTEM=OFF \
        -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF \
        -DLIBCXX_ENABLE_WIDE_CHARACTERS=OFF \
        -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
        -DLIBCXX_INCLUDE_TESTS=OFF \
        -DLIBCXX_CXX_ABI=libcxxabi \
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
        -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
        -DLIBCXXABI_INCLUDE_TESTS=OFF \
        -DLIBUNWIND_INCLUDE_TESTS=OFF \
        -DLIBUNWIND_INCLUDE_DOCS=OFF \
        -DCMAKE_C_FLAGS="$KERNEL_FLAGS" \
        -DCMAKE_CXX_FLAGS="$KERNEL_FLAGS" \
    || { echo "configure failed — see $BUILD_DIR-configure.log"; exit 1; }

echo "  LIBCXX build..."
_progress_run "$BUILD_DIR-build.log" \
    make -C "$BUILD_DIR" -j"$(nproc)" cxx cxxabi unwind \
    || { echo "build failed — see $BUILD_DIR-build.log"; exit 1; }

for name in libc++.a libc++abi.a libunwind.a; do
    ARCHIVE=$(find "$BUILD_DIR" -name "$name" | head -1)
    if [ -z "$ARCHIVE" ]; then
        echo "error: $name not produced" >&2; exit 1
    fi
    echo "OK: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
done
