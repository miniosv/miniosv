#!/usr/bin/env bash
# Build LLVM compiler-rt's builtins (the soft-int / arithmetic helpers like
# __umodti3) as a static archive for the OSv kernel - the compiler-rt
# replacement for GNU libgcc.a (no GCC anywhere in the toolchain).
#
# The host clang package only ships the builtins for the host arch
# (libclang_rt.builtins-x86_64.a), so a cross build (aarch64) has to build them
# from the same pinned llvm-project we use for llvm-libc and libc++. Built
# baremetal/freestanding - builtins pull no libc, only the compiler's own
# headers.
#
# Produces, for the requested arch:
#   build/compiler-rt/<arch>/lib/libclang_rt.builtins.a
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
        echo "build-compiler-rt.sh: unsupported arch '$osv_arch' (only x86/arm)" >&2
        exit 2 ;;
esac

LLVM_TAG=llvmorg-22.1.7
OSV_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_DIR="$OSV_ROOT/external/llvm-project"
BUILD_DIR="$OSV_ROOT/build/compiler-rt/$osv_arch"

# 1. fetch (shares the llvm-libc/libcxx checkout; just widen the sparse set).
# third-party is needed for the aarch64 emupac builtin's siphash/SipHash.h.
if [ ! -d "$LLVM_DIR/compiler-rt" ] || [ ! -d "$LLVM_DIR/third-party" ]; then
    if [ ! -d "$LLVM_DIR/.git" ]; then
        git clone --depth 1 --branch "$LLVM_TAG" --filter=blob:none --sparse \
            https://github.com/llvm/llvm-project.git "$LLVM_DIR"
    fi
    git -C "$LLVM_DIR" sparse-checkout add compiler-rt third-party runtimes cmake \
        llvm/cmake llvm/utils
fi

# 2. configure + build. Codegen must match the kernel (see COMMON in the
# top-level Makefile). builtins are freestanding but int_lib.h still pulls
# <limits.h>/<stdint.h>; resolve those to OSv's own headers (include/api, with
# the generated bits/alltypes.h) instead of the host glibc multiarch headers
# (which only exist for the host arch) - the same header environment as the
# libc++ build.
GEN_INC="$BUILD_DIR/gen/include"
mkdir -p "$GEN_INC/bits"
sh "$OSV_ROOT/include/api/$osv_arch/bits/alltypes.h.sh" > "$GEN_INC/bits/alltypes.h"
OSV_HEADERS="-isystem $OSV_ROOT/include/api -isystem $OSV_ROOT/include/api/$osv_arch -isystem $GEN_INC"
KERNEL_FLAGS="-fno-pie -fno-stack-protector -ftls-model=local-exec $OSV_HEADERS"

# The aarch64 cpu_model builtin (runtime CPU feature detection) includes
# <sys/auxv.h>, which OSv does not provide (a kernel has no ELF auxv). Supply a
# minimal stub so it compiles; it is never called at runtime and --gc-sections
# drops it from the kernel.
mkdir -p "$GEN_INC/sys"
cat > "$GEN_INC/sys/auxv.h" <<'EOF'
#ifndef _OSV_STUB_SYS_AUXV_H
#define _OSV_STUB_SYS_AUXV_H
/* Minimal stub for compiler-rt's cpu_model builtin - see build-compiler-rt.sh. */
#define AT_HWCAP  16
#define AT_HWCAP2 26
#ifdef __cplusplus
extern "C"
#endif
unsigned long getauxval(unsigned long);
#endif
EOF

# Pure-LLVM cross-compile when targeting a non-host arch: one clang driven at the
# target triple (no GNU cross toolchain). TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
# makes cmake's compiler check compile-only (we have no GNU sysroot to link an
# executable, and don't want one).
CROSS_CMAKE=
if [ "$llvm_arch" != "$(uname -m)" ]; then
    CROSS_CMAKE="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$llvm_arch \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_C_COMPILER_TARGET=$llvm_arch-linux-gnu \
        -DCMAKE_CXX_COMPILER_TARGET=$llvm_arch-linux-gnu \
        -DCMAKE_ASM_COMPILER_TARGET=$llvm_arch-linux-gnu"
fi

mkdir -p "$BUILD_DIR"
echo "  COMPILER-RT configure..."
_progress_run "$BUILD_DIR-configure.log" \
    cmake -S "$LLVM_DIR/runtimes" -B "$BUILD_DIR" \
        $CROSS_CMAKE \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DLLVM_ENABLE_RUNTIMES="compiler-rt" \
        -DCOMPILER_RT_BUILD_BUILTINS=ON \
        -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
        -DCOMPILER_RT_BUILD_XRAY=OFF \
        -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
        -DCOMPILER_RT_BUILD_PROFILE=OFF \
        -DCOMPILER_RT_BUILD_MEMPROF=OFF \
        -DCOMPILER_RT_BUILD_ORC=OFF \
        -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
        -DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
        -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
        -DCOMPILER_RT_BAREMETAL_BUILD=ON \
        -DCMAKE_C_FLAGS="$KERNEL_FLAGS" \
        -DCMAKE_CXX_FLAGS="$KERNEL_FLAGS" \
    || { echo "configure failed — see $BUILD_DIR-configure.log"; exit 1; }

echo "  COMPILER-RT build..."
_progress_run "$BUILD_DIR-build.log" \
    make -C "$BUILD_DIR" -j"$(nproc)" builtins \
    || { echo "build failed — see $BUILD_DIR-build.log"; exit 1; }

# cmake names the archive per-arch/triple (libclang_rt.builtins-aarch64.a or
# .../<triple>/libclang_rt.builtins.a). Settle on one stable path the Makefile
# can reference. Exclude that stable path from the search: when build/compiler-rt
# is restored from CI cache the script re-runs (its .compiler-rt-built stamp is
# not cached), and otherwise find would match the copy from the previous run and
# cp it onto itself ("are the same file").
DEST="$BUILD_DIR/lib/libclang_rt.builtins.a"
SRC=$(find "$BUILD_DIR" -name 'libclang_rt.builtins*.a' ! -path "$DEST" | head -1)
if [ -z "$SRC" ]; then
    echo "error: libclang_rt.builtins not produced" >&2; exit 1
fi
mkdir -p "$BUILD_DIR/lib"
if [ "$SRC" -ef "$DEST" ]; then
    echo "OK: $DEST (already in place)"
else
    cp -f "$SRC" "$DEST"
    echo "OK: $DEST (from $SRC, $(du -h "$SRC" | cut -f1))"
fi
