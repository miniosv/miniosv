/*
 * The memory primitives: vspace, frames and mapping, plus map_phys, map_phys_at
 * and vm_fault. One header per function of the public interface, with the
 * tests of that function listed under it, in the order of docs/memory-primitives.md.
 * What the heap and the page cache make of these is in os-memory.cc.
 *
 * The test application is linked into the kernel, which is what lets it call
 * them. The boot-only functions (frames::add_region, init, enable_percpu,
 * boot_alloc) have run by the time anything here does and are not covered.
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/fault.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/phys.hh>
#include <osv/mem/vspace.hh>

#include "mem-test.hh"

using namespace memtest;

namespace {

namespace vs = mem::vspace;
namespace fr = mem::frames;
namespace map = mem::mapping;
using resa = vs::resa_result;

const size_t page = fr::page_size;
const size_t huge = map::huge_page_size;

// Physical memory the allocator handed out, through the documented route.
char *view(fr::phys_addr pa, size_t bytes = page)
{
    return static_cast<char *>(mem::map_phys(pa, bytes));
}

// A read the compiler cannot hoist, for what a TLB may still hold.
char peek(uintptr_t addr)
{
    return *reinterpret_cast<volatile char *>(addr);
}

// A reservation to write translations into. Nothing else hands these addresses
// out, and no test leaves one attached, so no fault ever lands in one.
struct scratch {
    vs::region r;
    explicit scratch(size_t bytes, size_t align = page)
    {
        CHECK(vs::reserve(r, bytes, align) == resa::success);
    }
    ~scratch() { vs::release(r); }
    uintptr_t start() const { return r.span.start; }
    char *ptr(size_t off = 0) const { return reinterpret_cast<char *>(r.span.start + off); }
    mem::range range(size_t off, size_t len) const
    {
        return {r.span.start + off, r.span.start + off + len};
    }
};

// Frames taken to push the allocator somewhere: 2 MiB blocks while they last,
// then pages. Everything goes back when it is destroyed.
struct hoard {
    std::vector<fr::phys_addr> blocks, pages;
    size_t bytes = 0;

    hoard()
    {
        blocks.reserve(fr::total_available_bytes() / huge + 1);
        pages.reserve(std::min<size_t>(fr::total_available_bytes() / page, 1u << 20));
    }
    ~hoard()
    {
        for (auto p : pages) {
            fr::free(p);
        }
        for (auto b : blocks) {
            fr::free(b, huge);
        }
    }
    // Takes until stop() says so. False if the allocator emptied first.
    template <typename F>
    bool take_until(F stop)
    {
        while (!stop()) {
            auto b = fr::alloc(huge, huge);
            if (b != fr::no_memory) {
                blocks.push_back(b);
                bytes += huge;
                continue;
            }
            auto p = fr::alloc();
            if (p == fr::no_memory || pages.size() == pages.capacity()) {
                if (p != fr::no_memory) {
                    fr::free(p);
                }
                return false;
            }
            pages.push_back(p);
            bytes += page;
        }
        return true;
    }
};

/* vspace ------------------------------------------------------------------ */

// A registered region may not move, so the churn tests keep theirs in vectors
// sized up front.
uint32_t lcg(uint32_t &seed)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

void vspace_app_window()
{
    function("vspace::app_window");

    test("the window is the documented range, page aligned and not empty");
    {
        mem::range w = vs::app_window();
        CHECK(w.start == 0x200000000000ul);
        CHECK(w.end == 0x400000000000ul);
        CHECK(w.start % page == 0);
        CHECK(w.end % page == 0);
        CHECK(!w.empty());
    }

    test("every reservation lands inside it");
    {
        vs::region r{};
        for (size_t bytes : {page, huge, 1ul << 30}) {
            CHECK(vs::reserve(r, bytes, page) == resa::success);
            CHECK(vs::app_window().contains(r.span));
            vs::release(r);
        }
    }
}

void vspace_reserve()
{
    function("vspace::reserve");

    test("the span is as long as asked and aligned as asked");
    {
        for (size_t align : {page, huge, 1ul << 30}) {
            vs::region r{};
            CHECK(vs::reserve(r, huge, align) == resa::success);
            CHECK((r.span.start & (align - 1)) == 0);
            CHECK(r.span.size() == huge);
            vs::release(r);
        }
    }

    test("bytes and align are rounded up to a page");
    {
        vs::region r{};
        CHECK(vs::reserve(r, 1, 1) == resa::success);
        CHECK(r.span.size() == page);
        CHECK(r.span.start % page == 0);
        vs::release(r);
        CHECK(vs::reserve(r, page + 1, 64) == resa::success);
        CHECK(r.span.size() == 2 * page);
        vs::release(r);
    }

    test("zero bytes is no_space and the region is left alone");
    {
        vs::region r{};
        r.span = {7, 7};
        CHECK(vs::reserve(r, 0, page) == resa::no_space);
        CHECK(r.span.start == 7);
        CHECK(r.span.end == 7);
    }

    test("more than the window holds is no_space");
    {
        vs::region r{};
        CHECK(vs::reserve(r, vs::app_window().size() + page, page) == resa::no_space);
    }

    test("only span is written: perm and ops are the caller's");
    {
        static const vs::region_ops ops = {nullptr};
        vs::region r{};
        r.perm = 0x77;
        r.ops = &ops;
        CHECK(vs::reserve(r, page, page) == resa::success);
        CHECK(r.perm == 0x77);
        CHECK(r.ops == &ops);
        vs::release(r);
    }

    test("reserving costs no physical memory");
    {
        vs::region r{};
        size_t before = fr::free_bytes();
        CHECK(vs::reserve(r, 64ul << 20, page) == resa::success);
        CHECK(before - fr::free_bytes() < (1ul << 20));
        vs::release(r);
    }

    test("reservations do not overlap");
    {
        const int n = 64;
        std::vector<vs::region> r(n);
        for (int i = 0; i < n; i++) {
            CHECK(vs::reserve(r[i], (i % 4 + 1) * page, page) == resa::success);
        }
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                CHECK(!r[i].span.intersects(r[j].span));
            }
        }
        for (int i = 0; i < n; i++) {
            vs::release(r[i]);
        }
    }

    test("reservations from several threads do not overlap");
    {
        const int per_thread = 16;
        unsigned threads = n_cpus();
        std::vector<vs::region> r(threads * per_thread);
        parallel(threads, [&](unsigned id) {
            for (int i = 0; i < per_thread; i++) {
                vs::reserve(r[id * per_thread + i], 1ul << 20, page);
            }
        });
        std::vector<mem::range> spans;
        for (auto &e : r) {
            CHECK(!e.span.empty());
            spans.push_back(e.span);
        }
        std::sort(spans.begin(), spans.end(),
                  [](const mem::range &a, const mem::range &b) { return a.start < b.start; });
        for (size_t i = 1; i < spans.size(); i++) {
            CHECK(spans[i - 1].end <= spans[i].start);
        }
        for (auto &e : r) {
            vs::release(e);
        }
    }

    test("10000 live reservations are all found");
    {
        const int n = 10000;
        std::vector<vs::region> r(n);
        int got = 0;
        for (int i = 0; i < n; i++) {
            got += vs::reserve(r[i], page, page) == resa::success;
        }
        CHECK(got == n);
        CHECK(vs::self_check());
        bool found = true;
        for (int i = 0; i < n; i += 97) {
            found = found && vs::lookup(r[i].span.start) == &r[i];
        }
        CHECK(found);
        for (int i = 0; i < n; i++) {
            vs::release(r[i]);
        }
        CHECK(vs::self_check());
    }
}

void vspace_reserve_at()
{
    function("vspace::reserve_at");

    test("an exact range is reserved at that address");
    {
        vs::region a{}, b{};
        CHECK(vs::reserve(a, huge, huge) == resa::success);
        mem::range span = a.span;
        vs::release(a);
        CHECK(vs::reserve_at(b, span) == resa::success);
        CHECK(b.span.start == span.start);
        CHECK(b.span.end == span.end);
        CHECK(vs::lookup(span.start) == &b);
        vs::release(b);
    }

    test("the range is rounded outward to whole pages");
    {
        vs::region a{}, b{};
        CHECK(vs::reserve(a, 4 * page, page) == resa::success);
        mem::range span = a.span;
        vs::release(a);
        CHECK(vs::reserve_at(b, {span.start + 100, span.end - 100}) == resa::success);
        CHECK(b.span.start == span.start);
        CHECK(b.span.end == span.end);
        vs::release(b);
    }

    test("an empty range is no_space");
    {
        vs::region a{}, b{};
        CHECK(vs::reserve(a, page, page) == resa::success);
        uintptr_t at = a.span.start;
        vs::release(a);
        CHECK(vs::reserve_at(b, {at, at}) == resa::no_space);
        CHECK(vs::reserve_at(b, {at + page, at}) == resa::no_space);
        CHECK(vs::lookup(at) == nullptr);
    }

    test("a range already taken, wholly or in part, is already_reserved");
    {
        vs::region a{}, b{};
        CHECK(vs::reserve(a, huge, huge) == resa::success);
        CHECK(vs::reserve_at(b, a.span) == resa::already_reserved);
        CHECK(vs::reserve_at(b, {a.span.start + huge / 2, a.span.end + huge}) ==
              resa::already_reserved);
        CHECK(vs::reserve_at(b, {a.span.start - page, a.span.start + page}) ==
              resa::already_reserved);
        CHECK(vs::lookup(a.span.start) == &a);
        vs::release(a);
    }

    test("two exact ranges side by side are both reserved");
    {
        vs::region a{}, lo{}, hi{};
        CHECK(vs::reserve(a, 2 * page, page) == resa::success);
        mem::range span = a.span;
        vs::release(a);
        CHECK(vs::reserve_at(lo, {span.start, span.start + page}) == resa::success);
        CHECK(vs::reserve_at(hi, {span.start + page, span.end}) == resa::success);
        CHECK(vs::lookup(span.start) == &lo);
        CHECK(vs::lookup(span.start + page) == &hi);
        vs::release(lo);
        vs::release(hi);
    }
}

void vspace_release()
{
    function("vspace::release");

    test("a released range is free again");
    {
        vs::region a{}, b{};
        CHECK(vs::reserve(a, huge, page) == resa::success);
        mem::range span = a.span;
        vs::release(a);
        CHECK(!vs::reserved(span));
        CHECK(vs::lookup(span.start) == nullptr);
        CHECK(vs::reserve_at(b, span) == resa::success);
        vs::release(b);
    }

    test("releasing one region leaves its neighbours");
    {
        std::vector<vs::region> r(3);
        for (auto &e : r) {
            CHECK(vs::reserve(e, page, page) == resa::success);
        }
        vs::release(r[1]);
        CHECK(vs::lookup(r[0].span.start) == &r[0]);
        CHECK(vs::lookup(r[2].span.start) == &r[2]);
        CHECK(vs::lookup(r[1].span.start) == nullptr);
        vs::release(r[0]);
        vs::release(r[2]);
    }

    test("the index survives a random reserve/release sequence");
    {
        const int n = 512;
        std::vector<vs::region> r(n);
        std::vector<bool> live(n, false);
        uint32_t seed = 20260819;
        for (int i = 0; i < 4000; i++) {
            int at = lcg(seed) % n;
            if (live[at]) {
                vs::release(r[at]);
                live[at] = false;
            } else {
                size_t size = (lcg(seed) % 16 + 1) * page;
                live[at] = vs::reserve(r[at], size, page) == resa::success;
            }
        }
        CHECK(vs::self_check());
        for (int i = 0; i < n; i++) {
            if (live[i]) {
                vs::release(r[i]);
            }
        }
        CHECK(vs::self_check());
    }
}

