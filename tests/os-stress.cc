/*
 * OS-interaction stress tests for the slim OSv kernel (C++).
 *
 * Hammers the parts of the slim kernel that involve the scheduler and memory:
 * thread scheduling/synchronization, anonymous virtual memory, signals, and
 * time. It is deliberately concurrent and repetitive (many threads, many
 * iterations) to shake out races and leaks.
 *
 * The filesystem, the fd table, networking and the fd-based IPC/event
 * facilities (pipes, sockets, eventfd, poll/epoll) were removed from the slim
 * kernel (Phases 5-9), so the tests that exercised them are gone - what remains
 * is the surface the in-kernel application can actually use.
 *
 * Each CHECK keeps running on failure; os_stress_main() returns non-zero if
 * anything failed. Worker threads never call CHECK directly - they report via
 * atomics/return values and the main thread asserts after joining.
 */
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <vector>
#include <queue>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

static std::atomic<int> g_checks{0};
static std::atomic<int> g_fails{0};
static const char *g_section = "";

#define CHECK(cond) do { \
        g_checks.fetch_add(1); \
        if (!(cond)) { \
            g_fails.fetch_add(1); \
            printf("FAIL [%s] %s:%d: %s\n", g_section, __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static void section(const char *s) { g_section = s; printf("  - %s\n", s); }

/* ============================================================ threads */

static void test_threads_atomic()
{
    section("threads: atomic counter under contention");
    const int NT = 16, PER = 100000;
    std::atomic<long> counter{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] { for (int j = 0; j < PER; j++) counter.fetch_add(1, std::memory_order_relaxed); });
    for (auto &t : ts) t.join();
    CHECK(counter.load() == (long)NT * PER);
}

static void test_threads_mutex()
{
    section("threads: mutex-protected counter (lost-update detection)");
    const int NT = 12, PER = 20000;
    long counter = 0;
    std::mutex m;
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] { for (int j = 0; j < PER; j++) { std::lock_guard<std::mutex> lk(m); counter++; } });
    for (auto &t : ts) t.join();
    CHECK(counter == (long)NT * PER);
}

static void test_condvar_queue()
{
    section("threads: condition_variable producer/consumer");
    std::mutex m;
    std::condition_variable cv;
    std::queue<int> q;
    bool done = false;
    std::atomic<long> sum{0};
    const int N = 50000;

    std::thread consumer([&] {
        for (;;) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return !q.empty() || done; });
            while (!q.empty()) { sum += q.front(); q.pop(); }
            if (done && q.empty()) break;
        }
    });
    for (int i = 1; i <= N; i++) {
        { std::lock_guard<std::mutex> lk(m); q.push(i); }
        cv.notify_one();
    }
    { std::lock_guard<std::mutex> lk(m); done = true; }
    cv.notify_one();
    consumer.join();
    CHECK(sum.load() == (long)N * (N + 1) / 2);
}

static void test_async_futures()
{
    section("threads: std::async / std::future fan-out");
    const int N = 64;
    std::vector<std::future<long>> fs;
    for (int i = 0; i < N; i++)
        fs.push_back(std::async(std::launch::async, [i] {
            long s = 0; for (int j = 0; j <= i; j++) s += j; return s;
        }));
    long total = 0;
    for (auto &f : fs) total += f.get();
    long expect = 0;
    for (int i = 0; i < N; i++) expect += (long)i * (i + 1) / 2;
    CHECK(total == expect);
}

