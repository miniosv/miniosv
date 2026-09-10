/*
 * The clients of the memory primitives: early, the heap and the page cache,
 * as docs/memory-clients.md describes them. One header per function, with the
 * tests of that function listed under it, then one per documented capability
 * of the page cache. The libc surface over the heap and over anonymous
 * mappings (malloc, mmap) closes the file.
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

#include <malloc.h>
#include <sys/mman.h>

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/kernel_config.h>
#include <osv/mem/early.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/pagecache.hh>
#include <osv/mem/store.hh>
#include <osv/mem/vspace.hh>
#include <osv/mutex.h>

#include "mem-test.hh"

extern "C" void *reallocarray(void *ptr, size_t nmemb, size_t size);

using namespace memtest;

namespace {

namespace fr = mem::frames;
namespace map = mem::mapping;
namespace vs = mem::vspace;
namespace pc = mem::pagecache;

const size_t page = fr::page_size;
const size_t huge = map::huge_page_size;

// The region an address falls in, for telling the heap's paths apart.
vs::region *region_of(const void *p)
{
    return vs::lookup(reinterpret_cast<uintptr_t>(p));
}

char peek(const volatile char *p)
{
    return *p;
}

uint64_t peek64(const void *p)
{
    return *static_cast<const volatile uint64_t *>(p);
}

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

// The size class the document promises for "bytes".
size_t class_size(size_t bytes)
{
    if (bytes <= 16) {
        return 16;
    }
    if (bytes <= 8192) {
        size_t s = 16;
        while (s < bytes) {
            s *= 2;
        }
        return s;
    }
    unsigned e = 63 - __builtin_clzl(bytes - 1);
    size_t step = size_t(1) << (e - 2);
    return align_up(bytes, step);
}

// Whether "p" is in a reservation of its own, which is what the large path makes.
bool is_large(const void *p)
{
    vs::region *r = region_of(p);
    return r && r->span.start == reinterpret_cast<uintptr_t>(p) &&
           r->span.size() < (512ul << 30);
}

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

    test("the paged path rounds to the documented size classes");
    {
        // Powers of two from 16 bytes to 8 KiB, then four classes per power
        // of two up to 1 MiB, each asked for at its widest and one past it.
        bool ok = true;
        for (size_t n = 1; n <= (1ul << 20); n = n + (n >> 3) + 1) {
            void *p = mem::heap::alloc(n, 16);
            ok = ok && p && mem::heap::size_of(p) == class_size(n);
            mem::heap::free(p);
        }
        for (size_t n : {size_t(16), size_t(17), size_t(8192), size_t(8193), size_t(10240),
                         size_t(10241), size_t(1ul << 20)}) {
            void *p = mem::heap::alloc(n, 16);
            ok = ok && p && mem::heap::size_of(p) == class_size(n);
            mem::heap::free(p);
        }
        CHECK(ok);
    }

    test("paged or large, as the size and alignment say");
    {
        struct { size_t bytes, align; bool large; } cases[] = {
            {1ul << 20, 16, false},           // up to 1 MiB with a small alignment
            {(1ul << 20) + 1, 16, true},      // past it
            {4096, 4096, false},              // both within 8 KiB
            {8192, 8192, false},
            {16384, 4096, true},              // alignment past 2048, size past 8 KiB
            {100, huge, true},
        };
        for (auto &c : cases) {
            void *p = mem::heap::alloc(c.bytes, c.align);
            CHECK(p != nullptr);
            CHECK(is_large(p) == c.large);
            CHECK((reinterpret_cast<uintptr_t>(p) & (c.align - 1)) == 0);
            mem::heap::free(p);
        }
    }

    test("a large object has a reservation of its own, in 2 MiB leaves from 2 MiB up");
    {
        auto *p = static_cast<char *>(mem::heap::alloc(5 * huge / 2, 16));
        CHECK(p != nullptr);
        CHECK(is_large(p));
        auto e = map::find(reinterpret_cast<uintptr_t>(p));
        CHECK(bool(e));
        CHECK(e.level() == 1);
        memset(p, 0x3c, 5 * huge / 2);
        CHECK(p[5 * huge / 2 - 1] == 0x3c);
        mem::heap::free(p);

        auto *q = static_cast<char *>(mem::heap::alloc((1ul << 20) + 1, 16));
        CHECK(is_large(q));
        CHECK(map::find(reinterpret_cast<uintptr_t>(q)).level() == 0);
        mem::heap::free(q);
    }

    test("nullptr once memory has run out, and nothing is kept");
    {
        size_t before = fr::free_bytes();
        void *p = mem::heap::alloc(fr::total_available_bytes() + 64 * huge, 16);
        CHECK(p == nullptr);
        CHECK(fr::free_bytes() + (1ul << 20) >= before);
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

    test("an emptied page is reused rather than replaced");
    {
        // Enough for many pages, cycled so that each round can only be served
        // from what the last gave back.
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

    test("the sized form is the unsized one");
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
        void *q = mem::heap::alloc(64, 16);
        mem::heap::free(q, 64);
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

    test("a recycled address is never reached through a stale translation");
    {
        // A working set too big to hold back, so emptied pages are unmapped
        // and their addresses handed out again over new frames, on every cpu.
        // Only another cpu reading the same address would see the old frame.
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

    test("a large object gives its memory and its address back");
    {
        const size_t bytes = 8 * huge;
        size_t before = fr::free_bytes();
        void *p = mem::heap::alloc(bytes, 16);
        CHECK(p != nullptr);
        memset(p, 0x5a, bytes);
        CHECK(before - fr::free_bytes() >= bytes);
        mem::heap::free(p);
        CHECK(!map::find(reinterpret_cast<uintptr_t>(p)));
        CHECK(region_of(p) == nullptr);
        CHECK(fr::free_bytes() + huge >= before);
    }
}

void heap_size_of()
{
    function("heap::size_of");

    test("the size class of a paged object, what was asked for a large one");
    {
        void *p = mem::heap::alloc(100, 16);
        CHECK(mem::heap::size_of(p) == 128);
        mem::heap::free(p);
        void *q = mem::heap::alloc(3 * huge + 5, 16);
        CHECK(mem::heap::size_of(q) == 3 * huge + 5);
        mem::heap::free(q);
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

    test("what alloc handed out, and nothing else");
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

void heap_pressure()
{
    function("heap: memory pressure");

    test("empty pages are held, and given back 16 at a time when asked");
    {
        // A working set of many pages, freed: the heap keeps them mapped.
        // Drained first: one call asks each watcher once.
        while (fr::reclaim(fr::total_available_bytes())) {
        }
        const size_t want = std::min(fr::free_bytes() / 4, 512ul << 20);
        const size_t size = 64ul << 10;
        size_t n = want / size;
        std::vector<void *> p(n);
        size_t before = fr::free_bytes();
        for (size_t i = 0; i < n; i++) {
            p[i] = mem::heap::alloc(size, 16);
        }
        for (size_t i = 0; i < n; i++) {
            mem::heap::free(p[i]);
        }
        size_t held = before - fr::free_bytes();
        CHECK(held > want / 2);

        // One round of asking: the cache has nothing, the heap answers with
        // up to 16 pages, which covers a small request and ends the round.
        size_t low = fr::free_bytes();
        CHECK(fr::reclaim(page));
        size_t gave = fr::free_bytes() - low;
        CHECK(gave >= huge);
        CHECK(gave <= 16 * huge + (1ul << 20));

        // Asked until it has nothing left, it gives everything.
        while (fr::reclaim(fr::total_available_bytes())) {
        }
        CHECK(fr::free_bytes() + (16ul << 20) >= before);
        printf("\t  held %zu MiB, one round gave %zu MiB\n", held >> 20, gave >> 20);
    }

    test("an allocation that would fail is served from what the heap holds");
    {
        const size_t want = std::min(fr::free_bytes() / 4, 512ul << 20);
        const size_t size = 64ul << 10;
        size_t n = want / size;
        std::vector<void *> p(n);
        for (size_t i = 0; i < n; i++) {
            p[i] = mem::heap::alloc(size, 16);
        }
        for (size_t i = 0; i < n; i++) {
            mem::heap::free(p[i]);
        }
        // More than is free, less than free plus what the heap holds.
        size_t free_now = fr::free_bytes();
        size_t ask = free_now + want / 4;
        std::vector<fr::phys_addr> blocks;
        blocks.reserve(ask / huge + 1);
        size_t taken = 0;
        while (taken < ask) {
            auto b = fr::alloc(huge, huge);
            if (b == fr::no_memory) {
                break;
            }
            blocks.push_back(b);
            taken += huge;
        }
        CHECK(taken >= ask);
        for (auto b : blocks) {
            fr::free(b, huge);
        }
    }
}

/* page cache -------------------------------------------------------------- */