void vspace_lookup()
{
    function("vspace::lookup");

    test("the region holding an address, edges included, and nothing outside");
    {
        vs::region r{};
        CHECK(vs::reserve(r, huge, page) == resa::success);
        uintptr_t a = r.span.start;
        CHECK(vs::lookup(a) == &r);
        CHECK(vs::lookup(a + huge - 1) == &r);
        CHECK(vs::lookup(a + huge) != &r);
        CHECK(vs::lookup(a - 1) != &r);
        vs::release(r);
        CHECK(vs::lookup(a) == nullptr);
    }

    test("every page of a region resolves to it, and the pointer is the caller's region");
    {
        vs::region r{};
        CHECK(vs::reserve(r, huge, page) == resa::success);
        bool inside = true;
        for (size_t off = 0; off < huge; off += page) {
            inside = inside && vs::lookup(r.span.start + off) == &r;
        }
        CHECK(inside);
        r.perm = mem::perm_read;
        CHECK(vs::lookup(r.span.start)->perm == mem::perm_read);
        vs::release(r);
    }

    test("lookups stay correct while the index is rebuilt underneath them");
    {
        const int stable_n = 256;
        const int churn_n = 256;
        std::vector<vs::region> stable(stable_n);
        std::vector<vs::region> churn(churn_n);
        for (int i = 0; i < stable_n; i++) {
            CHECK(vs::reserve(stable[i], page, page) == resa::success);
        }

        std::atomic<bool> stop{false};
        std::atomic<long> wrong{0};
        std::atomic<long> missing{0};
        std::atomic<long> done{0};
        unsigned threads = std::max(2u, std::min(n_cpus(), 16u));

        parallel(threads, [&](unsigned id) {
            if (id == 0) {
                std::vector<bool> live(churn_n, false);
                uint32_t seed = 99194853;
                for (int round = 0; round < 20000; round++) {
                    int at = lcg(seed) % churn_n;
                    if (live[at]) {
                        vs::release(churn[at]);
                        live[at] = false;
                    } else {
                        live[at] = vs::reserve(churn[at], page, page) == resa::success;
                    }
                }
                for (int i = 0; i < churn_n; i++) {
                    if (live[i]) {
                        vs::release(churn[i]);
                    }
                }
                stop.store(true);
                return;
            }
            uint32_t seed = id * 2654435761u + 1;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 64; k++) {
                    int i = lcg(seed) % stable_n;
                    auto *found = vs::lookup(stable[i].span.start);
                    if (!found) {
                        missing.fetch_add(1, std::memory_order_relaxed);
                    } else if (found != &stable[i]) {
                        wrong.fetch_add(1, std::memory_order_relaxed);
                    }
                    done.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        CHECK(done.load() > 0);
        CHECK(missing.load() == 0);
        CHECK(wrong.load() == 0);
        CHECK(vs::self_check());
        printf("\t  %ld lookups against 20000 reserve/release rounds\n", done.load());
        for (int i = 0; i < stable_n; i++) {
            vs::release(stable[i]);
        }
    }
}

void vspace_reserved()
{
    function("vspace::reserved");

    test("a region, and any part of it, is reserved");
    {
        vs::region r{};
        CHECK(vs::reserve(r, 4 * page, page) == resa::success);
        uintptr_t a = r.span.start;
        CHECK(vs::reserved(r.span));
        CHECK(vs::reserved({a + page, a + 2 * page}));
        CHECK(vs::reserved({a + 1, a + 2}));
        vs::release(r);
        CHECK(!vs::reserved({a, a + page}));
    }

    test("a range reaching past a region is not");
    {
        // The middle page of three, so that both sides are known to be free.
        vs::region wide{}, r{};
        CHECK(vs::reserve(wide, 3 * page, page) == resa::success);
        uintptr_t a = wide.span.start + page;
        vs::release(wide);
        CHECK(vs::reserve_at(r, {a, a + page}) == resa::success);
        CHECK(!vs::reserved({a - page, a + page}));
        CHECK(!vs::reserved({a, a + 2 * page}));
        CHECK(!vs::reserved({a - page, a + 2 * page}));
        CHECK(vs::reserved({a, a + page}));
        vs::release(r);
    }

    test("a range covered by several regions is, and not once one of them goes");
    {
        vs::region a{}, lo{}, hi{};
        CHECK(vs::reserve(a, 2 * page, page) == resa::success);
        mem::range span = a.span;
        vs::release(a);
        CHECK(vs::reserve_at(lo, {span.start, span.start + page}) == resa::success);
        CHECK(vs::reserve_at(hi, {span.start + page, span.end}) == resa::success);
        CHECK(vs::reserved(span));
        vs::release(hi);
        CHECK(!vs::reserved(span));
        CHECK(vs::reserved({span.start, span.start + page}));
        vs::release(lo);
    }
}

void vspace_accounting()
{
    function("vspace::reserved_bytes, vspace::count");

    test("both rise with a reservation and fall with its release");
    {
        const size_t size = 1ul << 20;
        const int n = 8;
        std::vector<vs::region> r(n);
        size_t regions_before = vs::count();
        size_t bytes_before = vs::reserved_bytes();
        for (int i = 0; i < n; i++) {
            CHECK(vs::reserve(r[i], size, page) == resa::success);
            CHECK(vs::count() == regions_before + i + 1);
            CHECK(vs::reserved_bytes() == bytes_before + (i + 1) * size);
        }
        for (int i = 0; i < n; i++) {
            vs::release(r[i]);
        }
        CHECK(vs::count() == regions_before);
        CHECK(vs::reserved_bytes() == bytes_before);
    }

    test("the rounded size is what is counted");
    {
        vs::region r{};
        size_t before = vs::reserved_bytes();
        CHECK(vs::reserve(r, 1, 1) == resa::success);
        CHECK(vs::reserved_bytes() == before + page);
        vs::release(r);
    }
}

void vspace_for_each()
{
    function("vspace::for_each");

    test("every live region is visited once, as the caller's object, with the argument");
    {
        struct visit {
            std::set<const vs::region *> seen;
            size_t calls = 0;
            size_t bytes = 0;
        };
        const int n = 8;
        std::vector<vs::region> r(n);
        for (int i = 0; i < n; i++) {
            CHECK(vs::reserve(r[i], (i + 1) * page, page) == resa::success);
        }
        visit v;
        vs::for_each([](const vs::region &e, void *arg) {
            auto *s = static_cast<visit *>(arg);
            s->calls++;
            s->seen.insert(&e);
            s->bytes += e.span.size();
        }, &v);
        CHECK(v.calls == vs::count());
        CHECK(v.seen.size() == v.calls);
        CHECK(v.bytes == vs::reserved_bytes());
        bool ours = true;
        for (int i = 0; i < n; i++) {
            ours = ours && v.seen.count(&r[i]);
        }
        CHECK(ours);
        for (int i = 0; i < n; i++) {
            vs::release(r[i]);
        }
    }
}

void vspace_self_check()
{
    function("vspace::self_check");

    test("the index is consistent now, and after a burst of reservations");
    {
        CHECK(vs::self_check());
        const int n = 1000;
        std::vector<vs::region> r(n);
        for (int i = 0; i < n; i++) {
            CHECK(vs::reserve(r[i], page, page) == resa::success);
        }
        CHECK(vs::self_check());
        for (int i = 0; i < n; i += 2) {
            vs::release(r[i]);
        }
        CHECK(vs::self_check());
        for (int i = 1; i < n; i += 2) {
            vs::release(r[i]);
        }
        CHECK(vs::self_check());
    }
}

void vspace_perf()
{
    group("vspace - performance");

    section("reserve + release");
    {
        struct { const char *name; size_t size; int n; } cases[] = {
            {"4 KiB",  4ul << 10, 20000},
            {"2 MiB",  2ul << 20, 20000},
            {"64 MiB", 64ul << 20, 2000},
        };
        for (auto &c : cases) {
            vs::region r{};
            auto t0 = clk::now();
            for (int i = 0; i < c.n; i++) {
                vs::reserve(r, c.size, page);
                vs::release(r);
            }
            char label[64];
            snprintf(label, sizeof(label), "reserve + release %s", c.name);
            report_ns(label, since(t0), c.n);
        }
    }

    section("reserve + release, scaling");
    {
        const int total = 40000;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(total, t, 64);
            double s = parallel(t, [&](unsigned) {
                vs::region r{};
                for (int i = 0; i < n; i++) {
                    vs::reserve(r, 2ul << 20, page);
                    vs::release(r);
                }
            });
            report_scale("reserve + release 2 MiB", t, static_cast<double>(n) * t, s);
        }
    }

    section("lookup");
    {
        for (int live : {1, 100, 1000, 10000}) {
            std::vector<vs::region> r(live);
            for (int i = 0; i < live; i++) {
                vs::reserve(r[i], page, page);
            }
            const int probes = 200000;
            uintptr_t target = r[live / 2].span.start;
            auto t0 = clk::now();
            for (int i = 0; i < probes; i++) {
                escape(vs::lookup(target));
            }
            char label[64];
            snprintf(label, sizeof(label), "lookup with %d live regions", live);
            report_ns(label, since(t0), probes);
            for (int i = 0; i < live; i++) {
                vs::release(r[i]);
            }
        }
    }

    section("lookup, scaling");
    {
        const int live = 1000;
        std::vector<vs::region> r(live);
        for (int i = 0; i < live; i++) {
            vs::reserve(r[i], page, page);
        }
        const int probes = 200000;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            double s = parallel(t, [&](unsigned id) {
                uintptr_t target = r[(id * 37) % live].span.start;
                for (int i = 0; i < probes; i++) {
                    escape(vs::lookup(target));
                }
            });
            report_scale("lookup, 1000 live regions", t,
                         static_cast<double>(probes) * t, s);
        }
        for (int i = 0; i < live; i++) {
            vs::release(r[i]);
        }
    }
}

/* frames ------------------------------------------------------------------ */

// Two pressure watchers of the test's own, either side of the cache and the
// heap. The list keeps them forever, so they are armed only while a test
// looks, and can hold blocks to give back when asked.
std::atomic<int> tick{0};

