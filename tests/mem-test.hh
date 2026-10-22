/*
 * Shared scaffolding for the two memory suites: counters, section headings,
 * timing, and a thread runner that pins.
 *
 * The counters are shared between the suites, so each one resets them when it
 * starts and prints its own total when it ends.
 */

#ifndef TEST_MEM_TEST_HH
#define TEST_MEM_TEST_HH

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <osv/sched.hh>

namespace memtest {

inline std::atomic<int> checks{0};
inline std::atomic<int> fails{0};
inline const char *current = "";

inline void reset()
{
    checks.store(0);
    fails.store(0);
    current = "";
}

inline void group(const char *s)
{
    printf("\n== %s ==\n", s);
}

// Flushed, so that a crash leaves the last section it reached on the console
// rather than in a buffer that is never written.
inline void section(const char *s)
{
    current = s;
    printf("  - %s\n", s);
    fflush(stdout);
}

// A function under test: the header its tests are listed under.
inline void function(const char *s)
{
    printf("\n%s\n", s);
    fflush(stdout);
}

// One test of that function.
inline void test(const char *s)
{
    current = s;
    printf("\t%s\n", s);
    fflush(stdout);
}

inline int summary(const char *what)
{
    int bad = fails.load();
    printf("\n%d checks, %d failures\n", checks.load(), bad);
    printf("RESULT: %s %s\n", what, bad ? "TESTS FAILED" : "TESTS PASSED");
    return bad ? 1 : 0;
}

using clk = std::chrono::steady_clock;

inline double since(clk::time_point t0)
{
    return std::chrono::duration<double>(clk::now() - t0).count();
}

// clang knows malloc/free and will delete a pair whose result is unused.
inline void escape(void *p)
{
    asm volatile("" : : "r,m"(p) : "memory");
}

inline void report_ns(const char *what, double s, double n)
{
    printf("    %-46s %9.1f ns/op\n", what, s * 1e9 / n);
}

// ops is the total across all threads; the per-thread cost is what shows
// whether the work actually got faster or just got shared out.
inline void report_scale(const char *what, unsigned threads, double ops, double s)
{
    printf("    %-32s %3u thr %8.2f Mops/s %9.1f ns/op\n",
           what, threads, ops / s / 1e6, s * 1e9 * threads / ops);
}

// For measurements that are serialised, keep the total work constant so the
// run does not take longer and longer as threads are added.
inline int share(int total, unsigned threads, int least = 4)
{
    return std::max(least, total / static_cast<int>(threads));
}

inline unsigned n_cpus()
{
    unsigned n = std::thread::hardware_concurrency();
    return n ? n : 1;
}

// Runs fn(i) on `threads` threads started together; returns the wall time.
// Each thread is pinned to its own cpu: without that they all stay on the cpu
// that created them, since the load balancer runs every 100 ms, far longer
// than a measurement, and a scaling run would measure one core sharing its
// time N ways -- which looks exactly like a global lock.
template <typename F>
double parallel(unsigned threads, F fn)
{
    std::vector<std::thread> ts;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    for (unsigned i = 0; i < threads; i++) {
        ts.emplace_back([&, i] {
            sched::thread::pin(sched::cpus[i % sched::cpus.size()]);
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            fn(i);
        });
    }
    while (ready.load() != threads) {
        std::this_thread::yield();
    }
    auto t0 = clk::now();
    go.store(true, std::memory_order_release);
    for (auto &t : ts) {
        t.join();
    }
    return since(t0);
}

}

#define CHECK(cond) do { \
        memtest::checks.fetch_add(1); \
        if (!(cond)) { \
            memtest::fails.fetch_add(1); \
            printf("    FAIL [%s] %s:%d: %s\n", memtest::current, __FILE__, __LINE__, #cond); \
            fflush(stdout); \
        } \
    } while (0)

#endif