/*
 * A store with nothing behind it: every aligned word holds its own offset, so
 * an object of any size costs nothing to serve and every byte read back can be
 * checked. Synchronous; "notify" makes subscribe() call back at once.
 */
struct pattern_store : mem::store {
    explicit pattern_store(uint64_t bytes) : _bytes(bytes) {}

    uint64_t size() override { return _bytes; }
    size_t granularity() override { return _gran; }

    mem::io *read(void *buf, uint64_t off, size_t bytes) override
    {
        fill(static_cast<uint8_t *>(buf), off, bytes);
        reads += bytes;
        return done_with(bytes);
    }

    mem::io *write(const void *buf, uint64_t off, size_t bytes) override
    {
        auto *w = static_cast<const uint64_t *>(buf);
        if (verify) {
            for (size_t i = 0; i < bytes / 8; i++) {
                if (w[i] != word(off + i * 8) && !wrong.fetch_add(1)) {
                    wrong_off = off + i * 8;
                }
            }
        }
        if (record) {
            WITH_LOCK(lock) {
                for (size_t i = 0; i < bytes / 8; i++) {
                    written[off + i * 8] = w[i];
                }
            }
        }
        writes += bytes;
        return done_with(bytes);
    }

    bool done(mem::io *) override { return true; }

    int64_t wait(mem::io *req) override
    {
        auto *r = reinterpret_cast<int64_t *>(req);
        int64_t n = *r;
        delete r;
        return n;
    }

    bool subscribe(mem::io *, void (*fn)(void *), void *arg) override
    {
        if (!notify) {
            return false;
        }
        fn(arg);
        return true;
    }

    static uint64_t word(uint64_t off) { return off; }

    static void fill(uint8_t *p, uint64_t off, size_t n)
    {
        size_t whole = n & ~size_t(7);
        auto *w = reinterpret_cast<uint64_t *>(p);
        for (size_t i = 0; i < whole / 8; i++) {
            w[i] = off + i * 8;
        }
        for (size_t i = whole; i < n; i++) {
            p[i] = uint8_t((off + (i & ~size_t(7))) >> (8 * (i & 7)));
        }
    }

    uint64_t written_at(uint64_t off)
    {
        WITH_LOCK(lock) {
            auto it = written.find(off);
            return it == written.end() ? ~uint64_t(0) : it->second;
        }
    }

    uint64_t _bytes;
    size_t _gran = 4096;
    bool record = false;
    bool verify = false;
    bool notify = false;
    std::atomic<size_t> reads{0}, writes{0}, wrong{0};
    std::atomic<uint64_t> wrong_off{0};

private:
    mutex lock;
    std::map<uint64_t, uint64_t> written;

    mem::io *done_with(size_t bytes)
    {
        return reinterpret_cast<mem::io *>(new int64_t(bytes));
    }
};

// Whether every word of [off, off + len) reads as the store says.
bool matches(const char *b, uint64_t off, uint64_t len)
{
    bool ok = true;
    for (uint64_t o = off; o + 8 <= off + len; o += 8) {
        ok &= peek64(b + o) == pattern_store::word(o);
    }
    return ok;
}

// Tiles of one fixed size.
template <uint64_t N>
void fault_fixed(void *, uint64_t off, uint64_t *start, uint64_t *len)
{
    *len = N;
    *start = (off / N) * N;
}

// Three pages and a bit, so consecutive tiles begin and end part-way through a page.
const uint64_t ragged_span = 3 * 4096 + 1000;
const uint64_t big_span = (5ull << 20) + 12345;

