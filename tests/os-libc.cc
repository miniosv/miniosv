/*
 * C libc surface conformance test.
 *
 * os-features.cc walks the OS facilities (threads, mmap, clocks, the C++
 * runtime); this file covers the plain C library surface that sits on top of
 * them: string/memory ops, stdlib conversions, formatted I/O, math, the
 * qsort/bsearch pair, setjmp/longjmp, errno discipline, and calendar-time
 * formatting. These checks were the part of the old in-kernel app (app/app.cc)
 * not already exercised by the other suites.
 *
 * Each CHECK keeps going on failure; os_libc_main() returns non-zero if
 * anything failed.
 */
#include <atomic>

#include <cerrno>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static std::atomic<int> g_checks{0};
static std::atomic<int> g_fails{0};
static const char *g_section = "";

#define CHECK(cond) do { \
        g_checks.fetch_add(1); \
        if (!(cond)) { \
            g_fails.fetch_add(1); \
            printf("    FAIL [%s] %s:%d: %s (errno=%d %s)\n", \
                   g_section, __FILE__, __LINE__, #cond, errno, strerror(errno)); \
        } \
    } while (0)

static void section(const char *s) { g_section = s; printf("  - %s\n", s); }
static void group(const char *s)   { printf("\n== %s ==\n", s); }

/* ============================================== string & memory */

static void test_string()
{
    group("C string & memory");

    section("strcpy / strcat / strlen");
    char s[32];
    strcpy(s, "hello");
    strcat(s, ", world");
    CHECK(strlen(s) == 12);
    CHECK(strcmp(s, "hello, world") == 0);

    section("memset / memcmp");
    memset(s, 'x', 4);
    CHECK(memcmp(s, "xxxx", 4) == 0);
}

/* ============================================== stdlib conversions */

static void test_strtox()
{
    group("stdlib conversions");

    section("strtol");
    CHECK(strtol("  -123", nullptr, 10) == -123);

    section("strtod (hex float)");
    CHECK(strtod("0x1.8p3", nullptr) == 12.0);
}

/* ============================================== formatted I/O */

static void test_format()
{
    group("Formatted I/O");

    section("snprintf golden vector");
    char out[64];
    int n = snprintf(out, sizeof(out), "%d %5.2f %s %#x", 42, 3.14159, "ok", 255);
    CHECK(n == (int)strlen(out));
    CHECK(strcmp(out, "42  3.14 ok 0xff") == 0);

    section("sscanf");
    int iv = 0; double dv = 0;
    int matched = sscanf("7 2.5", "%d %lf", &iv, &dv);
    CHECK(matched == 2);
    CHECK(iv == 7 && dv == 2.5);
}

/* ============================================== math */

static void test_math()
{
    group("Math");

    section("sqrt / pow");
    CHECK(std::fabs(std::sqrt(2.0) * std::sqrt(2.0) - 2.0) < 1e-12);
    CHECK(std::fabs(std::pow(2.0, 10.0) - 1024.0) < 1e-9);

    section("log / exp");
    CHECK(std::fabs(std::log(std::exp(1.0)) - 1.0) < 1e-12);
}

/* ============================================== sort & search */

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

static void test_sort()
{
    group("qsort / bsearch");

    int arr[] = {5, 2, 9, 1, 7, 3};
    const int len = sizeof(arr) / sizeof(arr[0]);

    section("qsort sorts ascending");
    qsort(arr, len, sizeof(int), cmp_int);
    CHECK(arr[0] == 1 && arr[len - 1] == 9);

    section("bsearch finds key");
    int key = 7;
    int *found = (int *)bsearch(&key, arr, len, sizeof(int), cmp_int);
    CHECK(found != nullptr && *found == 7);
}

/* ============================================== control flow */

static jmp_buf g_jb;

static void test_setjmp()
{
    group("setjmp / longjmp");

    section("longjmp returns control to setjmp");
    volatile int hops = 0;
    if (setjmp(g_jb) == 0) {
        hops = 1;
        longjmp(g_jb, 99);
    } else {
        hops = 2;
    }
    CHECK(hops == 2);
}

/* ============================================== errno discipline */

static void test_errno()
{
    group("errno discipline");

    section("failed fopen sets errno and returns NULL");
    errno = 0;
    FILE *f = fopen("/this/does/not/exist", "r");
    CHECK(f == nullptr && errno != 0);
    if (f) {
        fclose(f);
    }
}

/* ============================================== calendar time */

static void test_time_format()
{
    group("Calendar time formatting");

    section("gmtime_r + strftime round-trip on a fixed epoch");
    time_t epoch = 1700000000; // 2023-11-14T22:13:20Z
    struct tm tmv{};
    gmtime_r(&epoch, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    CHECK(strcmp(buf, "2023-11-14T22:13:20") == 0);
}

/* ============================================================== main */

int os_libc_main()
{
    printf("OSv C libc surface conformance test\n");
    printf("===================================\n");

    test_string();
    test_strtox();
    test_format();
    test_math();
    test_sort();
    test_setjmp();
    test_errno();
    test_time_format();

    int checks = g_checks.load(), fails = g_fails.load();
    printf("\n-----------------------------------\n");
    printf("Checks run: %d, failures: %d\n", checks, fails);
    if (fails == 0) {
        printf("RESULT: ALL LIBC TESTS PASSED\n");
        return 0;
    }
    printf("RESULT: %d LIBC TEST(S) FAILED\n", fails);
    return 1;
}