struct spy {
    fr::pressure_watcher w;
    std::atomic<bool> armed{false};
    std::atomic<int> calls{0}, last{0};
    std::atomic<bool> inner{true};      // what reclaim() answered from inside
    std::vector<fr::phys_addr> blocks;

    void reset()
    {
        calls = 0;
        last = 0;
        inner = true;
    }
    bool asked()
    {
        if (!armed.load()) {
            return false;
        }
        calls.fetch_add(1);
        last.store(tick.fetch_add(1) + 1);
        inner.store(fr::reclaim(page));
        bool gave = !blocks.empty();
        for (auto b : blocks) {
            fr::free(b, huge);
        }
        blocks.clear();
        return gave;
    }
};

spy low, high;
bool low_asked() { return low.asked(); }
bool high_asked() { return high.asked(); }

void register_spies()
{
    static bool done = false;
    if (!done) {
        done = true;
        fr::watch_pressure(low.w, low_asked, 5);
        fr::watch_pressure(high.w, high_asked, 99);
    }
}

// The pool a watcher hands back when asked.
void stock(spy &s, unsigned blocks)
{
    for (unsigned i = 0; i < blocks; i++) {
        auto b = fr::alloc(huge, huge);
        CHECK(b != fr::no_memory);
        s.blocks.push_back(b);
    }
}

void frames_alloc()
{
    function("frames::alloc");

    test("frames are page aligned, distinct and writable");
    {
        const int n = 64;
        fr::phys_addr p[n];
        for (int i = 0; i < n; i++) {
            p[i] = fr::alloc();
            CHECK(p[i] != fr::no_memory);
            CHECK(p[i] % page == 0);
            memset(view(p[i]), 0xa5, page);
        }
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                CHECK(p[i] != p[j]);
            }
        }
        for (int i = 0; i < n; i++) {
            CHECK(static_cast<unsigned char>(view(p[i])[page - 1]) == 0xa5);
            fr::free(p[i]);
        }
    }

    test("align below a page is a page, and above it is honoured");
    {
        struct { size_t bytes, align; } cases[] = {
            {page, 1}, {page, huge}, {page, 4 * huge}, {huge, huge}, {3 * huge, 4 * huge},
        };
        for (auto &c : cases) {
            auto p = fr::alloc(c.bytes, c.align);
            CHECK(p != fr::no_memory);
            CHECK(p % std::max(c.align, page) == 0);
            memset(view(p, c.bytes), 0x5a, c.bytes);
            fr::free(p, c.bytes);
        }
    }

    test("zero bytes is no_memory");
    {
        CHECK(fr::alloc(0) == fr::no_memory);
        CHECK(fr::alloc(0, huge) == fr::no_memory);
    }

    test("bytes is rounded to whole frames");
    {
        size_t before = fr::free_bytes();
        auto p = fr::alloc(1);
        CHECK(p != fr::no_memory);
        CHECK(before - fr::free_bytes() == page);
        memset(view(p), 0x11, page);
        fr::free(p, 1);
        CHECK(fr::free_bytes() == before);
    }

    test("up to one block the count is rounded to a power of two");
    {
        // Three frames are served as four: the accounting says so, and the
        // fourth is the caller's to use.
        size_t before = fr::free_bytes();
        auto p = fr::alloc(3 * page);
        CHECK(p != fr::no_memory);
        CHECK(before - fr::free_bytes() == 4 * page);
        memset(view(p, 4 * page), 0x22, 4 * page);
        auto q = fr::alloc(3 * page);
        CHECK(q >= p + 4 * page || q + 4 * page <= p);
        fr::free(q, 3 * page);
        fr::free(p, 3 * page);
        CHECK(fr::free_bytes() == before);

        before = fr::free_bytes();
        p = fr::alloc(huge - page);
        CHECK(before - fr::free_bytes() == huge);
        fr::free(p, huge - page);
    }

    test("above one block the count is exact");
    {
        size_t before = fr::free_bytes();
        auto p = fr::alloc(513 * page);
        CHECK(p != fr::no_memory);
        CHECK(before - fr::free_bytes() == 513 * page);
        memset(view(p, 513 * page), 0x33, 513 * page);
        fr::free(p, 513 * page);
        CHECK(fr::free_bytes() == before);
    }

    test("a contiguous run is the caller's from end to end");
    {
        // Nothing handed out afterwards falls inside it, and what was written
        // is still there.
        const size_t size = 8ul << 20;
        auto p = fr::alloc(size, page);
        CHECK(p != fr::no_memory);
        char *v = view(p, size);
        memset(v, 0x44, size);
        std::vector<fr::phys_addr> others(4096);
        bool inside = false;
        for (auto &o : others) {
            o = fr::alloc();
            inside = inside || (o >= p && o < p + size);
        }
        CHECK(!inside);
        for (auto o : others) {
            fr::free(o);
        }
        bool kept = true;
        for (size_t off = 0; off < size; off += page) {
            kept = kept && static_cast<unsigned char>(v[off]) == 0x44;
        }
        CHECK(kept);
        fr::free(p, size);
    }

    test("nothing free after reclaim is no_memory");
    {
        // Taken down to nothing, the allocator says so rather than failing
        // some other way, and gives everything back in one piece.
        register_spies();
        size_t before = fr::free_bytes();
        {
            hoard h;
            bool emptied = !h.take_until([] { return false; });
            if (emptied) {
                CHECK(fr::alloc() == fr::no_memory);
                CHECK(fr::alloc(huge, huge) == fr::no_memory);
            } else {
                printf("\t  not emptied: the page list filled first\n");
            }
            printf("\t  %zu MiB taken before the allocator was empty\n", h.bytes >> 20);
        }
        // The hoard's own lists cost the heap a page or two.
        CHECK(fr::free_bytes() + (4ul << 20) >= before);
    }
}

void frames_free()
{
    function("frames::free");

    test("a freed frame is counted free again and handed out again");
    {
        size_t before = fr::free_bytes();
        auto p = fr::alloc();
        CHECK(fr::free_bytes() == before - page);
        fr::free(p);
        CHECK(fr::free_bytes() == before);
        // Out and back 4096 times: an allocator that never reused a frame
        // would touch 4096 of them.
        std::set<fr::phys_addr> seen;
        for (int i = 0; i < 4096; i++) {
            auto q = fr::alloc();
            seen.insert(q);
            fr::free(q);
        }
        CHECK(seen.size() < 4096);
    }

    test("free takes the size alloc was given");
    {
        size_t before = fr::free_bytes();
        auto p = fr::alloc(3 * page);
        auto q = fr::alloc(513 * page);
        CHECK(before - fr::free_bytes() == 517 * page);
        fr::free(p, 3 * page);
        CHECK(before - fr::free_bytes() == 513 * page);
        fr::free(q, 513 * page);
        CHECK(fr::free_bytes() == before);
    }

    test("no_memory and zero bytes are ignored");
    {
        size_t before = fr::free_bytes();
        fr::free(fr::no_memory);
        auto p = fr::alloc();
        fr::free(p, 0);
        CHECK(fr::free_bytes() == before - page);
        fr::free(p);
        CHECK(fr::free_bytes() == before);
    }

    test("4096 frames out and back leave the count where it was");
    {
        size_t before = fr::free_bytes();
        const int n = 4096;
        std::vector<fr::phys_addr> p(n);
        for (int i = 0; i < n; i++) {
            p[i] = fr::alloc();
        }
        CHECK(fr::free_bytes() == before - n * page);
        for (int i = 0; i < n; i++) {
            fr::free(p[i]);
        }
        CHECK(fr::free_bytes() == before);
    }

    test("frames freed on another cpu than they came from");
    {
        unsigned threads = std::max(2u, std::min(n_cpus(), 8u));
        const int per_thread = 512;
        std::vector<fr::phys_addr> p(threads * per_thread);
        parallel(threads, [&](unsigned id) {
            for (int i = 0; i < per_thread; i++) {
                p[id * per_thread + i] = fr::alloc();
            }
        });
        // Whether the frames really came back is checked by taking them
        // again: the thread stacks and the heap move the count meanwhile.
        parallel(threads, [&](unsigned id) {
            unsigned from = (id + 1) % threads;
            for (int i = 0; i < per_thread; i++) {
                fr::free(p[from * per_thread + i]);
            }
        });
        std::set<fr::phys_addr> seen;
        for (unsigned i = 0; i < threads * per_thread; i++) {
            auto q = fr::alloc();
            CHECK(q != fr::no_memory);
            seen.insert(q);
            p[i] = q;
        }
        CHECK(seen.size() == threads * per_thread);
        for (unsigned i = 0; i < threads * per_thread; i++) {
            fr::free(p[i]);
        }
    }
}

void frames_accounting()
{
    function("frames::free_bytes, frames::total_available_bytes, frames::phys_mem_size");

    test("free is at most what the allocator holds, which is less than the RAM reported");
    {
        CHECK(fr::free_bytes() > 0);
        CHECK(fr::free_bytes() <= fr::total_available_bytes());
        CHECK(fr::total_available_bytes() < fr::phys_mem_size);
        printf("\t  reported %zu MiB, allocator %zu MiB, free %zu MiB\n",
               fr::phys_mem_size >> 20, fr::total_available_bytes() >> 20,
               fr::free_bytes() >> 20);
    }

    test("free follows allocations, the total does not");
    {
        size_t total = fr::total_available_bytes();
        size_t before = fr::free_bytes();
        auto p = fr::alloc(huge, huge);
        CHECK(fr::free_bytes() == before - huge);
        CHECK(fr::total_available_bytes() == total);
        fr::free(p, huge);
        CHECK(fr::free_bytes() == before);
        CHECK(fr::total_available_bytes() == total);
    }

    test("ready() is true once the allocator exists");
    {
        CHECK(fr::ready());
    }
}

void frames_watch_pressure()
{
    function("frames::watch_pressure");

    test("a registered watcher is asked when memory is reclaimed");
    {
        register_spies();
        low.reset();
        low.armed = true;
        fr::reclaim(fr::total_available_bytes());
        low.armed = false;
        CHECK(low.calls.load() == 1);
    }

    test("watchers are asked lowest order first");
    {
        register_spies();
        low.reset();
        high.reset();
        low.armed = high.armed = true;
        fr::reclaim(fr::total_available_bytes());
        low.armed = high.armed = false;
        CHECK(low.calls.load() == 1);
        CHECK(high.calls.load() == 1);
        CHECK(low.last.load() < high.last.load());
    }
}

void frames_under_pressure()
{
    function("frames::under_pressure");

    test("false with memory to spare, true below the threshold, false again after");
    {
        register_spies();
        // Start from a quiet allocator: anything the clients hold is asked back.
        while (fr::reclaim(fr::total_available_bytes())) {
        }
        CHECK(!fr::under_pressure());
        {
            hoard h;
            bool reached = h.take_until([] { return fr::under_pressure(); });
            CHECK(reached);
            CHECK(fr::under_pressure());
            printf("\t  under pressure after taking %zu of %zu MiB\n",
                   h.bytes >> 20, fr::total_available_bytes() >> 20);
        }
        CHECK(!fr::under_pressure());
    }
}