// Different tile sizes in one region: pages in the first MiB, 64 KiB tiles in
// the second, 1 MiB tiles after that.
void fault_mixed(void *, uint64_t off, uint64_t *start, uint64_t *len)
{
    uint64_t n = off < (1ul << 20) ? 4096 : off < (2ul << 20) ? 65536 : (1ul << 20);
    *len = n;
    *start = (off / n) * n;
}

// Asks for more than a tile may be.
void fault_oversize(void *, uint64_t off, uint64_t *start, uint64_t *len)
{
    *len = 2ull << 30;
    *start = (off / *len) * *len;
}

// Sequential readahead: what follows the tile that faulted, to the depth given.
void readahead(void *, pc::buffer &b, pc::offset_list &also)
{
    uint64_t step = pc::size(b);
    uint64_t next = pc::offset(b) + step;
    for (unsigned i = 0; i < also.max; i++, next += step) {
        also.add(next);
    }
}

// The default policy with another tile size. Static: a mapping keeps a
// reference to its policy for as long as it lives.
template <void (*extent)(void *, uint64_t, uint64_t *, uint64_t *)>
const pc::policy &policy_with()
{
    static const pc::policy p = [] {
        pc::policy q = pc::defaults();
        q.fault_extent = extent;
        return q;
    }();
    return p;
}

/*
 * A policy that records everything the cache tells it, so every hook and every
 * buffer helper is checked from the policy's side. Victims come off a stack
 * threaded through policy_data(), oldest last.
 */
struct spy_state {
    uint64_t created_size = 0;
    std::atomic<int> faults{0}, evicted{0}, prefetched_seen{0};
    std::atomic<int> accessed_at_fault{0}, accessed_at_evict{0}, dirty_at_evict{0};
    std::atomic<unsigned> victims_max{0};
    std::atomic<bool> helpers_ok{true};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> say_dirty{false};
    mutex lock;
    pc::buffer *top = nullptr;
};

spy_state g_spy;

void *spy_create(uint64_t store_size)
{
    g_spy.created_size = store_size;
    g_spy.faults = 0;
    g_spy.evicted = 0;
    g_spy.prefetched_seen = 0;
    g_spy.accessed_at_fault = 0;
    g_spy.accessed_at_evict = 0;
    g_spy.dirty_at_evict = 0;
    g_spy.victims_max = 0;
    g_spy.helpers_ok = true;
    g_spy.destroyed = false;
    g_spy.top = nullptr;
    return &g_spy;
}

void spy_destroy(void *p)
{
    g_spy.destroyed = p == &g_spy;
}

void spy_on_fault(void *p, pc::buffer &b)
{
    auto *st = static_cast<spy_state *>(p);
    st->faults.fetch_add(1);
    bool ok = pc::size(b) > 0 && pc::size(b) <= 65536 &&
              pc::offset(b) + pc::size(b) <= st->created_size &&
              pc::data(b) != nullptr && pc::policy_data(b) != nullptr;
    // The contents are there before the policy hears of the buffer.
    if (ok && pc::size(b) >= 8 && pc::offset(b) % 8 == 0) {
        ok = *static_cast<uint64_t *>(pc::data(b)) == pattern_store::word(pc::offset(b));
    }
    if (!ok) {
        st->helpers_ok = false;
    }
    st->accessed_at_fault.fetch_add(pc::accessed(b));
    st->prefetched_seen.fetch_add(pc::prefetched(b));
    WITH_LOCK(st->lock) {
        *static_cast<pc::buffer **>(pc::policy_data(b)) = st->top;
        st->top = &b;
    }
}

void spy_evict(void *p, size_t bytes, pc::buffer_list &victims)
{
    auto *st = static_cast<spy_state *>(p);
    st->victims_max = victims.max;
    size_t got = 0;
    WITH_LOCK(st->lock) {
        while (st->top && got < bytes && !victims.full()) {
            pc::buffer *b = st->top;
            st->top = *static_cast<pc::buffer **>(pc::policy_data(*b));
            st->accessed_at_evict.fetch_add(pc::accessed(*b));
            st->dirty_at_evict.fetch_add(pc::dirty(*b));
            victims.add(b);
            got += pc::size(*b);
        }
    }
}

void spy_on_evicted(void *p, pc::buffer &)
{
    static_cast<spy_state *>(p)->evicted.fetch_add(1);
}

bool spy_is_dirty(void *p, pc::buffer &)
{
    return static_cast<spy_state *>(p)->say_dirty.load();
}

const pc::policy spy_policy = {
    .bytes_per_buffer = sizeof(void *),
    .create = spy_create,
    .destroy = spy_destroy,
    .fault_extent = fault_fixed<ragged_span>,
    .prefetch = nullptr,
    .prefetch_depth = 0,
    .evict = spy_evict,
    .on_fault = spy_on_fault,
    .on_evicted = spy_on_evicted,
    .is_dirty = spy_is_dirty,
};

void store_interface()
{
    function("mem::store");

    test("read_now and write_now move bytes over read, write and wait");
    {
        pattern_store s(1ul << 20);
        s.record = true;
        std::vector<uint8_t> buf(3 * page);
        CHECK(s.read_now(buf.data(), 8192, buf.size()) == (int64_t)buf.size());
        CHECK(matches(reinterpret_cast<char *>(buf.data()) - 8192, 8192, buf.size()));

        uint64_t magic = 0x1122334455667788ull;
        memcpy(buf.data(), &magic, 8);
        CHECK(s.write_now(buf.data(), 4096, 8) == 8);
        CHECK(s.written_at(4096) == magic);

        mem::io *req = s.read(buf.data(), 0, page);
        CHECK(req != nullptr);
        CHECK(s.done(req));
        CHECK(s.wait(req) == (int64_t)page);
    }
}

