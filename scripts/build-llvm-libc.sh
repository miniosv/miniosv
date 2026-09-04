#!/usr/bin/env bash
# Build LLVM libc as a no-syscall static archive for the OSv kernel.
#
# Phase 1 of LLVM_LIBC_PLAN.md. Produces, for the requested arch,
#   build/llvm-libc/<arch>/libc/lib/{libc.a,libm.a}
# built in fullbuild mode for the "baremetal" target OS (a libc that performs
# no syscalls at all), with the entrypoint list, config overrides and
# kernel-matching codegen flags from external/llvm-libc-config/. Verifies the no-syscall
# gate with objdump at the end.
#
# This script is invoked automatically by the Makefile (it is a prerequisite of
# the kernel link when conf_llvm_libc=1); you do not need to run it by hand.
#
# llvm-project is pinned (see LLVM_TAG) and fetched as an untracked sparse
# checkout under external/llvm-project; external/llvm-libc-config/* are the files we
# control (a carried "patch": llvm-libc has no upstream x86_64 baremetal config
# yet - this script installs ours into the checkout; aarch64 baremetal is
# upstream-supported and we overlay the same curated surface on it).
#
# Only x86_64 (OSv arch "x64") and aarch64 ("arm"/"aarch64") are supported.

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

# --- arch handling: only x86 and arm ----------------------------------------
osv_arch="${1:-x64}"
case "$osv_arch" in
    x64|x86_64|x86)   osv_arch=x64;      llvm_arch=x86_64 ;;
    aarch64|arm|arm64) osv_arch=aarch64; llvm_arch=aarch64 ;;
    *)
        echo "build-llvm-libc.sh: unsupported arch '$osv_arch' (only x86/arm)" >&2
        exit 2 ;;
esac

LLVM_TAG=llvmorg-22.1.7
OSV_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_DIR="$OSV_ROOT/external/llvm-project"
BUILD_DIR="$OSV_ROOT/build/llvm-libc/$osv_arch"
CONFIG_SRC="$OSV_ROOT/external/llvm-libc-config"

# 1. fetch (pinned, sparse: libc + runtimes + cmake support)
# Guard the clone on .git (the checkout is shared with build-libcxx.sh /
# build-compiler-rt.sh) and widen with `add`, not `set`, so we don't drop the
# paths a sibling toolchain build already checked out.
if [ ! -d "$LLVM_DIR/libc" ]; then
    if [ ! -d "$LLVM_DIR/.git" ]; then
        git clone --depth 1 --branch "$LLVM_TAG" --filter=blob:none --sparse \
            https://github.com/llvm/llvm-project.git "$LLVM_DIR"
    fi
    git -C "$LLVM_DIR" sparse-checkout add libc runtimes cmake llvm/cmake llvm/utils
fi
actual_tag=$(git -C "$LLVM_DIR" describe --tags 2>/dev/null || true)
if [ "$actual_tag" != "$LLVM_TAG" ]; then
    echo "warning: external/llvm-project is at '$actual_tag', expected $LLVM_TAG" >&2
fi

# 2. install our baremetal config (entrypoints/headers/config) into the checkout
ARCH_CONF="$LLVM_DIR/libc/config/baremetal/$llvm_arch"
mkdir -p "$ARCH_CONF"
cp "$CONFIG_SRC/entrypoints.txt" "$ARCH_CONF/entrypoints.txt"
cp "$CONFIG_SRC/headers.txt"     "$ARCH_CONF/headers.txt"
cp "$CONFIG_SRC/config.json"     "$ARCH_CONF/config.json"

# 3. configure + build
# Codegen must match the kernel (see COMMON in the top-level Makefile):
# non-PIE, no stack protector, local-exec TLS.
KERNEL_FLAGS="-fno-pie -fno-stack-protector -ftls-model=local-exec"

mkdir -p "$BUILD_DIR"
echo "  LLVM-LIBC configure..."
_progress_run "$BUILD_DIR-configure.log" \
    cmake -S "$LLVM_DIR/runtimes" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DLLVM_ENABLE_RUNTIMES=libc \
        -DLLVM_LIBC_FULL_BUILD=ON \
        -DLIBC_TARGET_TRIPLE="$llvm_arch-none-elf" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_C_FLAGS="$KERNEL_FLAGS" \
        -DCMAKE_CXX_FLAGS="$KERNEL_FLAGS" \
    || { echo "configure failed — see $BUILD_DIR-configure.log"; exit 1; }

# fullbuild packages the math entrypoints into a separate libm.a
echo "  LLVM-LIBC build..."
_progress_run "$BUILD_DIR-build.log" \
    make -C "$BUILD_DIR" -j"$(nproc)" libc libm \
    || { echo "build failed — see $BUILD_DIR-build.log"; exit 1; }

# x86 uses the `syscall` instruction; arm uses `svc`. Gate against both so the
# archive is guaranteed free of any direct kernel-entry instruction. Use
# llvm-objdump - it disassembles every target, unlike the host GNU objdump
# which only knows the host arch. x86 has variable-length encodings (mnemonic
# is the last field of a no-operand insn); aarch64 is fixed 32-bit (one
# encoding field, so the mnemonic is always field 3).
OBJDUMP=$(command -v llvm-objdump || command -v llvm-objdump-20 || echo objdump)
case "$llvm_arch" in
    x86_64)  entry_insns='$NF=="syscall" || $NF=="sysenter"' ;;
    aarch64) entry_insns='$3=="svc"' ;;
esac

for name in libc.a libm.a; do
    ARCHIVE=$(find "$BUILD_DIR" -name "$name" | head -1)
    if [ -z "$ARCHIVE" ]; then
        echo "error: $name not produced" >&2; exit 1
    fi
    nsyscall=$($OBJDUMP -d "$ARCHIVE" | awk "$entry_insns" | wc -l)
    if [ "$nsyscall" -ne 0 ]; then
        echo "GATE FAILED: $nsyscall kernel-entry instructions in $ARCHIVE" >&2
        exit 1
    fi
    echo "OK: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1)), zero syscall instructions"
done