void frames_check_pressure()
{
    function("frames::check_pressure");

    test("above the threshold nobody is asked");
    {
        register_spies();
        while (fr::reclaim(fr::total_available_bytes())) {
        }
        low.reset();
        low.armed = true;
        fr::check_pressure();
        low.armed = false;
        CHECK(!fr::under_pressure());
        CHECK(low.calls.load() == 0);
    }

    test("below it the watchers are asked until the shortfall is covered");
    {
        register_spies();
        hoard h;
        // The heap holds nothing it could give, so what is asked is visible.
        while (fr::reclaim(fr::total_available_bytes())) {
        }
        CHECK(h.take_until([] { return fr::under_pressure(); }));
        low.reset();
        high.reset();
        low.armed = high.armed = true;
        fr::check_pressure();
        low.armed = high.armed = false;
        CHECK(low.calls.load() == 1);
        // Nothing gave anything, so every order was asked.
        CHECK(high.calls.load() == 1);

        // Enough in the lowest order to cover it, and nobody after it is asked.
        low.reset();
        high.reset();
        size_t shortfall = fr::total_available_bytes() / 10;
        for (size_t i = 0; i < shortfall / huge + 8 && !h.blocks.empty(); i++) {
            low.blocks.push_back(h.blocks.back());
            h.blocks.pop_back();
            h.bytes -= huge;
        }
        low.armed = high.armed = true;
        fr::check_pressure();
        low.armed = high.armed = false;
        CHECK(low.calls.load() == 1);
        CHECK(high.calls.load() == 0);
        CHECK(!fr::under_pressure());
    }
}

void frames_reclaim()
{
    function("frames::reclaim");

    test("stops once free memory has grown by the request");
    {
        register_spies();
        low.reset();
        high.reset();
        stock(low, 8);
        low.armed = high.armed = true;
        size_t before = fr::free_bytes();
        bool gave = fr::reclaim(4 * huge);
        low.armed = high.armed = false;
        CHECK(gave);
        CHECK(fr::free_bytes() >= before + 4 * huge);
        CHECK(low.calls.load() == 1);
        CHECK(high.calls.load() == 0);
    }

    test("asks every watcher once when the request is not covered");
    {
        register_spies();
        low.reset();
        high.reset();
        low.armed = high.armed = true;
        fr::reclaim(fr::total_available_bytes());
        low.armed = high.armed = false;
        CHECK(low.calls.load() == 1);
        CHECK(high.calls.load() == 1);
    }

    test("a request of nothing asks nobody and reports nothing given");
    {
        register_spies();
        low.reset();
        low.armed = true;
        CHECK(!fr::reclaim(0));
        low.armed = false;
        CHECK(low.calls.load() == 0);
    }

    test("returns whether a watcher gave something back");
    {
        register_spies();
        low.reset();
        stock(low, 1);
        low.armed = true;
        CHECK(fr::reclaim(huge));
        low.armed = false;
    }

    test("a call from inside a watcher returns false");
    {
        register_spies();
        low.reset();
        low.armed = true;
        fr::reclaim(fr::total_available_bytes());
        low.armed = false;
        CHECK(low.calls.load() == 1);
        CHECK(!low.inner.load());
    }

    test("alloc asks the watchers before giving up");
    {
        // Every block is taken, and two of them are the watcher's to give
        // back: an allocation that would fail is served from those.
        register_spies();
        hoard h;
        bool emptied = !h.take_until([] { return false; });
        low.reset();
        for (int i = 0; i < 2 && !h.blocks.empty(); i++) {
            low.blocks.push_back(h.blocks.back());
            h.blocks.pop_back();
            h.bytes -= huge;
        }
        low.armed = true;
        auto p = fr::alloc(huge, huge);
        low.armed = false;
        if (emptied) {
            CHECK(p != fr::no_memory);
            CHECK(low.calls.load() >= 1);
        } else {
            printf("\t  not emptied: the page list filled first\n");
        }
        if (p != fr::no_memory) {
            fr::free(p, huge);
        }
    }
}

void frames_perf()
{
    group("frames - performance");

    const int batch = 512;
    const int rounds = 100;

    section("4 KiB, scaling");
    {
        std::vector<fr::phys_addr> p(batch);
        for (int i = 0; i < batch; i++) {
            p[i] = fr::alloc();
        }
        for (int i = 0; i < batch; i++) {
            fr::free(p[i]);
        }

        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            double s = parallel(t, [&](unsigned) {
                std::vector<fr::phys_addr> q(batch);
                for (int r = 0; r < rounds; r++) {
                    for (int i = 0; i < batch; i++) {
                        q[i] = fr::alloc();
                    }
                    for (int i = 0; i < batch; i++) {
                        fr::free(q[i]);
                    }
                }
            });
            report_scale("frames::alloc + free", t, 2.0 * batch * rounds * t, s);
        }
    }

    section("cpu spread");
    {
        // A scaling run that lands on one cpu looks exactly like a global lock,
        // so check the threads really are spread out.
        unsigned t = std::min(n_cpus(), 8u);
        std::vector<unsigned> seen(t, ~0u);
        parallel(t, [&](unsigned id) { seen[id] = sched::cpu::current()->id; });
        unsigned distinct = 0;
        for (unsigned i = 0; i < t; i++) {
            bool first = true;
            for (unsigned j = 0; j < i; j++) {
                first = first && seen[j] != seen[i];
            }
            distinct += first;
        }
        CHECK(distinct == t);
        printf("      %u threads on %u distinct cpus\n", t, distinct);
    }

    section("2 MiB, scaling");
    {
        const int hbatch = 16;
        for (unsigned t = 1; t <= n_cpus(); t *= 2) {
            const int n = share(hbatch * rounds, t, 8);
            double s = parallel(t, [&](unsigned) {
                for (int i = 0; i < n; i++) {
                    auto h = fr::alloc(huge, huge);
                    escape(reinterpret_cast<void *>(h));
                    if (h != fr::no_memory) {
                        fr::free(h, huge);
                    }
                }
            });
            report_scale("frames::alloc(2 MiB) + free", t, 2.0 * n * t, s);
        }
    }

    section("contiguous");
    {
        const int n = 64;
        for (size_t size : {1ul << 20, 8ul << 20, 64ul << 20}) {
            auto t0 = clk::now();
            int got = 0;
            for (int i = 0; i < n; i++) {
                auto q = fr::alloc(size, page);
                if (q == fr::no_memory) {
                    break;
                }
                escape(view(q, size));
                fr::free(q, size);
                got++;
            }
            char label[64];
            snprintf(label, sizeof(label), "frames::alloc %zu MiB + map + free", size >> 20);
            if (got) {
                report_ns(label, since(t0), got);
            } else {
                printf("    %-46s %9s\n", label, "failed");
            }
        }
    }
}

/* mapping ----------------------------------------------------------------- */

// A frame holding one byte everywhere, to tell frames apart through a mapping.
fr::phys_addr marked(char c)
{
    auto p = fr::alloc();
    CHECK(p != fr::no_memory);
    memset(view(p), c, page);
    return p;
}

void mapping_find()
{
    function("mapping::find");

    test("nothing where nothing was attached");
    {
        scratch s(16 * page);
        CHECK(!map::find(s.start()));
        CHECK(!map::find(s.start() + 15 * page));
        CHECK(map::populate(s.range(page, page), mem::perm_rw));
        CHECK(!map::find(s.start()));
        CHECK(bool(map::find(s.start() + page)));
        CHECK(!map::find(s.start() + 2 * page));
        map::depopulate(s.range(page, page));
        CHECK(!map::find(s.start() + page));
    }

    test("a 4 KiB leaf: level 0, its frame and its permissions");
    {
        scratch s(page);
        auto f = fr::alloc();
        CHECK(map::attach(s.range(0, page), f, mem::perm_read));
        auto e = map::find(s.start() + 123);
        CHECK(bool(e));
        CHECK(e.level() == 0);
        CHECK(e.size() == page);
        CHECK(e.addr() == f);
        CHECK(e.perm() == mem::perm_read);
        map::detach(s.range(0, page));
        fr::free(f);
    }

    test("a 2 MiB leaf: level 1, from any address under it");
    {
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        CHECK(map::populate(s.range(0, huge), mem::perm_rw, huge));
        auto e = map::find(s.start() + huge - 1);
        CHECK(bool(e));
        CHECK(e.level() == 1);
        CHECK(e.size() == huge);
        CHECK(e.addr() % huge == 0);
        CHECK(map::find(s.start()).addr() == e.addr());
        map::depopulate(s.range(0, huge));
    }
}

void mapping_to_phys()
{
    function("mapping::to_phys");

    test("a mapped address translates, offset included");
    {
        scratch s(4 * page);
        auto pa = fr::alloc();
        CHECK(map::attach(s.range(0, page), pa, mem::perm_rw));
        CHECK(map::to_phys(s.start()) == pa);
        CHECK(map::to_phys(s.start() + 100) == pa + 100);
        CHECK(map::to_phys(s.ptr(200)) == pa + 200);
        map::detach(s.range(0, page));
        fr::free(pa);
    }

    test("an unmapped address is no_memory");
    {
        scratch s(4 * page);
        CHECK(map::to_phys(s.start()) == fr::no_memory);
        CHECK(map::to_phys(s.ptr(page)) == fr::no_memory);
    }

    test("a pointer into physical memory translates back to it");
    {
        auto pa = fr::alloc(huge, huge);
        CHECK(map::to_phys(view(pa, huge)) == pa);
        CHECK(map::to_phys(view(pa, huge) + huge - 1) == pa + huge - 1);
        fr::free(pa, huge);
    }
}

void mapping_is_contiguous()
{
    function("mapping::is_contiguous");

    test("one leaf is one run, and a seam between two is not");
    {
        // The same frame at two neighbouring addresses: bytes flow across the
        // boundary virtually, but physically the second page starts over.
        scratch s(4 * page);
        auto pa = fr::alloc();
        CHECK(map::attach(s.range(0, page), pa, mem::perm_rw));
        CHECK(map::attach(s.range(page, page), pa, mem::perm_rw));
        CHECK(map::is_contiguous(s.ptr(), page));
        CHECK(map::is_contiguous(s.ptr(100), page - 100));
        CHECK(!map::is_contiguous(s.ptr(), 2 * page));
        CHECK(!map::is_contiguous(s.ptr(page - 1), 2));
        map::detach(s.range(0, 2 * page));
        fr::free(pa);
    }

    test("two leaves over contiguous frames are one run");
    {
        scratch s(4 * page);
        auto pa = fr::alloc(2 * page);
        CHECK(map::attach(s.range(0, 2 * page), pa, mem::perm_rw));
        CHECK(map::find(s.start()).level() == 0);
        CHECK(map::is_contiguous(s.ptr(), 2 * page));
        map::detach(s.range(0, 2 * page));
        fr::free(pa, 2 * page);
    }

    test("a run ending in unmapped memory is not, and physical memory always is");
    {
        scratch s(4 * page);
        auto pa = fr::alloc();
        CHECK(map::attach(s.range(0, page), pa, mem::perm_rw));
        CHECK(!map::is_contiguous(s.ptr(), page + 1));
        CHECK(!map::is_contiguous(s.ptr(page), page));
        CHECK(map::is_contiguous(view(pa), page));
        map::detach(s.range(0, page));
        fr::free(pa);
    }
}