void pagecache_map()
{
    function("pagecache::map");

    test("the mapping is the store's size rounded to a page, 2 MiB aligned");
    {
        pattern_store s(4 * huge + 1234);
        void *m = pc::map(s);
        CHECK(m != nullptr);
        CHECK((reinterpret_cast<uintptr_t>(m) & (huge - 1)) == 0);
        vs::region *r = region_of(m);
        CHECK(r != nullptr);
        if (r) {
            CHECK(r->span.start == reinterpret_cast<uintptr_t>(m));
            CHECK(r->span.size() == 4 * huge + page);
        }
        CHECK(!pc::resident(m));
        CHECK(s.reads.load() == 0);
        pc::unmap(m);
    }

    test("refused for an empty store or a granularity over a page");
    {
        pattern_store empty(0);
        CHECK(pc::map(empty) == nullptr);
        pattern_store coarse(4 * huge);
        coarse._gran = 8192;
        CHECK(pc::map(coarse) == nullptr);
        pattern_store fine(4 * huge);
        fine._gran = 512;
        void *m = pc::map(fine);
        CHECK(m != nullptr);
        pc::unmap(m);
    }

    test("two mappings live side by side and die separately");
    {
        pattern_store s1(4 * huge), s2(4 * huge);
        char *m1 = static_cast<char *>(pc::map(s1));
        char *m2 = static_cast<char *>(pc::map(s2));
        CHECK(m1 != nullptr);
        CHECK(m2 != nullptr);
        CHECK(m1 != m2);
        CHECK(peek64(m1) == pattern_store::word(0));
        CHECK(peek64(m2 + 8) == pattern_store::word(8));
        pc::unmap(m1);
        CHECK(peek64(m2 + 16) == pattern_store::word(16));
        CHECK(pc::resident(m2));
        pc::unmap(m2);
    }
}

void pagecache_unmap()
{
    function("pagecache::unmap");

    test("writes back what is dirty, gives the memory back, releases the address");
    {
        pattern_store s(16 * huge);
        s.record = true;
        size_t before = fr::free_bytes();
        auto *m = static_cast<char *>(pc::map(s));
        CHECK(m != nullptr);
        CHECK(matches(m, 0, 64 * page));
        *reinterpret_cast<uint64_t *>(m + 4 * page) = 0xbeef;
        CHECK(before - fr::free_bytes() >= 64 * page);
        pc::unmap(m);
        CHECK(s.written_at(4 * page) == 0xbeef);
        // The policy's own state costs the heap a few pages.
        CHECK(fr::free_bytes() + (8ul << 20) >= before);
        CHECK(region_of(m) == nullptr);
        CHECK(!pc::resident(m));
    }

    test("the policy is destroyed with the mapping");
    {
        pattern_store s(16 * huge);
        void *m = pc::map(s, spy_policy);
        CHECK(!g_spy.destroyed.load());
        pc::unmap(m);
        CHECK(g_spy.destroyed.load());
    }
}

void pagecache_sync()
{
    function("pagecache::sync");

    test("writes back the dirty buffers in the range and reports the bytes");
    {
        pattern_store s(16 * huge);
        s.record = true;
        auto *m = static_cast<char *>(pc::map(s));
        auto *w = reinterpret_cast<uint64_t *>(m);
        w[0] = 0xfeed;
        CHECK(pc::sync(m, page) == (int64_t)page);
        CHECK(s.written_at(0) == 0xfeed);
        CHECK(s.writes.load() == page);
        pc::unmap(m);
    }

    test("nothing to write is zero bytes, where the hardware tracks writes");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s));
        *reinterpret_cast<uint64_t *>(m) = 0xfeed;
        CHECK(pc::sync(m, page) == (int64_t)page);
        if (map::tracks_writes) {
            CHECK(pc::sync(m, page) == 0);
            CHECK(s.writes.load() == page);
            CHECK(matches(m, page, page));
            CHECK(pc::sync(m, 2 * page) == 0);
        }
        pc::unmap(m);
    }

    test("only the buffers overlapping the range");
    {
        pattern_store s(16 * huge);
        s.record = true;
        auto *m = static_cast<char *>(pc::map(s));
        *reinterpret_cast<uint64_t *>(m) = 1;
        *reinterpret_cast<uint64_t *>(m + 8 * page) = 2;
        CHECK(pc::sync(m, page) == (int64_t)page);
        CHECK(s.written_at(0) == 1);
        CHECK(s.written_at(8 * page) == ~uint64_t(0));
        CHECK(pc::sync(m + 8 * page + 100, 1) == (int64_t)page);
        CHECK(s.written_at(8 * page) == 2);
        pc::unmap(m);
    }

    test("-EINVAL outside a mapping");
    {
        void *h = malloc(page);
        CHECK(pc::sync(h, page) == -EINVAL);
        free(h);
        pattern_store s(4 * huge);
        auto *m = static_cast<char *>(pc::map(s));
        pc::unmap(m);
        CHECK(pc::sync(m, page) == -EINVAL);
    }
}

void pagecache_resident()
{
    function("pagecache::resident");

    test("false until the page is brought in, true after, false once it is gone");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s));
        CHECK(!pc::resident(m));
        CHECK(!pc::resident(m + page - 1));
        CHECK(peek64(m) == pattern_store::word(0));
        CHECK(pc::resident(m));
        CHECK(pc::resident(m + page - 1));
        CHECK(!pc::resident(m + page));
        pc::unmap(m);
        CHECK(!pc::resident(m));
    }

    test("false outside a mapping");
    {
        void *h = malloc(page);
        CHECK(!pc::resident(h));
        free(h);
    }
}

void pagecache_fetch()
{
    function("pagecache::fetch");

    test("brings in every buffer overlapping the range, and reports the bytes resident");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s));
        CHECK(pc::fetch(m, 32 * page) == 32 * page);
        CHECK(s.reads.load() == 32 * page);
        for (unsigned i = 0; i < 32; i++) {
            CHECK(pc::resident(m + i * page));
        }
        CHECK(!pc::resident(m + 32 * page));
        CHECK(matches(m, 0, 32 * page));
        CHECK(s.reads.load() == 32 * page);
        pc::unmap(m);
    }

    test("a range starting inside a page counts only what it covers");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s));
        CHECK(pc::fetch(m + 100, page) == page);
        CHECK(pc::resident(m));
        CHECK(pc::resident(m + page));
        CHECK(pc::fetch(m + 100, 50) == 50);
        pc::unmap(m);
    }

    test("whole buffers, however the range cuts them");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s, policy_with<fault_fixed<65536>>()));
        CHECK(pc::fetch(m + 3 * page, page) == page);
        CHECK(s.reads.load() == 65536);
        for (unsigned i = 0; i < 16; i++) {
            CHECK(pc::resident(m + i * page));
        }
        pc::unmap(m);
    }

    test("zero outside a mapping");
    {
        void *h = malloc(page);
        CHECK(pc::fetch(h, page) == 0);
        free(h);
    }
}

