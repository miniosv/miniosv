/*
 * memmove() overlap-correctness test.
 *
 * Guards the kernel memmove() contract: copy every length / overlap offset /
 * destination alignment, in both directions, and compare against a known-good
 * reference. memmove/memcpy/memset are now provided by llvm-libc (the
 * hand-written OSv/arch implementations were removed once llvm-libc's were no
 * longer slower); this stays as a broad correctness regression test for
 * whatever memmove the kernel links.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// Reference memmove: copy through a temporary, which is always correct
// regardless of overlap direction.
static void ref_memmove(uint8_t* d, const uint8_t* s, size_t n)
{
    std::vector<uint8_t> tmp(s, s + n);
    for (size_t i = 0; i < n; i++) {
        d[i] = tmp[i];
    }
}

static inline uint8_t pattern_byte(size_t i)
{
    return static_cast<uint8_t>(i * 7u + 1u);
}

// Call through a volatile function pointer so the compiler cannot inline its
// own (correct) memmove expansion and bypass the kernel's memmove symbol -- we
// specifically want to exercise the actual linked memmove implementation.
using memmove_fn = void* (*)(void*, const void*, size_t);
static volatile memmove_fn kernel_memmove = memmove;

int os_memmove_main()
{
    printf("---- memmove overlap test ----\n");

    int failures = 0;
    // n < 64 exercises the small_memcpy / do_small_memcpy path that was
    // miscompiled; a little beyond covers the SSE / rep-movs paths too.
    const size_t MAXN = 96;
    const size_t MAXALIGN = 64;            // sweep destination alignment
    const size_t PAD = MAXALIGN + MAXN + MAXN + 32;

    // 64-byte aligned working buffers so "base + align" gives a controlled
    // offset relative to a cache line / SSE boundary.
    alignas(64) static uint8_t buf[PAD];
    alignas(64) static uint8_t exp[PAD];

    for (size_t align = 0; align < MAXALIGN; align++) {
        for (size_t n = 1; n <= MAXN; n++) {
            for (size_t shift = 1; shift <= n; shift++) {
                // Forward overlap: dest < src (the direction clang miscompiled).
                for (size_t i = 0; i < PAD; i++) buf[i] = exp[i] = pattern_byte(i);
                kernel_memmove(buf + align, buf + align + shift, n); // kernel
                ref_memmove(exp + align, exp + align + shift, n);   // reference
                if (memcmp(buf, exp, PAD) != 0) {
                    if (++failures <= 8) {
                        printf("  FAIL forward  align=%zu n=%zu shift=%zu (dest<src)\n",
                               align, n, shift);
                    }
                }

                // Backward overlap: dest > src.
                for (size_t i = 0; i < PAD; i++) buf[i] = exp[i] = pattern_byte(i);
                kernel_memmove(buf + align + shift, buf + align, n); // kernel
                ref_memmove(exp + align + shift, exp + align, n);   // reference
                if (memcmp(buf, exp, PAD) != 0) {
                    if (++failures <= 8) {
                        printf("  FAIL backward align=%zu n=%zu shift=%zu (dest>src)\n",
                               align, n, shift);
                    }
                }
            }
        }
    }

    // Non-overlapping sanity check across the same length range.
    for (size_t n = 1; n <= MAXN; n++) {
        std::vector<uint8_t> src(n), dst(n, 0), ref(n);
        for (size_t i = 0; i < n; i++) {
            src[i] = static_cast<uint8_t>(i * 13u + 5u);
            ref[i] = src[i];
        }
        kernel_memmove(dst.data(), src.data(), n);
        if (memcmp(dst.data(), ref.data(), n) != 0) {
            if (++failures <= 8) {
                printf("  FAIL disjoint n=%zu\n", n);
            }
        }
    }

    printf("memmove overlap test: %s (%d failure%s)\n",
           failures ? "FAILURE" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
