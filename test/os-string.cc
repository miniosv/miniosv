/*
 * memset() / memcmp() correctness and throughput test.
 *
 * The kernel may override the generic libc's memset/memcmp with an
 * arch-specific implementation (arch/<arch>/string.cc). This checks whatever
 * is actually linked: every length and destination alignment against a
 * reference, that neither routine touches a byte outside its range, and that
 * memcmp's sign follows the unsigned-char comparison the standard requires.
 *
 * It then reports throughput, so the arch implementation can be compared
 * against the libc one by building with and without it.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <vector>

// Call through volatile function pointers so the compiler cannot expand its
// own builtin and bypass the symbols the kernel actually links.
using memset_fn = void* (*)(void*, int, size_t);
using memcmp_fn = int (*)(const void*, const void*, size_t);
static volatile memset_fn kernel_memset = memset;
static volatile memcmp_fn kernel_memcmp = memcmp;

static int g_failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            g_failures++; \
            printf("    FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static int sign_of(int v) { return (v > 0) - (v < 0); }

// The comparison memcmp is defined to make: first differing byte, as unsigned
// char. Only the sign of the result is specified.
static int ref_memcmp(const uint8_t* a, const uint8_t* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

static void test_memset_correctness()
{
    printf("  memset: every length and alignment, with guard bytes\n");

    const size_t MAXN = 160;
    const size_t MAXALIGN = 64;
    const size_t GUARD = 8;
    const size_t PAD = MAXALIGN + MAXN + 2 * GUARD + 16;

    alignas(64) static uint8_t buf[PAD];

    for (size_t align = 0; align < MAXALIGN; align++) {
        for (size_t n = 0; n <= MAXN; n++) {
            for (int val : {0, 0x5a, 0xff}) {
                memset(buf, 0xa5, PAD);          // builtin: sets up the guards
                uint8_t* dst = buf + GUARD + align;

                void* ret = kernel_memset(dst, val, n);
                CHECK(ret == dst, "memset returned %p, want %p", ret, dst);

                for (size_t i = 0; i < n; i++) {
                    CHECK(dst[i] == (uint8_t)val,
                          "align=%zu n=%zu val=%d: byte %zu is 0x%02x",
                          align, n, val, i, dst[i]);
                }
                for (size_t i = 0; i < GUARD; i++) {
                    CHECK(dst[-1 - (ptrdiff_t)i] == 0xa5,
                          "align=%zu n=%zu: wrote %zu bytes before start",
                          align, n, i + 1);
                    CHECK(dst[n + i] == 0xa5,
                          "align=%zu n=%zu: wrote %zu bytes past end",
                          align, n, i + 1);
                }
            }
        }
    }

    // Lengths past any small-case cutoff, where a wide path takes over.
    for (size_t n : {size_t(4096), size_t(65536), size_t(1u << 20)}) {
        std::vector<uint8_t> v(n + 2 * GUARD, 0xa5);
        kernel_memset(v.data() + GUARD, 0x3c, n);
        for (size_t i = 0; i < n; i++) {
            CHECK(v[GUARD + i] == 0x3c, "n=%zu: byte %zu is 0x%02x",
                  n, i, v[GUARD + i]);
        }
        for (size_t i = 0; i < GUARD; i++) {
            CHECK(v[i] == 0xa5, "n=%zu: wrote before start", n);
            CHECK(v[GUARD + n + i] == 0xa5, "n=%zu: wrote past end", n);
        }
    }
}

static void test_memcmp_correctness()
{
    printf("  memcmp: equal, every differing position, and sign\n");

    const size_t MAXN = 160;
    const size_t MAXALIGN = 16;
    alignas(64) static uint8_t a[MAXALIGN + MAXN + 16];
    alignas(64) static uint8_t b[MAXALIGN + MAXN + 16];

    for (size_t align = 0; align < MAXALIGN; align++) {
        for (size_t n = 0; n <= MAXN; n++) {
            uint8_t* pa = a + align;
            uint8_t* pb = b + align;

            for (size_t i = 0; i < n; i++) {
                pa[i] = pb[i] = (uint8_t)(i * 7u + 1u);
            }
            CHECK(kernel_memcmp(pa, pb, n) == 0,
                  "align=%zu n=%zu: equal buffers compared unequal", align, n);

            // Differ at each position in turn, in both directions. 0x01 vs
            // 0xff catches an implementation comparing as signed char.
            for (size_t pos = 0; pos < n; pos++) {
                uint8_t save = pb[pos];

                pb[pos] = 0xff;
                pa[pos] = 0x01;
                CHECK(sign_of(kernel_memcmp(pa, pb, n))
                          == ref_memcmp(pa, pb, n),
                      "align=%zu n=%zu pos=%zu: wrong sign (0x01 vs 0xff)",
                      align, n, pos);

                pb[pos] = 0x01;
                pa[pos] = 0xff;
                CHECK(sign_of(kernel_memcmp(pa, pb, n))
                          == ref_memcmp(pa, pb, n),
                      "align=%zu n=%zu pos=%zu: wrong sign (0xff vs 0x01)",
                      align, n, pos);

                pa[pos] = pb[pos] = save;
            }
        }
    }

    // A difference far into a large buffer, past any small-case cutoff.
    for (size_t n : {size_t(4096), size_t(65536)}) {
        std::vector<uint8_t> x(n, 0x42), y(n, 0x42);
        CHECK(kernel_memcmp(x.data(), y.data(), n) == 0, "n=%zu: equal", n);
        y[n - 1] = 0x43;
        CHECK(kernel_memcmp(x.data(), y.data(), n) < 0,
              "n=%zu: difference in the last byte missed", n);
        y[n - 1] = 0x42;
        y[n / 2] = 0x41;
        CHECK(kernel_memcmp(x.data(), y.data(), n) > 0,
              "n=%zu: difference at the midpoint missed", n);
    }
}

static double now_sec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static volatile int g_sink;

static void test_throughput()
{
    printf("  throughput (GiB/s, higher is better)\n");
    printf("    %10s %12s %12s\n", "size", "memset", "memcmp");

    const size_t MAXLEN = 8u << 20;
    std::vector<uint8_t> a(MAXLEN, 0x5a), b(MAXLEN, 0x5a);

    struct { size_t len; long iters; } cases[] = {
        {       64, 2000000 },
        {      256, 1000000 },
        {     4096,  200000 },
        {    65536,   20000 },
        { 1u << 20,    2000 },
        { MAXLEN,       200 },
    };

    // Fault the pages in and let the clock settle.
    kernel_memset(a.data(), 0, MAXLEN);
    g_sink = kernel_memcmp(a.data(), b.data(), MAXLEN);
    kernel_memset(a.data(), 0x5a, MAXLEN);

    for (auto& c : cases) {
        double t0 = now_sec();
        for (long i = 0; i < c.iters; i++) {
            kernel_memset(a.data(), (int)(i & 0xff), c.len);
        }
        double set_s = now_sec() - t0;
        g_sink = a[c.len - 1];

        kernel_memset(a.data(), 0x5a, c.len);
        int acc = 0;
        t0 = now_sec();
        for (long i = 0; i < c.iters; i++) {
            acc += kernel_memcmp(a.data(), b.data(), c.len);
        }
        double cmp_s = now_sec() - t0;
        g_sink = acc;

        const double gib = 1024.0 * 1024.0 * 1024.0;
        double bytes = (double)c.len * (double)c.iters;
        char sz[16];
        if (c.len >= (1u << 20)) {
            snprintf(sz, sizeof sz, "%zu MiB", c.len >> 20);
        } else if (c.len >= 1024) {
            snprintf(sz, sizeof sz, "%zu KiB", c.len >> 10);
        } else {
            snprintf(sz, sizeof sz, "%zu B", c.len);
        }
        printf("    %10s %12.1f %12.1f\n", sz,
               bytes / set_s / gib, bytes / cmp_s / gib);
    }
}

int os_string_main()
{
    printf("---- memset / memcmp test ----\n");
    g_failures = 0;

    test_memset_correctness();
    test_memcmp_correctness();
    test_throughput();

    printf("---- memset / memcmp: %s ----\n",
           g_failures ? "FAILURE" : "all correctness checks passed");
    return g_failures ? 1 : 0;
}