void mapping_prepare()
{
    function("mapping::prepare");

    test("an empty level-0 slot, one store away from a usable mapping");
    {
        scratch s(page);
        auto e = map::prepare(s.start());
        CHECK(bool(e));
        CHECK(e.level() == 0);
        CHECK(e.empty());
        CHECK(!map::find(s.start()));

        auto f = fr::alloc();
        e.write(e.leaf_for(f, mem::perm_rw));
        map::barrier();
        s.ptr()[0] = 0x33;
        CHECK(view(f)[0] == 0x33);
        CHECK(map::find(s.start()).addr() == f);

        map::detach(s.range(0, page));
        fr::free(f);
    }

    test("the same slot again, holding what was written into it");
    {
        scratch s(page);
        auto f = fr::alloc();
        auto e = map::prepare(s.start());
        e.write(e.leaf_for(f, mem::perm_rw));
        map::barrier();
        auto again = map::prepare(s.start());
        CHECK(!again.empty());
        CHECK(again.addr() == f);
        map::detach(s.range(0, page));
        fr::free(f);
    }

    test("a 2 MiB leaf size stops one level up");
    {
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        auto e = map::prepare(s.start(), huge);
        CHECK(bool(e));
        CHECK(e.level() == 1);
        CHECK(e.size() == huge);
        CHECK(e.empty());

        auto b = fr::alloc(huge, huge);
        e.write(e.leaf_for(b, mem::perm_rw));
        map::barrier();
        s.ptr(huge - 1)[0] = 0x44;
        CHECK(view(b, huge)[huge - 1] == 0x44);
        CHECK(map::find(s.start() + page).level() == 1);

        map::detach(s.range(0, huge));
        fr::free(b, huge);
    }

    test("over a range, every level is built and costs nothing more per page");
    {
        scratch s(64 * page);
        CHECK(map::prepare(s.range(0, 64 * page)));
        size_t before = fr::free_bytes();
        bool ready = true;
        for (unsigned i = 0; i < 64; i++) {
            auto e = map::prepare(s.start() + i * page);
            ready = ready && e && e.level() == 0 && e.empty();
        }
        CHECK(ready);
        CHECK(fr::free_bytes() == before);
    }
}

void mapping_attach()
{
    function("mapping::attach");

    test("an attached frame is reachable at the address, and gone once detached");
    {
        scratch s(page);
        auto f = fr::alloc();
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        auto e = map::find(s.start());
        CHECK(bool(e));
        CHECK(e.addr() == f);
        CHECK(e.perm() & mem::perm_write);

        s.ptr()[0] = 0x5a;
        CHECK(view(f)[0] == 0x5a);

        map::detach(s.range(0, page));
        CHECK(!map::find(s.start()));
        fr::free(f);
    }

    test("a range of frames, one leaf per page");
    {
        scratch s(4 * page);
        auto f = fr::alloc(4 * page);
        CHECK(map::attach(s.range(0, 4 * page), f, mem::perm_rw));
        for (unsigned i = 0; i < 4; i++) {
            auto e = map::find(s.start() + i * page);
            CHECK(bool(e));
            CHECK(e.addr() == f + i * page);
            s.ptr(i * page)[0] = char(i);
        }
        for (unsigned i = 0; i < 4; i++) {
            CHECK(view(f, 4 * page)[i * page] == char(i));
        }
        map::detach(s.range(0, 4 * page));
        fr::free(f, 4 * page);
    }

    test("the permissions asked for are what the entry gets");
    {
        scratch s(page);
        auto f = fr::alloc();
        for (unsigned perm : {mem::perm_read, mem::perm_rw, mem::perm_rwx}) {
            CHECK(map::attach(s.range(0, page), f, perm));
            CHECK(map::find(s.start()).perm() == perm);
            map::detach(s.range(0, page));
        }
        fr::free(f);
    }

    test("refuses a range already mapped, wholly or in part, and writes nothing");
    {
        scratch s(4 * page);
        auto f = fr::alloc();
        auto g = fr::alloc();
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        CHECK(!map::attach(s.range(0, page), g, mem::perm_rw));
        CHECK(map::find(s.start()).addr() == f);
        CHECK(!map::attach(s.range(0, 4 * page), g, mem::perm_rw));
        CHECK(!map::find(s.start() + page));
        map::detach(s.range(0, 4 * page));
        fr::free(f);
        fr::free(g);
    }

    test("2 MiB leaves where both sides allow, 4 KiB ones where they do not");
    {
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        auto b = fr::alloc(huge, huge);
        CHECK(map::attach(s.range(0, huge), b, mem::perm_rw));
        CHECK(map::find(s.start()).level() == 1);
        map::detach(s.range(0, huge));

        // The same range over the same block, a page along: the addresses no
        // longer agree on a 2 MiB boundary.
        CHECK(map::attach(s.range(0, huge - page), b + page, mem::perm_rw));
        CHECK(map::find(s.start()).level() == 0);
        CHECK(map::find(s.start()).addr() == b + page);
        map::detach(s.range(0, huge));
        fr::free(b, huge);
    }

    test("an empty range is accepted and maps nothing");
    {
        scratch s(page);
        auto f = fr::alloc();
        CHECK(map::attach(s.range(0, 0), f, mem::perm_rw));
        CHECK(!map::find(s.start()));
        fr::free(f);
    }
}

void mapping_attach_missing()
{
    function("mapping::attach_missing");

    test("fills the pages that are not there and leaves the one that is");
    {
        scratch s(4 * page);
        auto f = fr::alloc(4 * page);
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        CHECK(map::attach_missing(s.range(0, 4 * page), f, mem::perm_rw));
        for (unsigned i = 0; i < 4; i++) {
            auto e = map::find(s.start() + i * page);
            CHECK(bool(e));
            CHECK(e.addr() == f + i * page);
        }
        map::detach(s.range(0, 4 * page));
        fr::free(f, 4 * page);
    }

    test("refuses a translation that disagrees with what is there");
    {
        scratch s(4 * page);
        auto f = fr::alloc(4 * page);
        auto g = fr::alloc(4 * page);
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        CHECK(!map::attach_missing(s.range(0, 4 * page), g, mem::perm_rw));
        CHECK(map::find(s.start()).addr() == f);
        CHECK(!map::find(s.start() + page));
        map::detach(s.range(0, 4 * page));
        fr::free(f, 4 * page);
        fr::free(g, 4 * page);
    }

    test("slop maps the surroundings too, as far as the addresses run together");
    {
        scratch s(4 * page, 4 * page);
        auto f = fr::alloc(4 * page, 4 * page);
        CHECK(map::attach_missing(s.range(page, page), f + page, mem::perm_rw, 4 * page));
        for (unsigned i = 0; i < 4; i++) {
            auto e = map::find(s.start() + i * page);
            CHECK(bool(e));
            CHECK(e.addr() == f + i * page);
        }
        map::detach(s.range(0, 4 * page));
        fr::free(f, 4 * page);
    }
}

void mapping_populate()
{
    function("mapping::populate");

    test("a frame per page, zeroed, that keeps what is written");
    {
        scratch s(64 * page);
        size_t before = fr::free_bytes();
        CHECK(map::populate(s.range(0, 64 * page), mem::perm_rw));
        CHECK(before - fr::free_bytes() >= 64 * page);
        bool zero = true, mapped = true;
        for (unsigned i = 0; i < 64; i++) {
            mapped = mapped && map::find(s.start() + i * page);
            zero = zero && s.ptr(i * page)[0] == 0 && s.ptr(i * page)[page - 1] == 0;
            s.ptr(i * page)[0] = char(i);
        }
        CHECK(mapped);
        CHECK(zero);
        bool kept = true;
        for (unsigned i = 0; i < 64; i++) {
            kept = kept && s.ptr(i * page)[0] == char(i);
        }
        CHECK(kept);
        map::depopulate(s.range(0, 64 * page));
    }

    test("a 2 MiB leaf size takes one block per leaf");
    {
        scratch s(2 * huge, huge);
        map::detach(s.range(0, 2 * huge));
        size_t before = fr::free_bytes();
        CHECK(map::populate(s.range(0, 2 * huge), mem::perm_rw, huge));
        CHECK(before - fr::free_bytes() >= 2 * huge);
        CHECK(map::find(s.start()).level() == 1);
        CHECK(map::find(s.start() + huge).level() == 1);
        CHECK(map::find(s.start()).addr() != map::find(s.start() + huge).addr());
        s.ptr(2 * huge - 1)[0] = 0x7e;
        CHECK(s.ptr(2 * huge - 1)[0] == 0x7e);
        map::depopulate(s.range(0, 2 * huge));
    }

    test("zero = false still maps every page");
    {
        scratch s(8 * page);
        CHECK(map::populate(s.range(0, 8 * page), mem::perm_rw, page, false));
        bool mapped = true;
        for (unsigned i = 0; i < 8; i++) {
            mapped = mapped && map::find(s.start() + i * page);
            s.ptr(i * page)[0] = 1;
        }
        CHECK(mapped);
        map::depopulate(s.range(0, 8 * page));
    }

    test("refuses a range with anything mapped in it, and takes no frame");
    {
        scratch s(4 * page);
        CHECK(map::populate(s.range(page, page), mem::perm_rw));
        auto f = map::find(s.start() + page).addr();
        size_t before = fr::free_bytes();
        CHECK(!map::populate(s.range(0, 4 * page), mem::perm_rw));
        CHECK(fr::free_bytes() == before);
        CHECK(map::find(s.start() + page).addr() == f);
        CHECK(!map::find(s.start()));
        CHECK(!map::find(s.start() + 2 * page));
        map::depopulate(s.range(0, 4 * page));
    }

    test("what was installed before a failure stays until depopulate takes it back");
    {
        // More than the allocator holds, in 2 MiB leaves and unzeroed so the
        // run is short. Every block the allocator can find goes into it.
        size_t want = align_up(fr::total_available_bytes() + 64 * huge, huge);
        scratch s(want, huge);
        size_t before = fr::free_bytes();
        CHECK(!map::populate(s.range(0, want), mem::perm_rw, huge, false));
        CHECK(bool(map::find(s.start())));
        CHECK(fr::free_bytes() < before);
        map::depopulate(s.range(0, want));
        CHECK(!map::find(s.start()));
        CHECK(fr::free_bytes() + (1ul << 20) >= before);
    }
}