void pagecache_access()
{
    function("pagecache: access through the pointer");

    test("a page is read in when it is touched, and not before");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s));
        CHECK(s.reads.load() == 0);
        CHECK(peek64(m) == pattern_store::word(0));
        CHECK(s.reads.load() == page);
        CHECK(!pc::resident(m + 8 * huge));
        CHECK(peek64(m + 8 * huge) == pattern_store::word(8 * huge));
        CHECK(s.reads.load() == 2 * page);
        pc::unmap(m);
    }

    test("what is written stays, and is what the next reader sees");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s));
        auto *w = reinterpret_cast<uint64_t *>(m);
        for (unsigned i = 0; i < 1024; i++) {
            w[i] = i * 3;
        }
        bool ok = true;
        for (unsigned i = 0; i < 1024; i++) {
            ok = ok && peek64(w + i) == i * 3;
        }
        CHECK(ok);
        pc::unmap(m);
    }

    test("past the store but inside the page rounding reads as zero");
    {
        const uint64_t odd = 4 * huge + 1234;
        pattern_store s(odd);
        auto *m = static_cast<char *>(pc::map(s));
        const uint64_t last = (odd - 8) & ~uint64_t(7);
        CHECK(peek64(m + last) == pattern_store::word(last));
        CHECK(peek(m + odd) == 0);
        CHECK(peek(m + odd + 100) == 0);
        CHECK(peek(m + align_up(odd, page) - 1) == 0);
        pc::unmap(m);
    }
}

void capability_buffer_size()
{
    function("capability: buffers of any size, each one its own");

    test("the default is one page per fault");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s));
        CHECK(peek64(m + 5 * page) == pattern_store::word(5 * page));
        CHECK(s.reads.load() == page);
        CHECK(pc::resident(m + 5 * page));
        CHECK(!pc::resident(m + 4 * page));
        CHECK(!pc::resident(m + 6 * page));
        pc::unmap(m);
    }

    test("a buffer is as big as the policy asks for, in one transfer");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s, policy_with<fault_fixed<65536>>()));
        CHECK(peek64(m + 8 * page) == pattern_store::word(8 * page));
        CHECK(s.reads.load() == 65536);
        for (unsigned i = 0; i < 16; i++) {
            CHECK(pc::resident(m + i * page));
        }
        CHECK(!pc::resident(m + 16 * page));
        pc::unmap(m);
    }

    test("buffers of different sizes in one region");
    {
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s, policy_with<fault_mixed>()));
        CHECK(peek64(m + 3 * page) == pattern_store::word(3 * page));
        CHECK(s.reads.load() == page);
        CHECK(!pc::resident(m + 4 * page));

        size_t before = s.reads.load();
        uint64_t o = (1ul << 20) + 3 * page;
        CHECK(peek64(m + o) == pattern_store::word(o));
        CHECK(s.reads.load() - before == 65536);
        CHECK(pc::resident(m + (1ul << 20) + 15 * page));
        CHECK(!pc::resident(m + (1ul << 20) + 16 * page));

        before = s.reads.load();
        o = (3ul << 20) + 100 * page;
        CHECK(peek64(m + o) == pattern_store::word(o));
        CHECK(s.reads.load() - before == (1ul << 20));
        CHECK(pc::resident(m + (3ul << 20)));
        CHECK(pc::resident(m + (4ul << 20) - page));
        CHECK(!pc::resident(m + (4ul << 20)));
        pc::unmap(m);
    }

    test("a buffer of an awkward size holds every page it touches, shared edges included");
    {
        /*
         *   bytes 0     13288       26576       39864       53152
         *         |  b0   |    b1     |    b2     |    b3     |
         *   pages 0  1  2  3  4  5  6  7  8  9 10 11 12
         *
         * b2 is [26576, 39864): pages 7 and 8 are inside it, pages 6 and 9 it
         * shares with its neighbours, and it holds those whole.
         */
        const uint64_t span = ragged_span;
        pattern_store s(16 * huge);
        auto *m = static_cast<char *>(pc::map(s, policy_with<fault_fixed<ragged_span>>()));

        const uint64_t b2 = 2 * span;
        const uint64_t lo = b2 / page;
        const uint64_t hi = (3 * span - 1) / page;
        const uint64_t mid = (b2 + span / 2) & ~uint64_t(7);
        CHECK(peek64(m + mid) == pattern_store::word(mid));
        for (uint64_t i = lo; i <= hi; i++) {
            CHECK(pc::resident(m + i * page));
        }
        CHECK(!pc::resident(m + (lo - 1) * page));
        CHECK(!pc::resident(m + (hi + 1) * page));
        CHECK(s.reads.load() == (hi - lo + 1) * page);

        // The neighbour finds one of its pages already there and comes up
        // short by exactly that page.
        size_t before = s.reads.load();
        CHECK(peek64(m + span) == pattern_store::word(span));
        CHECK(s.reads.load() - before == (lo - span / page) * page);
        CHECK(pc::resident(m + (span / page) * page));
        CHECK(!pc::resident(m + (span / page - 1) * page));

        CHECK(matches(m, 0, 4 * span));
        pc::unmap(m);
    }

    test("a page two buffers fall in goes to the one that was reached for");
    {
        const uint64_t span = ragged_span;
        const uint64_t seam = span / page;
        const uint64_t here = seam * page;                 // still b0
        {
            pattern_store s(16 * huge);
            auto *m = static_cast<char *>(pc::map(s, policy_with<fault_fixed<ragged_span>>()));
            CHECK(peek64(m + here) == pattern_store::word(here));
            CHECK(pc::resident(m));
            CHECK(pc::resident(m + here));
            CHECK(!pc::resident(m + here + page));
            CHECK(s.reads.load() == (seam + 1) * page);
            pc::unmap(m);
        }
        {
            pattern_store s(16 * huge);
            auto *m = static_cast<char *>(pc::map(s, policy_with<fault_fixed<ragged_span>>()));
            CHECK(peek64(m + span) == pattern_store::word(span));   // b1 begins here
            CHECK(!pc::resident(m));
            CHECK(pc::resident(m + here));
            CHECK(pc::resident(m + here + page));
            const uint64_t last = (2 * span - 1) / page;
            CHECK(s.reads.load() == (last + 1 - seam) * page);
            pc::unmap(m);
        }
    }

    test("a buffer may be 1 GiB and no more");
    {
        if (fr::free_bytes() < (3ul << 30)) {
            printf("\t  skipped: needs 3 GiB free\n");
        } else {
            pattern_store s(4ul << 30);
            auto *m = static_cast<char *>(pc::map(s, policy_with<fault_fixed<1ul << 30>>()));
            CHECK(peek64(m + (1ul << 30) + 12345 * page) ==
                  pattern_store::word((1ul << 30) + 12345 * page));
            CHECK(s.reads.load() == (1ul << 30));
            CHECK(pc::resident(m + (1ul << 30)));
            CHECK(pc::resident(m + (2ul << 30) - page));
            CHECK(!pc::resident(m + (2ul << 30)));
            pc::unmap(m);

            pattern_store t(4ul << 30);
            m = static_cast<char *>(pc::map(t, policy_with<fault_oversize>()));
            CHECK(peek64(m) == pattern_store::word(0));
            CHECK(t.reads.load() == (1ul << 30));
            CHECK(!pc::resident(m + (1ul << 30)));
            pc::unmap(m);
        }
    }

    test("eviction takes whole buffers");
    {
        const size_t cap = 32 << 20;
        const uint64_t tile = 65536;
        pattern_store s(1ull << 30);
        auto *m = static_cast<char *>(pc::map(s, policy_with<fault_fixed<tile>>(), cap));
        bool ok = true;
        for (uint64_t off = 0; off < 8 * size_t(cap); off += tile) {
            ok &= peek64(m + off + 8) == pattern_store::word(off + 8);
        }
        CHECK(ok);
        unsigned partial = 0, whole = 0;
        for (uint64_t off = 0; off < 8 * size_t(cap); off += tile) {
            unsigned in = 0;
            for (uint64_t p = 0; p < tile; p += page) {
                in += pc::resident(m + off + p);
            }
            partial += in != 0 && in != tile / page;
            whole += in == tile / page;
        }
        CHECK(partial == 0);
        CHECK(whole > 0);
        pc::unmap(m);
    }
}

