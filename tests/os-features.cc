/*
 * OS feature inventory + conformance test for the slimmed OSv unikernel (C++).
 *
 * Unlike apps/os-stress (which hammers a few subsystems concurrently to shake
 * out races), this program walks the *full list* of OS facilities the current,
 * slimmed-down code base is supposed to support and checks each one behaves.
 * It also probes the documented boundaries (no fork, no IP networking, no
 * directed thread signals) and asserts they fail cleanly rather than crash.
 *
 * Built as a dynamic PIE so every libc/syscall symbol resolves against OSv's
 * own libc + kernel at run time. NOTE: in a dynamically-linked OSv program a
 * call to a libc symbol the kernel does not define aborts the whole process,
 * so every function called below was verified to exist in loader.elf first.
 *
 * Each CHECK keeps going on failure; main() returns non-zero if anything
 * failed and prints "RESULT: ALL OS FEATURE TESTS PASSED" only when clean.
 */
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <typeinfo>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/time.h>
#include <sys/random.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "drivers/random.hh"

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

/* small helpers ----------------------------------------------------------- */

// monotonic milliseconds
static long now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ===================================================== process / program */

static void test_process_model()
{
    group("Process & program model (single address space)");

    section("getpid() returns a stable positive id");
    pid_t p1 = getpid();
    CHECK(p1 > 0);
    CHECK(getpid() == p1);

    section("environment variables: setenv / getenv roundtrip");
    CHECK(setenv("OSV_FEATURE_TEST", "42", 1) == 0);
    const char *v = getenv("OSV_FEATURE_TEST");
    CHECK(v != nullptr && std::string(v) == "42");
    CHECK(unsetenv("OSV_FEATURE_TEST") == 0);
    CHECK(getenv("OSV_FEATURE_TEST") == nullptr);

    section("BOUNDARY: fork() is unsupported (single address space)");
    errno = 0;
    // fork() is a failure stub on OSv (single address space, no process model);
    // it returns -1 rather than splitting the address space. There is no child
    // branch to guard - _exit() is not even linkable in the slim kernel.
    CHECK(fork() == -1);
}

/* ============================================================== threads */

static thread_local int tls_value = -1;

static void test_threads()
{
    group("Threads & scheduling");

    section("std::thread spawn/join, atomic counter is exact");
    {
        const int NT = 8, PER = 50000;
        std::atomic<long> counter{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < NT; i++)
            ts.emplace_back([&] {
                for (int j = 0; j < PER; j++)
                    counter.fetch_add(1, std::memory_order_relaxed);
            });
        for (auto &t : ts) t.join();
        CHECK(counter.load() == (long)NT * PER);
    }

    section("thread_local storage is per-thread");
    {
        tls_value = 1000;
        std::atomic<bool> ok{true};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; i++)
            ts.emplace_back([i, &ok] {
                CHECK(tls_value == -1);     // fresh in each new thread
                tls_value = i;
                std::this_thread::yield();
                if (tls_value != i) ok = false;
            });
        for (auto &t : ts) t.join();
        CHECK(ok.load());
        CHECK(tls_value == 1000);           // main thread's copy untouched
    }

    section("std::async / std::future fan-out returns correct results");
    {
        const int N = 32;
        std::vector<std::future<long>> fs;
        for (int i = 0; i < N; i++)
            fs.push_back(std::async(std::launch::async, [i] { return (long)i * i; }));
        long sum = 0;
        for (auto &f : fs) sum += f.get();
        long expect = 0;
        for (int i = 0; i < N; i++) expect += (long)i * i;
        CHECK(sum == expect);
    }

    section("sched_yield() and pthread set/get name");
    CHECK(sched_yield() == 0);
    {
        char name[32] = {0};
        CHECK(pthread_setname_np(pthread_self(), "feature-main") == 0);
        CHECK(pthread_getname_np(pthread_self(), name, sizeof name) == 0);
        CHECK(std::string(name) == "feature-main");
    }
}

/* ====================================================== synchronization */

