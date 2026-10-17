/*
 * The clients of the memory primitives: early and the heap. One header per
 * function, with the tests of that function listed under it. The libc
 * surface over the heap and over anonymous mappings (malloc, mmap) closes the
 * file.
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <malloc.h>
#include <sys/mman.h>

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/early.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/mapping.hh>

#include "mem-test.hh"

extern "C" void *reallocarray(void *ptr, size_t nmemb, size_t size);

using namespace memtest;

namespace {

namespace fr = mem::frames;
namespace map = mem::mapping;

const size_t page = fr::page_size;
const size_t huge = map::huge_page_size;

/* early ------------------------------------------------------------------- */

void early_takes()
{
    function("early::takes");

    test("what fits in a page with its header, and nothing near a page");
    {
        CHECK(mem::early::takes(64, 8));
        CHECK(mem::early::takes(page / 2, 64));
        CHECK(!mem::early::takes(page, 8));
        CHECK(!mem::early::takes(3 * page, 64));
    }
}

void early_alloc()
{
    function("early::alloc");

    test("an object of the size asked, aligned as asked, usable end to end");
    {
        for (size_t align : {size_t(8), size_t(64), size_t(256)}) {
            void *p = mem::early::alloc(100, align);
            CHECK(p != nullptr);
            CHECK((reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0);
            memset(p, 0x77, 100);
            CHECK(static_cast<unsigned char *>(p)[99] == 0x77);
            mem::early::free(p);
        }
    }

    test("as many page-sized objects as boot asks for, all distinct");
    {
        const int n = 512;
        const size_t big = page - 64;
        std::vector<void *> p(n);
        for (int i = 0; i < n; i++) {
            p[i] = mem::early::alloc(big, 8);
            CHECK(p[i] != nullptr);
            memset(p[i], i & 0xff, big);
        }
        bool kept = true;
        for (int i = 0; i < n; i++) {
            kept = kept && static_cast<unsigned char *>(p[i])[big - 1] == (i & 0xff);
        }
        CHECK(kept);
        for (int i = 0; i < n; i++) {
            mem::early::free(p[i]);
        }
    }

    test("what no page can hold gets frames of its own");
    {
        auto *p = static_cast<char *>(mem::early::alloc(3 * page, 64));
        CHECK(p != nullptr);
        CHECK(mem::early::owns(p));
        memset(p, 0x2f, 3 * page);
        CHECK(static_cast<unsigned char>(p[3 * page - 1]) == 0x2f);
        mem::early::free(p);
    }
}

void early_free()
{
    function("early::free");

    test("a page goes back once its last object does");
    {
        size_t before = fr::free_bytes();
        void *a = mem::early::alloc(64, 8);
        void *b = mem::early::alloc(64, 8);
        CHECK(fr::free_bytes() <= before);
        mem::early::free(a);
        mem::early::free(b);
        CHECK(fr::free_bytes() == before);
    }

    test("a big object gives its frames back");
    {
        size_t before = fr::free_bytes();
        void *p = mem::early::alloc(8 * page, 64);
        CHECK(before - fr::free_bytes() >= 8 * page);
        mem::early::free(p);
        CHECK(fr::free_bytes() == before);
    }
}

void early_size_of()
{
    function("early::size_of");

    test("what alloc was asked for, or more for a big object");
    {
        void *p = mem::early::alloc(100, 8);
        CHECK(mem::early::size_of(p) == 100);
        mem::early::free(p);
        void *q = mem::early::alloc(3 * page, 64);
        CHECK(mem::early::size_of(q) >= 3 * page);
        mem::early::free(q);
    }
}

void early_owns()
{
    function("early::owns");

    test("its own objects and not the heap's");
    {
        void *p = mem::early::alloc(64, 8);
        void *h = malloc(64);
        CHECK(mem::early::owns(p));
        CHECK(!mem::heap::owns(p));
        CHECK(!mem::early::owns(h));
        free(h);
        mem::early::free(p);
    }
}

/* heap -------------------------------------------------------------------- */

// What the header in front of an object costs.
const size_t header_bytes = 16;

void heap_ready()
{
    function("heap::ready");

    test("the heap is up, and malloc goes to it");
    {
        CHECK(mem::heap::ready());
        void *p = malloc(100);
        CHECK(mem::heap::owns(p));
        free(p);
    }
}

void heap_takes()
{
    function("heap::takes");

    test("any size, with an alignment up to 2 MiB");
    {
        CHECK(mem::heap::takes(1, 1));
        CHECK(mem::heap::takes(16, 16));
        CHECK(mem::heap::takes(1ul << 20, 4096));
        CHECK(mem::heap::takes(1ul << 30, huge));
        CHECK(!mem::heap::takes(100, 2 * huge));
    }
}

void heap_alloc()
{
    function("heap::alloc");

    test("at least bytes, aligned to at least alignment, writable throughout");
    {
        bool ok = true;
        for (size_t align = 16; align <= huge; align *= 4) {
            for (size_t n : {size_t(1), align, align + 1, size_t(100000), size_t(3ul << 20)}) {
                auto *p = static_cast<unsigned char *>(mem::heap::alloc(n, align));
                ok = ok && p && (reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0;
                if (!p) {
                    continue;
                }
                size_t have = mem::heap::size_of(p);
                ok = ok && have >= n;
                memset(p, 0xd7, have);
                ok = ok && p[have - 1] == 0xd7;
                mem::heap::free(p);
            }
        }
        CHECK(ok);
    }

    test("16-byte alignment when less is asked for");
    {
        bool ok = true;
        for (size_t n : {size_t(1), size_t(7), size_t(24), size_t(100), size_t(4095)}) {
            void *p = mem::heap::alloc(n, 1);
            ok = ok && p && (reinterpret_cast<uintptr_t>(p) & 15) == 0;
            mem::heap::free(p);
        }
        CHECK(ok);
    }

    test("objects are distinct");
    {
        const size_t sizes[] = {1, 8, 17, 64, 100, 512, 4096, 40000, 300000, 4ul << 20};
        std::vector<char *> p;
        for (size_t s : sizes) {
            auto *q = static_cast<char *>(mem::heap::alloc(s, 16));
            CHECK(q != nullptr);
            memset(q, 0x33, s);
            p.push_back(q);
        }
        for (size_t i = 0; i < p.size(); i++) {
            for (size_t j = i + 1; j < p.size(); j++) {
                size_t si = mem::heap::size_of(p[i]), sj = mem::heap::size_of(p[j]);
                CHECK(p[i] + si <= p[j] || p[j] + sj <= p[i]);
            }
        }
        for (char *q : p) {
            mem::heap::free(q);
        }
    }

    test("a large object is usable end to end");
    {
        const size_t bytes = 5 * huge / 2;
        auto *p = static_cast<char *>(mem::heap::alloc(bytes, 16));
        CHECK(p != nullptr);
        memset(p, 0x3c, bytes);
        CHECK(p[0] == 0x3c);
        CHECK(p[bytes - 1] == 0x3c);
        mem::heap::free(p);
    }

    test("nullptr for more than the window holds");
    {
        CHECK(mem::heap::alloc((512ul << 30) + 1, 16) == nullptr);
    }

    test("malloc, new and aligned_alloc all reach it");
    {
        void *a = malloc(100);
        char *b = new char[100];
        void *c = aligned_alloc(256, 512);
        void *d = nullptr;
        CHECK(posix_memalign(&d, 1024, 100) == 0);
        CHECK(mem::heap::owns(a));
        CHECK(mem::heap::owns(b));
        CHECK(mem::heap::owns(c));
        CHECK(mem::heap::owns(d));
        CHECK((reinterpret_cast<uintptr_t>(c) & 255) == 0);
        CHECK((reinterpret_cast<uintptr_t>(d) & 1023) == 0);
        free(a);
        delete[] b;
        free(c);
        free(d);
    }
}

void heap_free()
{
    function("heap::free");

    test("freed neighbours merge into one block");
    {
        // Three neighbours with a live block after them, so that what they
        // merge into is exactly the three.
        const size_t size = 64ul << 10;
        auto *a = static_cast<char *>(mem::heap::alloc(size, 16));
        auto *b = static_cast<char *>(mem::heap::alloc(size, 16));
        auto *c = static_cast<char *>(mem::heap::alloc(size, 16));
        void *fence = mem::heap::alloc(size, 16);
        const size_t block = size + header_bytes;
        if (b != a + block || c != b + block) {
            printf("\t  skipped: another thread allocated in between\n");
        } else {
            mem::heap::free(a);
            mem::heap::free(c);
            mem::heap::free(b);
            // Exactly the three blocks together, which only fits if they merged.
            void *d = mem::heap::alloc(3 * block - header_bytes, 16);
            CHECK(d == a);
            mem::heap::free(d);
            a = b = c = nullptr;
        }
        for (void *p : {static_cast<void *>(a), static_cast<void *>(b),
                        static_cast<void *>(c), fence}) {
            if (p) {
                mem::heap::free(p);
            }
        }
    }

    test("freed memory is reused rather than mapped anew");
    {
        // Cycled, so that each round can only be served from what the last
        // gave back.
        const int n = 8192;
        const size_t size = 3000;
        std::vector<void *> p(n);
        auto cycle = [&] {
            for (int i = 0; i < n; i++) {
                p[i] = mem::heap::alloc(size, 16);
                escape(p[i]);
            }
            for (int i = 0; i < n; i++) {
                mem::heap::free(p[i]);
            }
        };
        cycle();
        size_t before = fr::free_bytes();
        for (int r = 0; r < 20; r++) {
            cycle();
        }
        CHECK(before <= fr::free_bytes() + (4ul << 20));
    }

    test("delete gives back what new took, sized or not");
    {
        struct block { char c[200]; };
        const int n = 20000;
        std::vector<block *> p(n);
        for (int i = 0; i < n; i++) {
            p[i] = new block;
            p[i]->c[0] = char(i);
        }
        bool ok = true;
        for (int i = 0; i < n; i++) {
            ok = ok && p[i]->c[0] == char(i);
        }
        CHECK(ok);
        for (int i = 0; i < n; i++) {
            delete p[i];                // sized: the type is known here
        }
        for (int i = 0; i < n; i++) {
            p[i] = reinterpret_cast<block *>(new char[sizeof(block)]);
        }
        for (int i = 0; i < n; i++) {
            delete[] reinterpret_cast<char *>(p[i]);
        }
    }

    test("an object freed on another cpu than it came from");
    {
        const int n = 4096;
        std::vector<void *> p(n);
        parallel(2, [&](unsigned id) {
            if (id == 0) {
                for (int i = 0; i < n; i++) {
                    p[i] = mem::heap::alloc(64 + (i % 512), 16);
                }
            }
        });
        std::atomic<int> freed{0};
        parallel(2, [&](unsigned id) {
            if (id == 1) {
                for (int i = 0; i < n; i++) {
                    mem::heap::free(p[i]);
                    freed.fetch_add(1);
                }
            }
        });
        CHECK(freed.load() == n);
    }

    test("objects taken and given back on every cpu keep what was written");
    {
        // Every cpu at once, round after round, so the same addresses are
        // handed out again to another cpu than the one that last used them.
        const size_t size = 64ul << 10;
        size_t budget = fr::free_bytes() / 8 / n_cpus() / size;
        size_t each = std::min(std::max(budget, size_t(16)), size_t(2048));
        std::atomic<int> bad{0};
        for (int round = 0; round < 8; round++) {
            parallel(n_cpus(), [&](unsigned id) {
                std::vector<char *> q(each);
                for (size_t i = 0; i < each; i++) {
                    q[i] = static_cast<char *>(mem::heap::alloc(size, 16));
                    if (!q[i]) {
                        bad.fetch_add(1);
                        continue;
                    }
                    for (size_t off = 0; off < size; off += page) {
                        q[i][off] = char(id * 31 + i);
                    }
                }
                for (size_t i = 0; i < each; i++) {
                    if (!q[i]) {
                        continue;
                    }
                    for (size_t off = 0; off < size; off += page) {
                        if (q[i][off] != char(id * 31 + i)) {
                            bad.fetch_add(1);
                        }
                    }
                    mem::heap::free(q[i]);
                }
            });
        }
        CHECK(bad.load() == 0);
    }

}

void heap_size_of()
{
    function("heap::size_of");

    test("what alloc was asked for, rounded up to 16 bytes");
    {
        // A remainder too small to be a free block, 16 bytes, goes with the
        // object.
        bool ok = true;
        for (size_t n : {size_t(1), size_t(100), size_t(4096), size_t(3 * huge + 5)}) {
            void *p = mem::heap::alloc(n, 16);
            size_t have = mem::heap::size_of(p);
            ok = ok && have >= n && have < n + 32;
            mem::heap::free(p);
        }
        CHECK(ok);
        CHECK(malloc_usable_size(nullptr) == 0);
    }

    test("nothing for what is not a heap object");
    {
        void *m = mmap(nullptr, huge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(m != MAP_FAILED);
        CHECK(mem::heap::size_of(m) == 0);
        CHECK(munmap(m, huge) == 0);
    }
}

void heap_owns()
{
    function("heap::owns");

    test("a heap object, and nothing from early, mmap or the stack");
    {
        void *p = mem::heap::alloc(100, 16);
        void *l = mem::heap::alloc(3 * huge, 16);
        void *m = mmap(nullptr, huge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *e = mem::early::alloc(64, 8);
        int on_stack = 0;
        CHECK(mem::heap::owns(p));
        CHECK(mem::heap::owns(l));
        CHECK(!mem::heap::owns(m));
        CHECK(!mem::heap::owns(e));
        CHECK(!mem::heap::owns(&on_stack));
        CHECK(!mem::heap::owns(nullptr));
        mem::early::free(e);
        CHECK(munmap(m, huge) == 0);
        mem::heap::free(l);
        mem::heap::free(p);
    }
}

/* libc -------------------------------------------------------------------- */

void *anon(size_t bytes, int extra = 0)
{
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | extra, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

void libc_malloc()
{
    function("libc: malloc family");

    test("realloc keeps the contents, calloc zeroes");
    {
        char *q = static_cast<char *>(malloc(64));
        memset(q, 0x77, 64);
        q = static_cast<char *>(realloc(q, 8192));
        CHECK(q != nullptr);
        bool ok = true;
        for (int i = 0; i < 64; i++) {
            ok = ok && q[i] == 0x77;
        }
        CHECK(ok);
        free(q);
        char *z = static_cast<char *>(calloc(1024, 4));
        bool zero = true;
        for (int i = 0; i < 4096; i++) {
            zero = zero && z[i] == 0;
        }
        CHECK(zero);
        free(z);
    }

    test("malloc_usable_size is at least what was asked, and all of it is writable");
    {
        bool ok = true;
        for (size_t n = 1; n <= (4ul << 20); n = n + (n >> 2) + 1) {
            auto *q = static_cast<unsigned char *>(malloc(n));
            if (!q) {
                ok = false;
                break;
            }
            size_t usable = malloc_usable_size(q);
            ok = ok && usable >= n;
            memset(q, 0xd7, usable);
            ok = ok && q[usable - 1] == 0xd7;
            ok = ok && (reinterpret_cast<uintptr_t>(q) & (alignof(max_align_t) - 1)) == 0;
            free(q);
        }
        CHECK(ok);
    }

    test("the odd corners hold");
    {
        void *z = malloc(0);
        free(z);
        void *p = realloc(nullptr, 100);
        CHECK(p != nullptr);
        free(realloc(p, 0));
        CHECK(reallocarray(nullptr, SIZE_MAX / 2, 4) == nullptr);
        void *(*volatile vcalloc)(size_t, size_t) = calloc;
        CHECK(vcalloc(SIZE_MAX / 2, 4) == nullptr);
        void *m = memalign(512, 100);
        CHECK(m != nullptr);
        CHECK((reinterpret_cast<uintptr_t>(m) & 511) == 0);
        free(m);
    }
}

void libc_mmap()
{
    function("libc: mmap, munmap, mprotect, madvise, msync");

    test("an anonymous mapping is zeroed, keeps what is written, and unmaps");
    {
        const size_t size = 4ul << 20;
        char *p = static_cast<char *>(anon(size));
        CHECK(p != nullptr);
        bool zero = true;
        for (size_t off = 0; off < size; off += page) {
            zero = zero && p[off] == 0;
        }
        CHECK(zero);
        for (size_t off = 0; off < size; off += page) {
            p[off] = char(off / page);
        }
        bool kept = true;
        for (size_t off = 0; off < size; off += page) {
            kept = kept && p[off] == char(off / page);
        }
        CHECK(kept);
        CHECK(munmap(p, size) == 0);
    }

    test("mappings never overlap, from one thread or many");
    {
        const size_t size = 1ul << 20;
        const int per_thread = 16;
        unsigned threads = n_cpus();
        std::vector<void *> got(threads * per_thread, nullptr);
        parallel(threads, [&](unsigned id) {
            for (int i = 0; i < per_thread; i++) {
                got[id * per_thread + i] = anon(size);
            }
        });
        std::vector<uintptr_t> addr;
        for (void *q : got) {
            CHECK(q != nullptr);
            if (q) {
                addr.push_back(reinterpret_cast<uintptr_t>(q));
            }
        }
        std::sort(addr.begin(), addr.end());
        for (size_t i = 1; i < addr.size(); i++) {
            CHECK(addr[i - 1] + size <= addr[i]);
        }
        for (void *q : got) {
            if (q) {
                munmap(q, size);
            }
        }
    }

    test("a mapping spends its memory at once and gives it back");
    {
        const size_t size = 64ul << 20;
        size_t before = fr::free_bytes();
        char *p = static_cast<char *>(anon(size));
        CHECK(p != nullptr);
        CHECK(before - fr::free_bytes() >= size);
        CHECK(munmap(p, size) == 0);
        CHECK(fr::free_bytes() + (1ul << 20) >= before);
    }

    test("a mapping made on one thread is unmapped on another");
    {
        const size_t size = 2ul << 20;
        void *p = nullptr;
        std::thread a([&] { p = anon(size, MAP_POPULATE); });
        a.join();
        int rc = -1;
        std::thread b([&] { rc = munmap(p, size); });
        b.join();
        CHECK(rc == 0);
    }

    test("a mapping that needs a file is refused");
    {
        void *p = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, -1, 0);
        CHECK(p == MAP_FAILED);
    }

    test("mprotect changes access without unmapping");
    {
        const size_t size = 2ul << 20;
        char *p = static_cast<char *>(anon(size));
        memset(p, 7, size);
        CHECK(mprotect(p, size, PROT_READ) == 0);
        CHECK(p[0] == 7);
        CHECK(p[size - 1] == 7);
        CHECK(mprotect(p, size, PROT_READ | PROT_WRITE) == 0);
        memset(p, 8, size);
        CHECK(p[size - 1] == 8);
        CHECK(munmap(p, size) == 0);
    }

    test("MADV_DONTNEED is accepted and changes nothing");
    {
        const size_t size = 8ul << 20;
        char *p = static_cast<char *>(anon(size));
        memset(p, 9, size);
        size_t populated = fr::free_bytes();
        CHECK(madvise(p, size, MADV_DONTNEED) == 0);
        CHECK(fr::free_bytes() == populated);
        CHECK(p[0] == 9);
        CHECK(p[size - 1] == 9);
        CHECK(munmap(p, size) == 0);
    }

    test("msync on anonymous memory succeeds, and fails off the map");
    {
        const size_t size = 2ul << 20;
        char *p = static_cast<char *>(anon(size));
        CHECK(msync(p, size, MS_SYNC) == 0);
        CHECK(munmap(p, size) == 0);
        CHECK(msync(p, size, MS_SYNC) != 0);
    }

    test("these calls answer only for what mmap handed out");
    {
        void *m = aligned_alloc(page, page);
        CHECK(m != nullptr);
        CHECK(munmap(m, page) != 0);
        CHECK(mprotect(m, page, PROT_READ) != 0);
        CHECK(madvise(m, page, MADV_DONTNEED) != 0);
        CHECK(msync(m, page, MS_SYNC) != 0);
        free(m);
    }
}

}

int os_memory_main()
{
    reset();
    printf("######## memory clients ########\n");
    printf("cpus: %u, free: %zu MiB\n", n_cpus(), fr::free_bytes() >> 20);

    group("early");
    early_takes();
    early_alloc();
    early_free();
    early_size_of();
    early_owns();

    group("heap");
    heap_ready();
    heap_takes();
    heap_alloc();
    heap_free();
    heap_size_of();
    heap_owns();

    group("libc");
    libc_malloc();
    libc_mmap();

    return summary("MEMORY");
}