void capability_limit_and_policy()
{
    function("capability: a limit and a policy of the region's own");

    test("a region with a limit stays inside it");
    {
        const size_t cap = 64 << 20;
        pattern_store s(8ull << 30);
        void *m = pc::map(s, pc::defaults(), cap);
        CHECK(m != nullptr);
        size_t before = fr::free_bytes();
        auto *b = static_cast<char *>(m);
        bool ok = true;
        size_t worst = 0;
        for (uint64_t off = 0; off < 8 * size_t(cap); off += page) {
            ok &= peek64(b + off) == pattern_store::word(off);
            worst = std::max(worst, before - fr::free_bytes());
        }
        CHECK(ok);
        CHECK(worst < cap + (cap / 2));
        printf("\t  a %zu MiB limit held at most %zu MiB\n", cap >> 20, worst >> 20);
        pc::unmap(m);
    }

    test("big buffers cycle through a limit");
    {
        const size_t cap = 64 << 20;
        pattern_store s(1ull << 30);
        size_t before = fr::free_bytes();
        auto *b = static_cast<char *>(pc::map(s, policy_with<fault_fixed<big_span>>(), cap));
        bool ok = true;
        size_t worst = 0;
        for (uint64_t off = 0; off + 8 <= 8 * uint64_t(cap); off += big_span) {
            uint64_t o = (off + big_span / 2) & ~uint64_t(7);
            ok &= peek64(b + o) == pattern_store::word(o);
            worst = std::max(worst, before - fr::free_bytes());
        }
        CHECK(ok);
        CHECK(worst < cap + (cap / 2));
        pc::unmap(b);
    }

    test("a working set larger than the limit is served, and served correctly");
    {
        const size_t limit = 256 << 20;
        const uint64_t bytes = 1ull << 30;
        pattern_store s(bytes);
        auto *b = static_cast<char *>(pc::map(s, pc::defaults(), limit));
        CHECK(matches(b, 0, bytes));
        CHECK(s.reads.load() >= bytes);
        // The start was faulted longest ago, so going back reads it again.
        size_t after = s.reads.load();
        CHECK(matches(b, 0, bytes / 8));
        CHECK(s.reads.load() - after > bytes / 8 / 2);
        pc::unmap(b);
    }

    test("two regions each keep to their own limit and policy");
    {
        pattern_store s1(1ull << 30), s2(1ull << 30);
        size_t before = fr::free_bytes();
        auto *a = static_cast<char *>(pc::map(s1, pc::defaults(), 16 << 20));
        auto *b = static_cast<char *>(pc::map(s2, policy_with<fault_fixed<65536>>(), 64 << 20));
        bool ok = true;
        size_t worst = 0;
        for (uint64_t off = 0; off < (256ul << 20); off += page) {
            ok &= peek64(a + off) == pattern_store::word(off);
            ok &= peek64(b + off) == pattern_store::word(off);
            worst = std::max(worst, before - fr::free_bytes());
        }
        CHECK(ok);
        CHECK(worst < (80ul << 20) + (40ul << 20));
        // The second reads 16 pages at a time, the first one.
        CHECK(s1.reads.load() >= (256ul << 20));
        CHECK(s2.reads.load() >= (256ul << 20));
        CHECK(s2.reads.load() % 65536 == 0);
        pc::unmap(a);
        pc::unmap(b);
    }

    test("a policy of the application's own sees the life of every buffer");
    {
        const uint64_t bytes = 64ul << 20;
        pattern_store s(bytes);
        auto *b = static_cast<char *>(pc::map(s, spy_policy, 8ul << 20));
        CHECK(b != nullptr);
        CHECK(g_spy.created_size == bytes);
        b[0] = 0x55;
        bool ok = true;
        for (uint64_t off = page; off < bytes; off += ragged_span) {
            uint64_t o = off & ~uint64_t(7);
            ok &= peek64(b + o) == pattern_store::word(o);
        }
        CHECK(ok);
        CHECK(g_spy.helpers_ok.load());
        CHECK(g_spy.faults.load() >= int(bytes / ragged_span) / 2);
        CHECK(g_spy.evicted.load() > 0);
        CHECK(g_spy.victims_max.load() == 64);
        // Touched buffers read as accessed by the time they are evicted.
        CHECK(g_spy.accessed_at_evict.load() > 0);
        pc::unmap(b);
        CHECK(g_spy.evicted.load() == g_spy.faults.load());
        CHECK(g_spy.destroyed.load());
        // The policy said nothing was dirty, so nothing was written.
        CHECK(s.writes.load() == 0);
    }

    test("the policy's is_dirty decides what is written back");
    {
        pattern_store s(64ul << 20);
        s.verify = true;
        auto *b = static_cast<char *>(pc::map(s, spy_policy, 8ul << 20));
        g_spy.say_dirty = true;
        CHECK(matches(b, 0, 16ul << 20));
        // Nothing was written, yet every evicted buffer went back.
        CHECK(g_spy.evicted.load() > 0);
        CHECK(s.writes.load() > 0);
        CHECK(s.wrong.load() == 0);
        g_spy.say_dirty = false;
        pc::unmap(b);
    }

    test("without is_dirty the hardware bit decides");
    {
        pattern_store s(64ul << 20);
        s.verify = true;
        auto *b = static_cast<char *>(pc::map(s, pc::defaults(), 8ul << 20));
        CHECK(matches(b, 0, 32ul << 20));
        if (map::tracks_writes) {
            CHECK(s.writes.load() == 0);
        }
        for (uint64_t off = 0; off < (32ul << 20); off += page) {
            *reinterpret_cast<uint64_t *>(b + off) = pattern_store::word(off);
        }
        CHECK(matches(b, 0, 32ul << 20));
        CHECK(s.writes.load() > 0);
        CHECK(s.wrong.load() == 0);
        pc::unmap(b);
    }
}