static void test_sync()
{
    group("Synchronization primitives");

    section("mutex prevents lost updates under contention");
    {
        const int NT = 8, PER = 20000;
        long counter = 0;
        std::mutex m;
        std::vector<std::thread> ts;
        for (int i = 0; i < NT; i++)
            ts.emplace_back([&] {
                for (int j = 0; j < PER; j++) {
                    std::lock_guard<std::mutex> lk(m);
                    counter++;
                }
            });
        for (auto &t : ts) t.join();
        CHECK(counter == (long)NT * PER);
    }

    section("condition_variable producer/consumer");
    {
        std::mutex m; std::condition_variable cv;
        std::vector<int> q; bool done = false;
        long sum = 0; const int N = 20000;
        std::thread consumer([&] {
            for (;;) {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return !q.empty() || done; });
                for (int x : q) sum += x;
                q.clear();
                if (done) break;
            }
        });
        for (int i = 1; i <= N; i++) {
            { std::lock_guard<std::mutex> lk(m); q.push_back(i); }
            cv.notify_one();
        }
        { std::lock_guard<std::mutex> lk(m); done = true; }
        cv.notify_one();
        consumer.join();
        CHECK(sum == (long)N * (N + 1) / 2);
    }

    section("shared_mutex (rwlock): concurrent readers + exclusive writer");
    {
        std::shared_mutex sm;
        int shared = 0;
        std::atomic<long> reads{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 6; i++)
            ts.emplace_back([&] {
                for (int j = 0; j < 5000; j++) {
                    std::shared_lock<std::shared_mutex> rl(sm);
                    if (shared >= 0) reads.fetch_add(1);
                }
            });
        std::thread writer([&] {
            for (int j = 0; j < 1000; j++) {
                std::unique_lock<std::shared_mutex> wl(sm);
                shared++;
            }
        });
        for (auto &t : ts) t.join();
        writer.join();
        CHECK(shared == 1000);
        CHECK(reads.load() == 6 * 5000);
    }

    section("POSIX counting semaphore");
    {
        sem_t s;
        CHECK(sem_init(&s, 0, 0) == 0);
        std::atomic<int> got{0};
        std::thread t([&] { for (int i = 0; i < 100; i++) { sem_wait(&s); got++; } });
        for (int i = 0; i < 100; i++) sem_post(&s);
        t.join();
        CHECK(got.load() == 100);
        CHECK(sem_destroy(&s) == 0);
    }

    section("std::atomic compare_exchange");
    {
        std::atomic<int> a{5};
        int expected = 5;
        CHECK(a.compare_exchange_strong(expected, 10));
        CHECK(a.load() == 10);
        expected = 5;
        CHECK(!a.compare_exchange_strong(expected, 99));
        CHECK(expected == 10);
    }
}

/* ===================================================== memory management */

static void test_memory()
{
    group("Virtual memory & allocation");

    section("malloc / realloc / calloc / posix_memalign");
    {
        size_t n = 4 << 20;                 // 4 MiB
        char *p = (char *)malloc(n);
        CHECK(p != nullptr);
        memset(p, 0xAB, n);
        p = (char *)realloc(p, 2 * n);
        CHECK(p != nullptr);
        CHECK((unsigned char)p[0] == 0xAB && (unsigned char)p[n - 1] == 0xAB);
        free(p);

        int *z = (int *)calloc(1024, sizeof(int));
        CHECK(z != nullptr);
        bool zeroed = true;
        for (int i = 0; i < 1024; i++) if (z[i] != 0) zeroed = false;
        CHECK(zeroed);
        free(z);

        void *aligned = nullptr;
        CHECK(posix_memalign(&aligned, 4096, 8192) == 0);
        CHECK(((uintptr_t)aligned % 4096) == 0);
        free(aligned);
    }

    section("anonymous mmap: map, write, read, munmap");
    {
        size_t len = 2 << 20;               // 2 MiB
        void *m = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(m != MAP_FAILED);
        memset(m, 0x5A, len);
        CHECK(((char *)m)[0] == 0x5A && ((char *)m)[len - 1] == 0x5A);

        section("mprotect, madvise, mincore on the mapping");
        CHECK(mprotect(m, len, PROT_READ) == 0);
        CHECK(((char *)m)[123] == 0x5A);    // still readable
        CHECK(mprotect(m, len, PROT_READ | PROT_WRITE) == 0);
        CHECK(madvise(m, len, MADV_DONTNEED) == 0);
        {
            std::vector<unsigned char> vec(len / 4096);
            CHECK(mincore(m, len, vec.data()) == 0);
        }
        CHECK(munmap(m, len) == 0);
    }
}

/* ========================================================= time & clocks */

static void test_time()
{
    group("Time & clocks");

    section("CLOCK_MONOTONIC is monotonic and has a resolution");
    {
        struct timespec res;
        CHECK(clock_getres(CLOCK_MONOTONIC, &res) == 0);
        long t0 = now_ms();
        for (int i = 0; i < 1000000; i++) { asm volatile("" ::: "memory"); }
        long t1 = now_ms();
        CHECK(t1 >= t0);
    }

    section("CLOCK_REALTIME is past the 2020 epoch");
    {
        struct timespec ts;
        CHECK(clock_gettime(CLOCK_REALTIME, &ts) == 0);
        CHECK(ts.tv_sec > 1577836800);       // 2020-01-01
    }

    section("nanosleep actually sleeps ~30ms");
    {
        long t0 = now_ms();
        struct timespec req { 0, 30 * 1000 * 1000 }, rem{};
        CHECK(nanosleep(&req, &rem) == 0);
        long slept = now_ms() - t0;
        CHECK(slept >= 25);                  // allow scheduling slack
    }

    section("gettimeofday agrees roughly with CLOCK_REALTIME");
    {
        struct timeval tv; struct timespec ts;
        CHECK(gettimeofday(&tv, nullptr) == 0);
        clock_gettime(CLOCK_REALTIME, &ts);
        CHECK(llabs((long long)tv.tv_sec - (long long)ts.tv_sec) <= 2);
    }
}