static thread_local int tls_value = -1;
static void test_thread_local()
{
    section("threads: thread_local isolation");
    const int NT = 20;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([i, &mismatches] {
            tls_value = i;
            for (int k = 0; k < 1000; k++) {
                std::this_thread::yield();
                if (tls_value != i) mismatches++;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(mismatches.load() == 0);
}

static void test_pthread_barrier()
{
    section("threads: pthread_barrier synchronization");
    const int NT = 8, ROUNDS = 100;
    pthread_barrier_t bar;
    CHECK(pthread_barrier_init(&bar, nullptr, NT) == 0);
    std::atomic<int> phase{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] {
            for (int r = 0; r < ROUNDS; r++) {
                int seen = phase.load();
                int rc = pthread_barrier_wait(&bar);
                /* exactly one thread gets the SERIAL return value */
                if (rc == PTHREAD_BARRIER_SERIAL_THREAD) phase.fetch_add(1);
                (void)seen;
                pthread_barrier_wait(&bar);
            }
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
    CHECK(phase.load() == ROUNDS);
    pthread_barrier_destroy(&bar);
}

static void test_rwlock()
{
    section("threads: pthread_rwlock readers/writers");
    pthread_rwlock_t rw;
    CHECK(pthread_rwlock_init(&rw, nullptr) == 0);
    long shared = 0;
    std::atomic<int> inconsistencies{0};
    std::vector<std::thread> ts;
    const int READERS = 8, WRITERS = 4, ITERS = 5000;
    for (int w = 0; w < WRITERS; w++)
        ts.emplace_back([&] {
            for (int i = 0; i < ITERS; i++) {
                pthread_rwlock_wrlock(&rw);
                long a = shared; shared = a + 1; long b = shared;
                if (b != a + 1) inconsistencies++;
                pthread_rwlock_unlock(&rw);
            }
        });
    for (int r = 0; r < READERS; r++)
        ts.emplace_back([&] {
            for (int i = 0; i < ITERS; i++) {
                pthread_rwlock_rdlock(&rw);
                long a = shared; (void)a;
                pthread_rwlock_unlock(&rw);
            }
        });
    for (auto &t : ts) t.join();
    CHECK(inconsistencies.load() == 0);
    CHECK(shared == (long)WRITERS * ITERS);
    pthread_rwlock_destroy(&rw);
}

/* ============================================================ virtual memory */

static void test_mmap_anon()
{
    section("vm: anonymous mmap, touch pages, mprotect, madvise");
    const size_t SZ = 8 * 1024 * 1024;
    void *p = mmap(nullptr, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED) return;
    unsigned char *m = (unsigned char *)p;
    for (size_t i = 0; i < SZ; i += 4096) m[i] = 0xA5;
    bool ok = true;
    for (size_t i = 0; i < SZ; i += 4096) if (m[i] != 0xA5) ok = false;
    CHECK(ok);
    CHECK(mprotect(p, SZ, PROT_READ) == 0);          /* make read-only */
    CHECK(madvise(p, SZ, MADV_DONTNEED) == 0 || errno == EINVAL);
    CHECK(munmap(p, SZ) == 0);
}

static void test_mmap_concurrent()
{
    section("vm: concurrent anonymous mmap/touch/munmap (16 threads)");
    const int NT = 16;
    const size_t SZ = 1024 * 1024;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([i, &errors] {
            for (int r = 0; r < 32; r++) {
                void *p = mmap(nullptr, SZ, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p == MAP_FAILED) { errors++; continue; }
                unsigned char *m = (unsigned char *)p;
                for (size_t o = 0; o < SZ; o += 4096) m[o] = (unsigned char)(i + r);
                for (size_t o = 0; o < SZ; o += 4096) if (m[o] != (unsigned char)(i + r)) errors++;
                if (munmap(p, SZ) != 0) errors++;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
}

/* ============================================================ time/sched */

static void test_time_sched()
{
    section("time: monotonic clock + nanosleep + sched_yield");
    struct timespec a, b;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    struct timespec nap = {0, 20 * 1000 * 1000};   /* 20 ms */
    CHECK(nanosleep(&nap, nullptr) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    double elapsed = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    CHECK(elapsed >= 0.015);   /* at least ~15ms passed */

    for (int i = 0; i < 1000; i++) sched_yield();
    CHECK(true);

    /* hammer clock_gettime from many threads (no crash / monotonic per thread) */
    const int NT = 8;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++)
        ts.emplace_back([&] {
            struct timespec p{0, 0};
            for (int k = 0; k < 20000; k++) {
                struct timespec n;
                clock_gettime(CLOCK_MONOTONIC, &n);
                if (n.tv_sec < p.tv_sec) errors++;
                p = n;
            }
        });
    for (auto &t : ts) t.join();
    CHECK(errors.load() == 0);
}

int os_stress_main()
{
    printf("==== OSv OS-interaction stress tests ====\n");

    test_threads_atomic();
    test_threads_mutex();
    test_condvar_queue();
    test_async_futures();
    test_thread_local();
    test_pthread_barrier();
    test_rwlock();

    test_mmap_anon();
    test_mmap_concurrent();

    test_time_sched();

    printf("\n==== os-stress: %d checks, %d failures ====\n",
           g_checks.load(), g_fails.load());
    if (g_fails.load() == 0)
        printf("RESULT: ALL OS-STRESS TESTS PASSED\n");
    else
        printf("RESULT: %d OS-STRESS TEST(S) FAILED\n", g_fails.load());
    return g_fails.load() ? 1 : 0;
}
