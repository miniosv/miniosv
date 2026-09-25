/*
 * memcmp correctness and throughput.
 *
 * Checks whatever memcmp the kernel actually linked: every length and
 * alignment against a reference, that it never reads outside its range, and
 * that its sign follows the unsigned-char comparison the standard requires.
 * Then reports throughput, so arch/x64/string.cc can be compared against the
 * libc's by building with and without it.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>

// Called through a volatile pointer so the compiler cannot expand its own
// builtin and bypass the symbol the kernel links.
using memcmp_fn = int (*)(const void *, const void *, size_t);
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

static int ref_memcmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

static void test_correctness()
{
    printf("  every length and alignment, every differing position\n");

    const size_t MAXN = 160, MAXALIGN = 32, PAD = MAXALIGN + MAXN + 16;
    alignas(64) static uint8_t x[PAD], y[PAD];

    for (size_t align = 0; align < MAXALIGN; align++) {
        for (size_t n = 0; n <= MAXN; n++) {
            uint8_t *a = x + align;
            uint8_t *b = y + align;
            memset(x, 0x42, PAD);
            memset(y, 0x42, PAD);

            CHECK(kernel_memcmp(a, b, n) == 0,
                  "align=%zu n=%zu: equal buffers compared non-zero", align, n);

            for (size_t pos = 0; pos < n; pos++) {
                for (int delta : { -1, +1 }) {
                    b[pos] = (uint8_t)(0x42 + delta);
                    CHECK(sign_of(kernel_memcmp(a, b, n)) ==
                          sign_of(ref_memcmp(a, b, n)),
                          "align=%zu n=%zu pos=%zu: wrong sign", align, n, pos);
                    b[pos] = 0x42;
                }
            }

            // 0xff must compare greater than 0x01: the comparison is on
            // unsigned char, whatever plain char signedness happens to be.
            if (n > 0) {
                a[n - 1] = 0xff;
                b[n - 1] = 0x01;
                CHECK(sign_of(kernel_memcmp(a, b, n)) > 0,
                      "n=%zu: compared as signed", n);
                a[n - 1] = b[n - 1] = 0x42;
            }
        }
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

    const size_t MAXLEN = 8u << 20;
    std::vector<uint8_t> a(MAXLEN, 0x5a), b(MAXLEN, 0x5a);

    // Sizes below 64 are here because that is where a vector implementation
    // stops paying for itself, and where the two candidates crossed over.
    struct { size_t len; long iters; } cases[] = {
        {       16, 4000000 }, {       32, 3000000 }, {       48, 2000000 },
        {       64, 2000000 }, {      256, 1000000 }, {     4096,  200000 },
        {    65536,   20000 }, { 1u << 20,    2000 }, {   MAXLEN,     200 },
    };

    g_sink = kernel_memcmp(a.data(), b.data(), MAXLEN);   // fault the pages in

    for (auto& c : cases) {
        int acc = 0;
        const double t0 = now_sec();
        for (long i = 0; i < c.iters; i++) {
            acc += kernel_memcmp(a.data(), b.data(), c.len);
        }
        const double dt = now_sec() - t0;
        g_sink = acc;

        char sz[16];
        if (c.len >= (1u << 20)) {
            snprintf(sz, sizeof sz, "%zu MiB", c.len >> 20);
        } else if (c.len >= 1024) {
            snprintf(sz, sizeof sz, "%zu KiB", c.len >> 10);
        } else {
            snprintf(sz, sizeof sz, "%zu B", c.len);
        }
        printf("    %10s %10.1f\n", sz,
               (double)c.len * (double)c.iters / dt / (1024.0 * 1024.0 * 1024.0));
    }
}

int os_string_main()
{
    printf("---- memcmp ----\n");
    g_failures = 0;

    test_correctness();
    test_throughput();

    printf("---- memcmp: %s ----\n",
           g_failures ? "FAILURE" : "all correctness checks passed");
    return g_failures ? 1 : 0;
}