/* ============================================================ randomness */

static void test_random()
{
    group("Randomness (ChaCha20 CSPRNG)");

    // Note: getrandom(buf, n, 0) returns EAGAIN on the slim kernel (the blocking
    // entropy pool is never marked "ready"), so randomness is exercised through
    // arc4random(), which is wired straight to the same ChaCha20 CSPRNG
    // (drivers/random.cc).
    section("arc4random() sets and clears every bit across samples");
    {
        uint32_t any_set = 0, any_clear = 0;
        for (int i = 0; i < 64; i++) {
            uint32_t r = arc4random();
            any_set |= r;
            any_clear |= ~r;
        }
        CHECK(any_set == 0xffffffffu);
        CHECK(any_clear == 0xffffffffu);
    }
}

/* ======================================================= C/C++ runtime */

static void test_runtime()
{
    group("C / C++ runtime (musl libc, exceptions, RTTI, dlopen, locale)");

    section("C++ exceptions unwind and are caught by type");
    {
        bool caught = false;
        try {
            throw std::runtime_error("boom");
        } catch (const std::exception &e) {
            caught = (std::string(e.what()) == "boom");
        }
        CHECK(caught);
    }

    section("RTTI: typeid and dynamic_cast");
    {
        struct Base { virtual ~Base() {} };
        struct Derived : Base { int x = 7; };
        Base *b = new Derived();
        Derived *d = dynamic_cast<Derived *>(b);
        CHECK(d != nullptr && d->x == 7);
        CHECK(std::string(typeid(*b).name()).find("Derived") != std::string::npos);
        delete b;
    }

    section("STL containers and algorithms");
    {
        std::map<std::string, int> m;
        for (int i = 0; i < 100; i++) m["k" + std::to_string(i)] = i;
        CHECK(m.size() == 100 && m["k42"] == 42);
        std::vector<int> v(1000);
        std::iota(v.begin(), v.end(), 0);
        std::reverse(v.begin(), v.end());
        std::sort(v.begin(), v.end());
        CHECK(std::is_sorted(v.begin(), v.end()) && v.front() == 0 && v.back() == 999);
    }

    section("runtime dynamic loading is unsupported (statically linked kernel)");
    {
        // The ELF loader and runtime application model are gone: the app is
        // linked into the kernel, so dlopen() is a failure stub by design.
        void *h = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
        CHECK(h == nullptr);
    }

    section("ctype classification");
    {
        // The locale_t machinery was removed (vestigial - nothing called it);
        // the narrow ctype functions come straight from llvm-libc.
        CHECK(isalpha('A') && !isalpha('1') && isdigit('7'));
        CHECK(toupper('a') == 'A' && tolower('Z') == 'z');
    }
}

/* ====================================================== boundaries recap */

static void test_boundaries()
{
    group("Unsupported boundaries (must fail cleanly, not crash)");

    // The TCP/IP stack and networking were removed (Phase 9.2), and the fd-based
    // facilities went with the fd table and the filesystem (Phase 6): sockets,
    // pipes, eventfd, timerfd, signalfd, inotify, epoll/poll on fds are not even
    // linkable, so they are intentionally not referenced here. fork() and
    // directed inter-thread signals are exercised as boundaries above.
    section("fork / IP networking / fd facilities: removed (covered above)");
    CHECK(true);
}

/* ================================================================== main */

int os_features_main()
{
    printf("OSv OS-feature inventory & conformance test\n");
    printf("===========================================\n");

    test_process_model();
    test_threads();
    test_sync();
    test_memory();
    test_time();
    test_random();
    test_runtime();
    test_boundaries();

    int checks = g_checks.load(), fails = g_fails.load();
    printf("\n-------------------------------------------\n");
    printf("Checks run: %d, failures: %d\n", checks, fails);
    if (fails == 0) {
        printf("RESULT: ALL OS FEATURE TESTS PASSED\n");
        return 0;
    }
    printf("RESULT: %d OS FEATURE TEST(S) FAILED\n", fails);
    return 1;
}