void mapping_detach()
{
    function("mapping::detach");

    test("clears the entries and keeps the frames");
    {
        scratch s(4 * page);
        auto f = fr::alloc(4 * page);
        CHECK(map::attach(s.range(0, 4 * page), f, mem::perm_rw));
        s.ptr(3 * page)[0] = 0x66;
        size_t before = fr::free_bytes();
        map::detach(s.range(0, 4 * page));
        for (unsigned i = 0; i < 4; i++) {
            CHECK(!map::find(s.start() + i * page));
        }
        CHECK(fr::free_bytes() <= before);
        CHECK(view(f, 4 * page)[3 * page] == 0x66);
        fr::free(f, 4 * page);
    }

    test("a range only partly mapped detaches what is there");
    {
        scratch s(4 * page);
        auto f = fr::alloc();
        CHECK(map::attach(s.range(2 * page, page), f, mem::perm_rw));
        map::detach(s.range(0, 4 * page));
        CHECK(!map::find(s.start() + 2 * page));
        fr::free(f);
    }

    test("the TLB is flushed: a new frame at the same address is what is read");
    {
        scratch s(page);
        auto f = marked('f');
        auto g = marked('g');
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        CHECK(peek(s.start()) == 'f');
        map::detach(s.range(0, page));
        CHECK(map::attach(s.range(0, page), g, mem::perm_rw));
        CHECK(peek(s.start()) == 'g');
        map::detach(s.range(0, page));
        fr::free(f);
        fr::free(g);
    }
}

void mapping_detach_deferred()
{
    function("mapping::detach_deferred");

    test("clears the entries, records the addresses, and flushes nothing");
    {
        scratch s(4 * page);
        auto f = fr::alloc(4 * page);
        CHECK(map::attach(s.range(0, 4 * page), f, mem::perm_rw));
        auto before = map::flush_epoch();

        map::pending_invalidation stale;
        map::detach_deferred(s.range(0, 2 * page), stale);
        CHECK(stale.count == 2);
        CHECK(stale.va[0] == s.start());
        CHECK(stale.va[1] == s.start() + page);
        CHECK(!stale.all);
        CHECK(stale.epoch == before);
        CHECK(!map::find(s.start()));
        CHECK(bool(map::find(s.start() + 2 * page)));
        CHECK(map::flush_epoch() == before);

        // A second call adds to the same list.
        map::detach_deferred(s.range(2 * page, 2 * page), stale);
        CHECK(stale.count == 4);
        CHECK(stale.va[3] == s.start() + 3 * page);
        stale.invalidate();
        fr::free(f, 4 * page);
    }

    test("past flush_batch addresses the list says all");
    {
        const size_t n = map::flush_batch + 1;
        scratch s(n * page);
        auto f = fr::alloc(n * page);
        CHECK(map::attach(s.range(0, n * page), f, mem::perm_rw));
        map::pending_invalidation stale;
        map::detach_deferred(s.range(0, n * page), stale);
        CHECK(stale.count == map::flush_batch);
        CHECK(stale.all);
        stale.invalidate();
        fr::free(f, n * page);
    }

    test("invalidate makes a new frame at the address what is read");
    {
        scratch s(page);
        auto f = marked('f');
        auto g = marked('g');
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        CHECK(peek(s.start()) == 'f');
        map::pending_invalidation stale;
        map::detach_deferred(s.range(0, page), stale);
        CHECK(map::attach(s.range(0, page), g, mem::perm_rw));
        stale.invalidate();
        CHECK(peek(s.start()) == 'g');
        map::detach(s.range(0, page));
        fr::free(f);
        fr::free(g);
    }

    test("two global flushes since the detach settle the list without another");
    {
        scratch s(page);
        auto f = fr::alloc();
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        map::pending_invalidation stale;
        map::detach_deferred(s.range(0, page), stale);
        map::flush_all();
        map::flush_all();
        auto quiet = map::flush_epoch();
        stale.invalidate();
        CHECK(map::flush_epoch() == quiet);
        CHECK(stale.count == 0);
        CHECK(!stale.all);
        CHECK(stale.epoch == map::never_flushed);
        fr::free(f);
    }
}

void mapping_depopulate()
{
    function("mapping::depopulate");

    test("clears the entries and gives the frames back");
    {
        scratch s(64 * page);
        size_t before = fr::free_bytes();
        CHECK(map::populate(s.range(0, 64 * page), mem::perm_rw));
        map::depopulate(s.range(0, 64 * page));
        bool gone = true;
        for (unsigned i = 0; i < 64; i++) {
            gone = gone && !map::find(s.start() + i * page);
        }
        CHECK(gone);
        CHECK(fr::free_bytes() + page >= before);
    }

    test("a range with holes frees what is there");
    {
        scratch s(4 * page);
        size_t before = fr::free_bytes();
        CHECK(map::populate(s.range(0, page), mem::perm_rw));
        CHECK(map::populate(s.range(2 * page, page), mem::perm_rw));
        map::depopulate(s.range(0, 4 * page));
        CHECK(!map::find(s.start()));
        CHECK(!map::find(s.start() + 2 * page));
        CHECK(fr::free_bytes() + page >= before);
    }

    test("a 2 MiB leaf gives its block back");
    {
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        size_t before = fr::free_bytes();
        CHECK(map::populate(s.range(0, huge), mem::perm_rw, huge));
        CHECK(before - fr::free_bytes() >= huge);
        map::depopulate(s.range(0, huge));
        CHECK(!map::find(s.start()));
        CHECK(fr::free_bytes() + page >= before);
    }

    test("more than a batch goes back in batches");
    {
        const size_t n = 4 * map::flush_batch + 3;
        scratch s(n * page);
        size_t before = fr::free_bytes();
        CHECK(map::populate(s.range(0, n * page), mem::perm_rw));
        map::depopulate(s.range(0, n * page));
        CHECK(!map::find(s.start() + (n - 1) * page));
        CHECK(fr::free_bytes() + page >= before);
    }
}

void mapping_protect()
{
    function("mapping::protect");

    test("changes what an entry allows without moving the frame");
    {
        scratch s(page);
        CHECK(map::populate(s.range(0, page), mem::perm_rw));
        auto phys = map::find(s.start()).addr();
        s.ptr()[0] = 0x11;

        map::protect(s.range(0, page), mem::perm_read);
        CHECK(map::find(s.start()).perm() == mem::perm_read);
        CHECK(map::find(s.start()).addr() == phys);
        CHECK(s.ptr()[0] == 0x11);

        map::protect(s.range(0, page), mem::perm_none);
        CHECK(map::find(s.start()).perm() == mem::perm_none);
        CHECK(map::find(s.start()).addr() == phys);

        map::protect(s.range(0, page), mem::perm_rw);
        CHECK(map::find(s.start()).perm() & mem::perm_write);
        CHECK(s.ptr()[0] == 0x11);
        map::depopulate(s.range(0, page));
    }

    test("every mapped entry in the range, and none that is not");
    {
        scratch s(4 * page);
        CHECK(map::populate(s.range(0, page), mem::perm_rw));
        CHECK(map::populate(s.range(3 * page, page), mem::perm_rw));
        map::protect(s.range(0, 4 * page), mem::perm_read);
        CHECK(map::find(s.start()).perm() == mem::perm_read);
        CHECK(map::find(s.start() + 3 * page).perm() == mem::perm_read);
        CHECK(!map::find(s.start() + page));
        map::depopulate(s.range(0, 4 * page));
    }

    test("a 2 MiB leaf is protected as one");
    {
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        CHECK(map::populate(s.range(0, huge), mem::perm_rw, huge));
        map::protect(s.range(0, huge), mem::perm_read);
        auto e = map::find(s.start() + huge / 2);
        CHECK(e.level() == 1);
        CHECK(e.perm() == mem::perm_read);
        map::depopulate(s.range(0, huge));
    }
}

void mapping_split()
{
    function("mapping::split");

    test("a 2 MiB leaf becomes 512 leaves over the same block");
    {
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        CHECK(map::populate(s.range(0, huge), mem::perm_rw, huge));
        auto phys = map::find(s.start()).addr();
        s.ptr(huge - 1)[0] = 0x7e;

        map::split(s.range(0, huge));
        bool small = true;
        for (unsigned i = 0; i < 512; i++) {
            auto e = map::find(s.start() + i * page);
            small = small && e && e.level() == 0 && e.addr() == phys + i * page;
        }
        CHECK(small);
        CHECK(s.ptr(huge - 1)[0] == 0x7e);
        map::depopulate(s.range(0, huge));
    }

    test("a range covering part of a leaf splits the whole leaf");
    {
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        CHECK(map::populate(s.range(0, huge), mem::perm_rw, huge));
        map::split(s.range(huge / 2, page));
        CHECK(map::find(s.start()).level() == 0);
        CHECK(map::find(s.start() + huge - page).level() == 0);
        map::depopulate(s.range(0, huge));
    }

    test("4 KiB leaves are left as they are");
    {
        scratch s(4 * page);
        CHECK(map::populate(s.range(0, 4 * page), mem::perm_rw));
        auto phys = map::find(s.start()).addr();
        map::split(s.range(0, 4 * page));
        CHECK(map::find(s.start()).level() == 0);
        CHECK(map::find(s.start()).addr() == phys);
        map::depopulate(s.range(0, 4 * page));
    }

    test("a block split into pages is given back as pages");
    {
        // What depopulate hands the allocator after a split: 512 frames of a
        // block it handed out as one.
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        size_t before = fr::free_bytes();
        CHECK(map::populate(s.range(0, huge), mem::perm_rw, huge));
        map::split(s.range(0, huge));
        map::depopulate(s.range(0, huge));
        CHECK(fr::free_bytes() + page >= before);
        auto b = fr::alloc(huge, huge);
        CHECK(b != fr::no_memory);
        fr::free(b, huge);
    }
}

// An entry rewritten by hand, which is what the flushes exist for: after the
// swap the TLB of this cpu still names the old frame until it is told.
void swap_by_hand(map::pte_ref e, fr::phys_addr to)
{
    e.write(e.leaf_for(to, mem::perm_rw));
    map::barrier();
}