void capability_prefetch()
{
    function("capability: prefetch at fault time");

    test("the policy's prefetches are read with the fault and installed later without a read");
    {
        pattern_store s(16 * huge);
        pc::policy p = pc::defaults();
        p.prefetch = readahead;
        p.prefetch_depth = 4;
        auto *b = static_cast<char *>(pc::map(s, p));
        CHECK(peek64(b) == pattern_store::word(0));
        CHECK(pc::resident(b));
        CHECK(s.reads.load() == 5 * page);
        for (unsigned i = 1; i <= 4; i++) {
            CHECK(!pc::resident(b + i * page));
        }
        CHECK(!pc::resident(b + 5 * page));
        for (unsigned i = 1; i <= 4; i++) {
            CHECK(peek64(b + i * page) == pattern_store::word(i * page));
            CHECK(pc::resident(b + i * page));
        }
        CHECK(s.reads.load() == 5 * page);
        pc::unmap(b);
    }

    test("a store that announces completion has them installed at once");
    {
        pattern_store s(16 * huge);
        s.notify = true;
        pc::policy p = pc::defaults();
        p.prefetch = readahead;
        p.prefetch_depth = 4;
        auto *b = static_cast<char *>(pc::map(s, p));
        CHECK(peek64(b) == pattern_store::word(0));
        CHECK(s.reads.load() == 5 * page);
        for (unsigned i = 1; i <= 4; i++) {
            CHECK(pc::resident(b + i * page));
        }
        CHECK(!pc::resident(b + 5 * page));
        CHECK(matches(b, 0, 5 * page));
        CHECK(s.reads.load() == 5 * page);
        pc::unmap(b);
    }

    test("the policy is told which buffers were guesses");
    {
        // Page-aligned tiles, so that no tile shares a page with the next.
        const uint64_t tile = 16384;
        pattern_store s(64ul << 20);
        pc::policy p = spy_policy;
        p.fault_extent = fault_fixed<tile>;
        p.prefetch = readahead;
        p.prefetch_depth = 4;
        auto *b = static_cast<char *>(pc::map(s, p));
        CHECK(peek64(b) == pattern_store::word(0));
        CHECK(g_spy.prefetched_seen.load() == 0);
        for (unsigned i = 1; i <= 4; i++) {
            CHECK(peek64(b + i * tile) == pattern_store::word(i * tile));
        }
        CHECK(g_spy.prefetched_seen.load() == 4);
        CHECK(g_spy.faults.load() == 5);
        pc::unmap(b);
    }

    test("no prefetch function, or a depth of zero, means none");
    {
        pattern_store s(16 * huge);
        pc::policy p = pc::defaults();
        p.prefetch = readahead;
        p.prefetch_depth = 0;
        auto *b = static_cast<char *>(pc::map(s, p));
        CHECK(peek64(b) == pattern_store::word(0));
        CHECK(s.reads.load() == page);
        pc::unmap(b);
    }
}

void capability_huge_frames()
{
    function("capability: 2 MiB frames where a buffer allows");

    test("an aligned 2 MiB buffer is one leaf, a page buffer is not");
    {
        pattern_store s(64 * huge);
        auto *b = static_cast<char *>(pc::map(s, policy_with<fault_fixed<huge>>()));
        CHECK(peek64(b + 3 * huge + 100 * page) == pattern_store::word(3 * huge + 100 * page));
        auto e = map::find(reinterpret_cast<uintptr_t>(b) + 3 * huge);
        CHECK(bool(e));
        CHECK(e.level() == 1);
        CHECK(s.reads.load() == huge);
        pc::unmap(b);

        pattern_store t(64 * huge);
        b = static_cast<char *>(pc::map(t));
        CHECK(peek64(b + 3 * huge) == pattern_store::word(3 * huge));
        CHECK(map::find(reinterpret_cast<uintptr_t>(b) + 3 * huge).level() == 0);
        pc::unmap(b);
    }

    test("a big misaligned buffer takes 2 MiB leaves inside and pages at the edges");
    {
        pattern_store s(1ull << 30);
        auto *b = static_cast<char *>(pc::map(s, policy_with<fault_fixed<big_span>>()));
        const uint64_t off = (big_span + 3 * page) & ~uint64_t(7);
        CHECK(peek64(b + off) == pattern_store::word(off));
        size_t foot = ((2 * big_span - 1) / page - big_span / page + 1) * page;
        CHECK(s.reads.load() == foot);
        uintptr_t base = reinterpret_cast<uintptr_t>(b);
        auto h = map::find(base + align_up(big_span, uint64_t(huge)));
        CHECK(bool(h));
        CHECK(h.level() == 1);
        auto l = map::find(base + align_down(big_span, uint64_t(page)));
        CHECK(bool(l));
        CHECK(l.level() == 0);
        CHECK(matches(b, big_span & ~uint64_t(7), big_span));
        pc::unmap(b);
    }
}

void capability_pressure()
{
    function("capability: memory back under pressure");

    test("a region without a limit gives memory back when frames asks");
    {
        pattern_store s(1ull << 30);
        auto *b = static_cast<char *>(pc::map(s));
        size_t before = fr::free_bytes();
        CHECK(matches(b, 0, 64ul << 20));
        CHECK(before - fr::free_bytes() >= (64ul << 20));
        unsigned resident = 0;
        for (uint64_t off = 0; off < (64ul << 20); off += page) {
            resident += pc::resident(b + off);
        }
        CHECK(resident == 64 * 256);

        // The cache is asked before the heap, and one call gives one pass.
        size_t low = fr::free_bytes();
        CHECK(fr::reclaim(page));
        CHECK(fr::free_bytes() > low);
        unsigned left = 0;
        for (uint64_t off = 0; off < (64ul << 20); off += page) {
            left += pc::resident(b + off);
        }
        CHECK(left < resident);
        printf("\t  %u of %u pages left after one pass\n", left, resident);
        CHECK(matches(b, 0, 64ul << 20));
        pc::unmap(b);
    }
}

void capability_stats()
{
    function("capability: counters compiled in on demand");

    test("stats_dump and stats_reset");
    {
#if CONF_pagecache_stats
        pattern_store s(16 * huge);
        auto *b = static_cast<char *>(pc::map(s));
        pc::stats_reset();
        CHECK(matches(b, 0, 16 * page));
        pc::stats_dump();
        pc::unmap(b);
#else
        printf("\t  not compiled in (conf_pagecache_stats=0)\n");
#endif
    }
}

void pagecache_concurrency()
{
    function("pagecache: many cpus at once");

    test("faults on one buffer from every cpu read it once and all see it");
    {
        pattern_store s(16 * huge);
        auto *b = static_cast<char *>(pc::map(s, policy_with<fault_fixed<65536>>()));
        std::atomic<unsigned> good{0};
        unsigned threads = n_cpus();
        parallel(threads, [&](unsigned id) {
            uint64_t o = (id * 1000) & ~uint64_t(7);
            good += peek64(b + o) == pattern_store::word(o);
        });
        CHECK(good.load() == threads);
        CHECK(s.reads.load() == 65536);
        pc::unmap(b);
    }

    test("ragged buffers survive eviction, from many threads");
    {
        pattern_store s(1ull << 30);
        auto *b = static_cast<char *>(pc::map(s, policy_with<fault_fixed<ragged_span>>(), 32 << 20));
        std::atomic<bool> ok{true};
        std::atomic<uint64_t> bad_off{0}, bad_val{0};
        parallel(n_cpus(), [&](unsigned t) {
            uint64_t seed = 0x9e3779b9u * (t + 1);
            for (int i = 0; i < 20000; i++) {
                seed = seed * 6364136223846793005ull + 1;
                uint64_t off = (seed >> 16) % ((1ull << 30) - 8) & ~uint64_t(7);
                uint64_t v = peek64(b + off);
                if (v != pattern_store::word(off) && ok.exchange(false)) {
                    bad_off = off;
                    bad_val = v;
                }
            }
        });
        if (!ok) {
            printf("\t  at %#lx read %#lx\n", bad_off.load(), bad_val.load());
        }
        CHECK(ok.load());
        pc::unmap(b);
    }

    test("the same under writes, every eviction writing back");
    {
        pattern_store s(1ull << 30);
        s.verify = true;
        auto *b = static_cast<char *>(pc::map(s, policy_with<fault_fixed<ragged_span>>(), 32 << 20));
        std::atomic<bool> ok{true};
        parallel(n_cpus(), [&](unsigned t) {
            uint64_t seed = 0x85ebca6bu * (t + 1);
            for (int i = 0; i < 20000; i++) {
                seed = seed * 6364136223846793005ull + 1;
                uint64_t off = (seed >> 16) % ((1ull << 30) - 8) & ~uint64_t(7);
                auto *at = reinterpret_cast<volatile uint64_t *>(b + off);
                if (*at != pattern_store::word(off)) {
                    ok = false;
                }
                *at = pattern_store::word(off);
            }
        });
        if (s.wrong.load()) {
            printf("\t  write-back handed over %zu wrong words, first at %#lx\n",
                   s.wrong.load(), s.wrong_off.load());
        }
        CHECK(ok.load());
        CHECK(s.wrong.load() == 0);
        CHECK(s.writes.load() > 0);
        pc::unmap(b);
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
    heap_pressure();

    group("page cache");
    store_interface();
    pagecache_map();
    pagecache_unmap();
    pagecache_sync();
    pagecache_resident();
    pagecache_fetch();
    pagecache_access();
    capability_buffer_size();
    capability_limit_and_policy();
    capability_prefetch();
    capability_huge_frames();
    capability_pressure();
    capability_stats();
    pagecache_concurrency();

    group("libc");
    libc_malloc();
    libc_mmap();

    return summary("MEMORY");
}