void mapping_flush()
{
    function("mapping::flush_local, mapping::flush_range, mapping::flush_all");

    test("flush_local: this cpu sees an entry rewritten by hand");
    {
        scratch s(page);
        auto f = marked('f');
        auto g = marked('g');
        auto e = map::prepare(s.start());
        swap_by_hand(e, f);
        CHECK(peek(s.start()) == 'f');
        swap_by_hand(e, g);
        map::flush_local(s.range(0, page));
        CHECK(peek(s.start()) == 'g');
        map::detach(s.range(0, page));
        fr::free(f);
        fr::free(g);
    }

    test("flush_range: every cpu does, and the epoch does not move");
    {
        scratch s(page);
        auto f = marked('f');
        auto g = marked('g');
        auto e = map::prepare(s.start());
        swap_by_hand(e, f);
        unsigned threads = std::min(n_cpus(), 8u);
        std::atomic<unsigned> saw_f{0};
        parallel(threads, [&](unsigned) { saw_f += peek(s.start()) == 'f'; });
        CHECK(saw_f.load() == threads);
        swap_by_hand(e, g);
        auto before = map::flush_epoch();
        map::flush_range(s.range(0, page));
        CHECK(map::flush_epoch() == before);
        std::atomic<unsigned> saw_g{0};
        parallel(threads, [&](unsigned) { saw_g += peek(s.start()) == 'g'; });
        CHECK(saw_g.load() == threads);
        map::detach(s.range(0, page));
        fr::free(f);
        fr::free(g);
    }

    test("flush_all: every cpu does, and the epoch advances");
    {
        scratch s(page);
        auto f = marked('f');
        auto g = marked('g');
        auto e = map::prepare(s.start());
        swap_by_hand(e, f);
        unsigned threads = std::min(n_cpus(), 8u);
        parallel(threads, [&](unsigned) { peek(s.start()); });
        swap_by_hand(e, g);
        auto before = map::flush_epoch();
        map::flush_all();
        CHECK(map::flush_epoch() > before);
        std::atomic<unsigned> saw_g{0};
        parallel(threads, [&](unsigned) { saw_g += peek(s.start()) == 'g'; });
        CHECK(saw_g.load() == threads);
        map::detach(s.range(0, page));
        fr::free(f);
        fr::free(g);
    }

    test("a range past flush_batch pages is flushed whole");
    {
        const size_t n = map::flush_batch + 1;
        scratch s(n * page);
        auto f = fr::alloc(n * page);
        auto g = fr::alloc(n * page);
        memset(view(f, n * page), 'f', n * page);
        memset(view(g, n * page), 'g', n * page);
        CHECK(map::prepare(s.range(0, n * page)));
        std::vector<map::pte_ref> slot(n);
        for (size_t i = 0; i < n; i++) {
            slot[i] = map::prepare(s.start() + i * page);
            swap_by_hand(slot[i], f + i * page);
        }
        CHECK(peek(s.start() + (n - 1) * page) == 'f');
        for (size_t i = 0; i < n; i++) {
            swap_by_hand(slot[i], g + i * page);
        }
        map::flush_range(s.range(0, n * page));
        CHECK(peek(s.start() + (n - 1) * page) == 'g');
        map::flush_local(s.range(0, n * page));
        CHECK(peek(s.start()) == 'g');
        map::detach(s.range(0, n * page));
        fr::free(f, n * page);
        fr::free(g, n * page);
    }
}

void mapping_flush_epoch()
{
    function("mapping::flush_epoch, mapping::barrier");

    test("the epoch counts global flushes and nothing else");
    {
        auto e0 = map::flush_epoch();
        scratch s(4 * page);
        CHECK(map::populate(s.range(0, 4 * page), mem::perm_rw));
        map::protect(s.range(0, 4 * page), mem::perm_read);
        map::flush_local(s.range(0, 4 * page));
        map::flush_range(s.range(0, 4 * page));
        map::depopulate(s.range(0, 4 * page));
        CHECK(map::flush_epoch() == e0);
        map::flush_all();
        CHECK(map::flush_epoch() > e0);
    }

    test("barrier makes an entry written by hand usable");
    {
        scratch s(page);
        auto f = marked('f');
        auto e = map::prepare(s.start());
        e.write(e.leaf_for(f, mem::perm_rw));
        map::barrier();
        CHECK(peek(s.start()) == 'f');
        map::detach(s.range(0, page));
        fr::free(f);
    }
}

void mapping_bits()
{
    function("mapping::accessed, mapping::dirty, mapping::clear_accessed, mapping::clear_dirty");

    test("a read sets accessed, a write sets dirty, and clearing starts over");
    {
        scratch s(page);
        CHECK(map::populate(s.range(0, page), mem::perm_rw));
        mem::range v = s.range(0, page);

        map::clear_accessed(v);
        map::clear_dirty(v);
        map::flush_all();
        CHECK(!map::accessed(v));
        if (map::tracks_writes) {
            CHECK(!map::dirty(v));
        }

        peek(s.start());
        CHECK(map::accessed(v));
        if (map::tracks_writes) {
            CHECK(!map::dirty(v));
        }

        *reinterpret_cast<volatile char *>(s.start()) = 1;
        CHECK(map::dirty(v));
        map::clear_dirty(v);
        if (map::tracks_writes) {
            CHECK(!map::dirty(v));
        }
        map::depopulate(v);
    }

    test("over a range, one touched page is enough, and clearing reaches every page");
    {
        scratch s(4 * page);
        CHECK(map::populate(s.range(0, 4 * page), mem::perm_rw));
        mem::range all = s.range(0, 4 * page);
        map::clear_accessed(all);
        map::clear_dirty(all);
        map::flush_all();
        *reinterpret_cast<volatile char *>(s.start() + 2 * page) = 1;
        CHECK(map::accessed(all));
        CHECK(map::accessed(s.range(2 * page, page)));
        CHECK(!map::accessed(s.range(0, page)));
        if (map::tracks_writes) {
            CHECK(map::dirty(s.range(2 * page, page)));
            CHECK(!map::dirty(s.range(0, 2 * page)));
        }
        for (unsigned i = 0; i < 4; i++) {
            peek(s.start() + i * page);
        }
        map::clear_accessed(all);
        map::clear_dirty(all);
        map::flush_all();
        CHECK(!map::accessed(all));
        map::depopulate(all);
    }

    test("clear_dirty with a pending list defers the flush to the caller");
    {
        scratch s(page);
        CHECK(map::populate(s.range(0, page), mem::perm_rw));
        mem::range v = s.range(0, page);
        *reinterpret_cast<volatile char *>(s.start()) = 1;
        map::pending_invalidation stale;
        map::clear_dirty(v, stale);
        if (map::tracks_writes) {
            CHECK(stale.count == 1);
            CHECK(stale.va[0] == s.start());
            CHECK(stale.epoch != map::never_flushed);
            CHECK(!map::dirty(v));
        }
        stale.invalidate();
        CHECK(stale.count == 0);
        map::depopulate(v);
    }
}

void mapping_pending_invalidation()
{
    function("mapping::pending_invalidation");

    test("add records up to flush_batch addresses, then says all");
    {
        map::pending_invalidation stale;
        CHECK(stale.count == 0);
        CHECK(!stale.all);
        CHECK(stale.epoch == map::never_flushed);
        for (size_t i = 0; i < map::flush_batch; i++) {
            stale.add(0x1000 * (i + 1));
        }
        CHECK(stale.count == map::flush_batch);
        CHECK(!stale.all);
        CHECK(stale.va[map::flush_batch - 1] == 0x1000 * map::flush_batch);
        stale.add(0x1000 * (map::flush_batch + 1));
        CHECK(stale.count == map::flush_batch);
        CHECK(stale.all);
        stale.epoch = map::flush_epoch();
        stale.invalidate();
    }

    test("invalidate empties the list");
    {
        map::pending_invalidation stale;
        stale.add(0x1000);
        stale.epoch = map::flush_epoch();
        stale.invalidate();
        CHECK(stale.count == 0);
        CHECK(!stale.all);
        CHECK(stale.epoch == map::never_flushed);
        stale.invalidate();
        CHECK(stale.count == 0);
    }
}

void mapping_pte_ref()
{
    function("mapping::pte_ref");

    test("empty is false; a leaf reports level, size, address and permissions");
    {
        map::pte_ref none;
        CHECK(!none);
        scratch s(page);
        auto f = fr::alloc();
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        auto e = map::find(s.start());
        CHECK(bool(e));
        CHECK(e.level() == 0);
        CHECK(e.size() == page);
        CHECK(e.addr() == f);
        CHECK(e.perm() == mem::perm_rw);
        CHECK(e.present());
        CHECK(!e.empty());
        map::detach(s.range(0, page));
        fr::free(f);
    }

    test("read, write, exchange and compare_exchange act on the slot");
    {
        scratch s(page);
        auto f = fr::alloc();
        auto g = fr::alloc();
        auto e = map::prepare(s.start());
        CHECK(e.read() == 0);
        map::pte leaf_f = e.leaf_for(f, mem::perm_rw);
        map::pte leaf_g = e.leaf_for(g, mem::perm_rw);
        e.write(leaf_f);
        CHECK(e.read() == leaf_f);
        CHECK(e.exchange(leaf_g) == leaf_f);
        CHECK(e.addr() == g);
        map::pte expected = leaf_f;
        CHECK(!e.compare_exchange(expected, leaf_f));
        CHECK(expected == leaf_g);
        CHECK(e.compare_exchange(expected, leaf_f));
        CHECK(e.addr() == f);
        map::barrier();
        map::detach(s.range(0, page));
        fr::free(f);
        fr::free(g);
    }

    test("leaf_for builds the entry for this slot's level");
    {
        scratch s(huge, huge);
        map::detach(s.range(0, huge));
        auto b = fr::alloc(huge, huge);
        auto e = map::prepare(s.start(), huge);
        e.write(e.leaf_for(b, mem::perm_read));
        map::barrier();
        CHECK(e.present());
        CHECK(e.addr() == b);
        CHECK(e.perm() == mem::perm_read);
        CHECK(map::find(s.start() + huge - 1).addr() == b);
        map::detach(s.range(0, huge));
        fr::free(b, huge);
    }

    test("the software bits survive a round trip without disturbing the entry");
    {
        scratch s(page);
        auto f = fr::alloc();
        CHECK(map::attach(s.range(0, page), f, mem::perm_rw));
        auto e = map::find(s.start());
        CHECK(map::sw_bits >= 3);
        for (unsigned n = 0; n < map::sw_bits; n++) {
            CHECK(!map::pte_sw_bit(e.read(), n));
            e.write(map::pte_set_sw_bit(e.read(), n, true));
            CHECK(map::pte_sw_bit(e.read(), n));
            CHECK(e.addr() == f);
            CHECK(e.perm() & mem::perm_write);
            e.write(map::pte_set_sw_bit(e.read(), n, false));
            CHECK(!map::pte_sw_bit(e.read(), n));
        }
        s.ptr()[0] = 0x44;
        map::detach(s.range(0, page));
        fr::free(f);
    }
}

void mapping_perf()
{
    group("mapping - performance");

    section("attach and detach one page");
    {
        const size_t pages = 512;
        const int rounds = 40;
        scratch s(pages * page);
        auto f = fr::alloc();
        map::prepare(s.range(0, pages * page), page);

        double total = 0;
        for (int r = 0; r < rounds; r++) {
            auto t0 = clk::now();
            for (size_t i = 0; i < pages; i++) {
                map::attach(s.range(i * page, page), f, mem::perm_rw);
            }
            total += since(t0);
            map::detach(s.range(0, pages * page));
        }
        report_ns("attach 4 KiB, levels already built", total,
                  double(rounds) * pages);

        const int n = 20000;
        auto t0 = clk::now();
        for (int i = 0; i < n; i++) {
            map::attach(s.range(0, page), f, mem::perm_rw);
            map::detach(s.range(0, page));
        }
        report_ns("attach + detach 4 KiB, one global flush each", since(t0), n);

        fr::free(f);
    }

    section("the prepared path: one store per page");
    {
        const size_t pages = 512;
        scratch s(pages * page);
        auto f = fr::alloc();
        std::vector<map::pte_ref> slot(pages);

        auto t0 = clk::now();
        for (size_t i = 0; i < pages; i++) {
            slot[i] = map::prepare(s.start() + i * page);
        }
        report_ns("prepare, per 4 KiB page", since(t0), pages);

        const int rounds = 2000;
        t0 = clk::now();
        for (int r = 0; r < rounds; r++) {
            for (size_t i = 0; i < pages; i++) {
                slot[i].write(slot[i].leaf_for(f, mem::perm_rw));
            }
        }
        map::barrier();
        report_ns("write a prepared leaf", since(t0), double(rounds) * pages);

        map::detach(s.range(0, pages * page));
        fr::free(f);
    }

    section("populate and depopulate");
    {
        struct { const char *name; size_t size; size_t leaf; int n; } cases[] = {
            {"64 pages, 4 KiB leaves", 64 * page, page, 2000},
            {"2 MiB, 4 KiB leaves",    huge,      page, 300},
            {"2 MiB, one huge leaf",   huge,      huge, 300},
        };
        for (auto &c : cases) {
            scratch s(c.size, c.leaf);
            auto t0 = clk::now();
            for (int i = 0; i < c.n; i++) {
                map::populate(s.range(0, c.size), mem::perm_rw, c.leaf);
                map::depopulate(s.range(0, c.size));
            }
            double s_total = since(t0);
            char label[80];
            snprintf(label, sizeof(label), "populate + depopulate %s", c.name);
            report_ns(label, s_total, c.n);
            snprintf(label, sizeof(label), "  the same, per 4 KiB page");
            report_ns(label, s_total, double(c.n) * (c.size / page));
        }
    }

    section("find");
    {
        scratch s(huge, huge);
        map::populate(s.range(0, huge), mem::perm_rw);
        const int probes = 200000;

        map::pte e = 0;
        auto t0 = clk::now();
        for (int i = 0; i < probes; i++) {
            e |= map::find(s.start() + (i % 512) * page).read();
        }
        escape(&e);
        report_ns("find, 4 KiB leaf", since(t0), probes);
        map::depopulate(s.range(0, huge));
    }
}

/* misc -------------------------------------------------------------------- */

void misc_map_phys()
{
    function("mem::map_phys");

    test("a pointer to the frames given, that reaches them");
    {
        auto pa = fr::alloc();
        auto *v = static_cast<char *>(mem::map_phys(pa, page));
        CHECK(v != nullptr);
        v[0] = 0x5c;
        v[page - 1] = 0x5d;
        CHECK(map::to_phys(v) == pa);
        CHECK(map::to_phys(v + page - 1) == pa + page - 1);
        fr::free(pa);
    }

    test("the same frames twice is the same pointer");
    {
        auto pa = fr::alloc(huge, huge);
        void *a = mem::map_phys(pa, huge);
        void *b = mem::map_phys(pa, huge);
        void *c = mem::map_phys(pa + page, page);
        CHECK(a == b);
        CHECK(c == static_cast<char *>(a) + page);
        fr::free(pa, huge);
    }

    test("the range is rounded out to whole pages and the pointer is to the byte");
    {
        auto pa = fr::alloc();
        auto *v = static_cast<char *>(mem::map_phys(pa + 100, 100));
        CHECK(map::to_phys(v) == pa + 100);
        CHECK(map::to_phys(v - 100) == pa);
        v[0] = 0x21;
        CHECK(view(pa)[100] == 0x21);
        fr::free(pa);
    }
}

void misc_map_phys_at()
{
    function("mem::map_phys_at");

    test("maps at the address asked, and reserves it for good");
    {
        // A free address outside the linear map: the mapping and the
        // reservation it makes are permanent, which is the contract.
        uintptr_t va;
        {
            vs::region r{};
            CHECK(vs::reserve(r, page, page) == resa::success);
            va = r.span.start;
            vs::release(r);
        }
        auto pa = fr::alloc();
        memset(view(pa), 0x77, page);
        mem::map_phys_at(reinterpret_cast<void *>(va), pa, page);
        CHECK(map::to_phys(va) == pa);
        CHECK(peek(va) == 0x77);
        *reinterpret_cast<volatile char *>(va + 1) = 0x78;
        CHECK(view(pa)[1] == 0x78);
        CHECK(map::find(va).perm() == mem::perm_rwx);

        vs::region *r = vs::lookup(va);
        CHECK(r != nullptr);
        if (r) {
            CHECK(r->span.start == va);
            CHECK(r->span.end == va + page);
            CHECK(r->perm == mem::perm_rwx);
            CHECK(r->ops == nullptr);
        }
        CHECK(vs::reserved({va, va + page}));
    }
}

// A region whose faults the test answers itself: one page per fault, recorded.
struct probe_region {
    vs::region r;
    std::atomic<int> faults{0};
    std::atomic<uintptr_t> last_addr{0};
    std::atomic<unsigned> last_error{0};
    std::atomic<bool> inside{true};
};

bool probe_fault(vs::region &r, uintptr_t addr, unsigned error)
{
    auto *pr = reinterpret_cast<probe_region *>(&r);
    pr->faults.fetch_add(1);
    pr->last_addr.store(addr);
    pr->last_error.store(error);
    if (!r.span.contains(addr)) {
        pr->inside = false;
    }
    uintptr_t s = align_down(addr, uintptr_t(page));
    return map::populate({s, s + page}, r.perm) || map::find(s);
}

const vs::region_ops probe_ops = {probe_fault};

// The SIGSEGV side, for an address in no region, a region without ops, a
// handler that answers false or an access the region's perm does not allow,
// is not covered: for code inside the kernel image it aborts the kernel.
void misc_vm_fault()
{
    function("mem::vm_fault");

    test("a fault reaches the handler of the region that owns the address, with the byte");
    {
        probe_region pr;
        pr.r.ops = &probe_ops;
        pr.r.perm = mem::perm_rw;
        CHECK(vs::reserve(pr.r, 16 * page, page) == resa::success);
        uintptr_t at = pr.r.span.start + 3 * page + 17;
        *reinterpret_cast<volatile char *>(at) = 3;
        CHECK(pr.faults.load() == 1);
        CHECK(pr.last_addr.load() == at);
        CHECK(pr.inside.load());
        CHECK(peek(at) == 3);
        map::depopulate({pr.r.span.start, pr.r.span.end});
        vs::release(pr.r);
    }

    test("the error code says whether the access was a write");
    {
        probe_region pr;
        pr.r.ops = &probe_ops;
        pr.r.perm = mem::perm_rw;
        CHECK(vs::reserve(pr.r, 4 * page, page) == resa::success);
        *reinterpret_cast<volatile char *>(pr.r.span.start) = 1;
        CHECK(map::is_page_fault_write(pr.last_error.load()));
        peek(pr.r.span.start + page);
        CHECK(!map::is_page_fault_write(pr.last_error.load()));
        CHECK(pr.faults.load() == 2);
        map::depopulate({pr.r.span.start, pr.r.span.end});
        vs::release(pr.r);
    }

    test("a page the handler mapped does not fault again, and the others stay absent");
    {
        probe_region pr;
        pr.r.ops = &probe_ops;
        pr.r.perm = mem::perm_rw;
        CHECK(vs::reserve(pr.r, 16 * page, page) == resa::success);
        auto *p = reinterpret_cast<volatile char *>(pr.r.span.start);
        p[0] = 1;
        p[3 * page] = 3;
        p[1] = 2;
        p[3 * page + 1] = 4;
        CHECK(pr.faults.load() == 2);
        CHECK(p[0] == 1 && p[1] == 2);
        CHECK(p[3 * page] == 3 && p[3 * page + 1] == 4);
        CHECK(!map::find(pr.r.span.start + page));
        CHECK(!map::find(pr.r.span.start + 15 * page));
        map::depopulate({pr.r.span.start, pr.r.span.end});
        vs::release(pr.r);
    }

    test("faults from every cpu on one region each reach the handler");
    {
        probe_region pr;
        pr.r.ops = &probe_ops;
        pr.r.perm = mem::perm_rw;
        unsigned threads = n_cpus();
        const int per_thread = 32;
        CHECK(vs::reserve(pr.r, threads * per_thread * page, page) == resa::success);
        parallel(threads, [&](unsigned id) {
            for (int i = 0; i < per_thread; i++) {
                auto *p = reinterpret_cast<volatile char *>(
                    pr.r.span.start + (id * per_thread + i) * page);
                *p = char(id + 1);
            }
        });
        CHECK(pr.faults.load() == int(threads * per_thread));
        CHECK(pr.inside.load());
        bool kept = true;
        for (unsigned id = 0; id < threads; id++) {
            for (int i = 0; i < per_thread; i++) {
                kept = kept && peek(pr.r.span.start + (id * per_thread + i) * page) == char(id + 1);
            }
        }
        CHECK(kept);
        map::depopulate({pr.r.span.start, pr.r.span.end});
        vs::release(pr.r);
    }
}

}

int os_memory_primitives_main()
{
    reset();
    printf("######## memory primitives ########\n");
    printf("cpus: %u, memory: %zu MiB\n", n_cpus(), fr::total_available_bytes() >> 20);

    group("vspace");
    vspace_app_window();
    vspace_reserve();
    vspace_reserve_at();
    vspace_release();
    vspace_lookup();
    vspace_reserved();
    vspace_accounting();
    vspace_for_each();
    vspace_self_check();
    vspace_perf();

    group("frames");
    frames_alloc();
    frames_free();
    frames_accounting();
    frames_watch_pressure();
    frames_under_pressure();
    frames_check_pressure();
    frames_reclaim();
    frames_perf();

    group("mapping");
    mapping_find();
    mapping_to_phys();
    mapping_is_contiguous();
    mapping_prepare();
    mapping_attach();
    mapping_attach_missing();
    mapping_populate();
    mapping_detach();
    mapping_detach_deferred();
    mapping_depopulate();
    mapping_protect();
    mapping_split();
    mapping_flush();
    mapping_flush_epoch();
    mapping_bits();
    mapping_pending_invalidation();
    mapping_pte_ref();
    mapping_perf();

    group("misc");
    misc_map_phys();
    misc_map_phys_at();
    misc_vm_fault();

    return summary("MEMORY PRIMITIVE");
}
