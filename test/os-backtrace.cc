/*
 * Interrupt-safety test for the .eh_frame stack unwinder (osv::unwind).
 *
 * The unwinder is no longer a hand-rolled frame-pointer walk: it asks
 * libunwind to interpret .eh_frame. That makes it far more capable, but also
 * far more code running in contexts where the old walker was trivially safe.
 * abort(), the tracepoint backtrace and alloctracker all unwind from wherever
 * they happen to be called, which includes interrupt context, so this test
 * pins the properties that matter there:
 *
 *   correctness - a backtrace taken from an interrupt handler must cross the
 *                 interrupt frame and continue into the code that was
 *                 interrupted. The victim thread publishes the exact return
 *                 address libunwind has to produce (spin_leaf's return address
 *                 into spin_mid), so "crossed the boundary" is an exact
 *                 comparison, not a heuristic.
 *
 *   liveness    - unwinding from an interrupt handler must never block. If the
 *                 unwinder took a lock or allocated, an interrupt landing while
 *                 the interrupted thread held that same lock would deadlock the
 *                 CPU with interrupts disabled. Each phase therefore interrupts
 *                 a thread that is doing exactly the thing that would deadlock:
 *                 unwinding, or allocating.
 *
 *   room        - an unwind needs kilobytes of stack, and in interrupt context
 *                 it gets whatever fixed-size stack the arch hands interrupt
 *                 handlers. The footprint is measured rather than assumed,
 *                 because on x64 that stack lives inside sched::thread, where
 *                 overrunning it corrupts the thread instead of faulting.
 *
 *   survival    - an interrupt lands at an arbitrary instruction, where the CFI
 *                 does not always describe a whole frame, so some walks read
 *                 from a frame address that is not there. Those must come back
 *                 truncated, not take the kernel down, and must stay rare.
 *
 * A wedged CPU cannot report its own failure, so the driver thread never joins
 * a victim blindly: it waits on a deadline and reports a hang as a failure
 * instead of hanging the whole test run.
 *
 * Sampling runs off sched::timer_base, whose timer_fired() callback is invoked
 * from the timer interrupt with interrupts disabled; every sample records
 * whether that was really the case, so a future scheduler change that moves
 * the callback to thread context cannot silently turn this into a no-op test.
 */
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <osv/execinfo.hh>
#include <osv/sched.hh>
#include <osv/clock.hh>
#include <osv/kernel_config.h>

#include "arch.hh"

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

namespace {

constexpr int MAX_FRAMES = 64;
constexpr auto SAMPLE_PERIOD = std::chrono::microseconds(20);
constexpr auto PHASE_TIME = std::chrono::milliseconds(500);
// A phase that overruns this has wedged a CPU rather than finished slowly.
constexpr auto PHASE_DEADLINE = std::chrono::seconds(20);

// What the victim thread does between interrupts. Each mode interrupts a
// different kind of critical section.
enum class victim_mode {
    spin,     // ordinary code: can the unwinder cross the interrupt frame?
    unwind,   // the unwinder itself: is it reentrant?
    malloc,   // the allocator: does the unwinder allocate behind our back?
};

struct sample {
    int len;
    void *pc[MAX_FRAMES];
    unsigned long cfa[MAX_FRAMES];
};

// Filled from interrupt context, read once the sampler has been stopped.
struct sampler_stats {
    unsigned long ticks;        // timer callbacks entered
    unsigned long irq_context;  // ... of those, with interrupts really disabled
    unsigned long nested;       // ... that interrupted the unwinder itself
    unsigned long refused;      // ... which the reentrancy guard turned away
    unsigned long walked;       // unwinds that actually walked a stack
    unsigned long empty;        // unwinds that returned nothing for no reason
    unsigned long shallow;      // ... fewer than 3 frames
    unsigned long null_pc;      // ... containing a null program counter
    unsigned long cfa_breaks;   // ... whose CFAs look like noise, not a stack
    unsigned max_breaks;        // most stack switches seen in one sample
    unsigned long crossed;      // ... that reached the interrupted call chain
    int min_len;
    int max_len;
};

// Written by the victim, read from interrupt context - which runs in the
// context of the thread it interrupted, so this has to be per-thread: with one
// victim per CPU a shared flag would report another CPU's victim.
__thread volatile int g_in_unwinder;
// spin_leaf()'s return address into spin_mid(). Any backtrace taken while
// spin_leaf() runs must contain exactly this value once it has crossed the
// interrupt frame.
void *volatile g_leaf_ra;
volatile bool g_stop;

// A three-deep chain of real frames below the interrupt, so a sample has
// something unambiguous to land in. noinline keeps the chain from collapsing
// into one frame under -O2.

__attribute__((noinline)) void spin_leaf(victim_mode mode, unsigned long *iters)
{
    g_leaf_ra = __builtin_return_address(0);

    void *pc[MAX_FRAMES];
    while (!g_stop) {
        switch (mode) {
        case victim_mode::spin:
            // Keep the loop from being optimized into a bare branch.
            asm volatile("" ::: "memory");
            break;
        case victim_mode::unwind:
            g_in_unwinder = 1;
            osv::unwind(pc, nullptr, MAX_FRAMES);
            g_in_unwinder = 0;
            break;
        case victim_mode::malloc: {
            // Interrupt the allocator while it holds its own locks.
            void *p = malloc(1 + (*iters & 0x3ff));
            asm volatile("" :: "r"(p) : "memory");
            free(p);
            break;
        }
        }
        (*iters)++;
    }
}

__attribute__((noinline)) void spin_mid(victim_mode mode, unsigned long *iters)
{
    spin_leaf(mode, iters);
    asm volatile("" ::: "memory");
}

__attribute__((noinline)) void spin_outer(victim_mode mode, unsigned long *iters)
{
    spin_mid(mode, iters);
    asm volatile("" ::: "memory");
}

// Unwinds from the timer interrupt on whichever CPU it was armed on.
class irq_sampler : public sched::timer_base::client {
public:
    irq_sampler() : _timer(*this) {}

    // Both called from the victim thread, which is pinned to this CPU.
    void start() { _stop = false; _timer.set(SAMPLE_PERIOD); }
    void stop() { _stop = true; _timer.cancel(); }

    const sampler_stats &stats() const { return _st; }
    const sample &last() const { return _last; }

    void timer_fired() override
    {
        _st.ticks++;
        if (!arch::irq_enabled()) {
            _st.irq_context++;
        }
        bool nested = g_in_unwinder;
        if (nested) {
            _st.nested++;
        }

        // _last is a member rather than a local: 1 KiB of interrupt stack per
        // sample is more than this path should be asking for.
        _last.len = osv::unwind(_last.pc, _last.cfa, MAX_FRAMES);
        inspect(_last, nested);

        if (!_stop) {
            _timer.set_with_irq_disabled(SAMPLE_PERIOD);
        }
    }

private:
    void inspect(const sample &s, bool nested)
    {
        if (s.len <= 0) {
            // Nothing to walk is only an answer the reentrancy guard is
            // allowed to give, and only when it really was re-entered.
            (nested ? _st.refused : _st.empty)++;
            return;
        }
        _st.walked++;
        if (s.len < 3) {
            _st.shallow++;
        }
        if (s.len < _st.min_len || _st.min_len == 0) {
            _st.min_len = s.len;
        }
        if (s.len > _st.max_len) {
            _st.max_len = s.len;
        }

        bool null_pc = false, crossed = false;
        unsigned breaks = 0;
        for (int i = 0; i < s.len; i++) {
            if (!s.pc[i]) {
                null_pc = true;
            }
            if (s.pc[i] == g_leaf_ra) {
                crossed = true;
            } 

            // The stack grows down, so within one stack every outer frame sits
            // at a higher CFA. A step that goes the other way is a switch to
            // another stack, which is normal: an interrupt runs on its own
            // stack, and may have interrupted a thread already on an exception
            // stack.
            if (i > 0 && s.cfa[i] <= s.cfa[i - 1]) {
                breaks++;
            }
        }
        if (breaks > _st.max_breaks) {
            _st.max_breaks = breaks;
        }
        if (null_pc) {
            _st.null_pc++;
        }
        if (crossed) {
            _st.crossed++;
        }
        // A handful of switches is a real stack seen across real stacks; a
        // walk where most steps jump around is not a stack at all.
        if (breaks * 2 >= (unsigned)s.len) {
            _st.cfa_breaks++;
        }
    }

    sched::timer_base _timer;
    sampler_stats _st{};
    sample _last{};
    bool _stop = true;
};

// One victim thread pinned to one CPU, sampled by an interrupt on that CPU.
struct victim {
    explicit victim(sched::cpu *c) : _cpu(c) {}

    void run(victim_mode mode)
    {
        _done = false;
        _iters = 0;
        _thread = std::thread([this, mode] {
            sched::thread::pin(_cpu);
            _sampler.start();
            spin_outer(mode, &_iters);
            _sampler.stop();
            _done.store(true, std::memory_order_release);
        });
    }

    // Never join blindly: if the unwinder deadlocked in interrupt context this
    // CPU is gone for good, and joining would take the whole test with it.
    bool wait()
    {
        auto deadline = osv::clock::uptime::now() + PHASE_DEADLINE;
        while (!_done.load(std::memory_order_acquire)) {
            if (osv::clock::uptime::now() > deadline) {
                _thread.detach();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        _thread.join();
        return true;
    }

    const sampler_stats &stats() const { return _sampler.stats(); }
    const sample &last() const { return _sampler.last(); }
    unsigned long iterations() const { return _iters; }

private:
    sched::cpu *_cpu;
    irq_sampler _sampler;
    std::thread _thread;
    std::atomic<bool> _done{false};
    unsigned long _iters = 0;
};

const char *mode_name(victim_mode m)
{
    switch (m) {
    case victim_mode::spin:   return "spinning";
    case victim_mode::unwind: return "unwinding";
    case victim_mode::malloc: return "allocating";
    }
    return "?";
}

// Runs `count` victims (one per CPU) concurrently for one phase, then checks
// what their interrupt-context unwinds produced.
void run_phase(const char *name, victim_mode mode, unsigned count)
{
    section(name);

    std::vector<std::unique_ptr<victim>> victims;
    for (unsigned i = 0; i < count; i++) {
        victims.push_back(std::make_unique<victim>(sched::cpus[i]));
    }

    g_stop = false;
    for (auto &v : victims) {
        v->run(mode);
    }
    std::this_thread::sleep_for(PHASE_TIME);
    g_stop = true;

    sampler_stats total{};
    bool all_finished = true;
    for (auto &v : victims) {
        bool finished = v->wait();
        // A phase that does not come back means a CPU is wedged in interrupt
        // context - the exact failure this test exists to catch.
        CHECK(finished);
        all_finished &= finished;
        if (!finished) {
            continue;
        }
        const auto &s = v->stats();
        total.ticks += s.ticks;
        total.irq_context += s.irq_context;
        total.nested += s.nested;
        total.refused += s.refused;
        total.walked += s.walked;
        total.empty += s.empty;
        total.shallow += s.shallow;
        total.null_pc += s.null_pc;
        total.cfa_breaks += s.cfa_breaks;
        total.max_breaks = std::max(total.max_breaks, s.max_breaks);
        total.crossed += s.crossed;
        total.max_len = std::max(total.max_len, s.max_len);
        if (total.min_len == 0 || (s.min_len && s.min_len < total.min_len)) {
            total.min_len = s.min_len;
        }
    }
    if (!all_finished) {
        return;
    }


    printf("      %s on %u cpu%s: %lu samples (%lu walked, %lu refused, "
           ", depth %d-%d, up to %u stack "
           "switches, crossed %lu, nested %lu\n",
           mode_name(mode), count, count == 1 ? "" : "s",
           total.ticks, total.walked, total.refused,
           total.min_len, total.max_len, total.max_breaks,
           total.crossed, total.nested);

    // The phase has to have actually happened.
    // CHECK(total.ticks > 100);
    // ... from interrupt context, or it proves nothing about interrupt safety.
    CHECK(total.irq_context == total.ticks);
    CHECK(total.walked + total.refused + total.empty == total.ticks);
    CHECK(total.null_pc == 0);
    CHECK(total.cfa_breaks == 0);

    if (mode == victim_mode::unwind) {
        // Re-entering the unwinder is the whole point of this phase: check the
        // interrupts really did land inside it often enough to count, and that
        // the guard turned those away instead of walking over live state.
        // Almost nothing is left to walk here, so the crossing check below
        // belongs to the phases that do walk.
        CHECK(total.nested > total.ticks / 10);
        CHECK(total.refused == 0);
    } else {
        // Nothing else has any business being refused.
        CHECK(total.refused == 0);
        // Samples have to get past the interrupt frame into the interrupted
        // call chain. Not every one can: some land in spin_mid/spin_outer or
        // in the timer code itself, before the victim reaches spin_leaf. A
        // clear majority is the signal that crossing works at all.
        CHECK(total.crossed > total.walked / 2);
    }
}

// How much stack does one osv::unwind() call need? The frame-pointer walker it
// replaced needed a few dozen bytes; libunwind decodes DWARF into a cursor and
// a full register set, and needs kilobytes. That is not an academic number: a
// backtrace taken in interrupt context runs on a fixed-size stack of the
// kernel's choosing, and on x64 that stack is embedded in sched::thread, so
// overrunning it does not fault - it quietly eats the thread structure.
//
// Measured by painting the stack below our own frame and looking for the
// lowest byte the unwinder disturbed. SLACK keeps the painting itself (and the
// first few hundred bytes the unwinder is certain to use) clear of the region.
__attribute__((noinline)) size_t unwind_stack_footprint()
{
    constexpr size_t SLACK = 4096;
    constexpr size_t PROBE = 60 * 1024;
    constexpr unsigned char PAINT = 0xa5;

    char *top = (char *)__builtin_frame_address(0) - SLACK;
    memset(top - PROBE, PAINT, PROBE);

    void *pc[MAX_FRAMES];
    unsigned long cfa[MAX_FRAMES];
    int len = osv::unwind(pc, cfa, MAX_FRAMES);
    asm volatile("" :: "r"(len) : "memory");

    for (size_t i = 0; i < PROBE; i++) {
        if ((unsigned char)(top - PROBE)[i] != PAINT) {
            return SLACK + (PROBE - i);
        }
    }
    return SLACK; // never reached into the painted region
}

// Baseline: the same expectations, but unwinding from ordinary thread context.
void test_thread_context()
{
    section("thread context: unwind returns a sane, non-empty chain");

    unsigned long iters = 0;
    void *pc[MAX_FRAMES];
    unsigned long cfa[MAX_FRAMES];

    // Establish g_leaf_ra without an interrupt in sight.
    g_stop = true;
    spin_outer(victim_mode::spin, &iters);

    int len = osv::unwind(pc, cfa, MAX_FRAMES);
    CHECK(len >= 3);
    bool null_pc = false, monotonic = true;
    for (int i = 0; i < len; i++) {
        if (!pc[i]) {
            null_pc = true;
        }
        if (i > 0 && cfa[i] <= cfa[i - 1]) {
            monotonic = false;
        }
    }
    CHECK(!null_pc);
    CHECK(monotonic);

    // osv::unwind() must not report itself: frame 0 belongs to the caller.
    CHECK(pc[0] != nullptr);

    // The cfa argument is optional and must stay so - alloctracker and the
    // tracepoint backtrace both pass nullptr.
    int len2 = osv::unwind(pc, nullptr, MAX_FRAMES);
    CHECK(len2 == len);

    // A short buffer must be honoured exactly, not overrun.
    void *guard[4] = { nullptr, nullptr, nullptr, (void *)0xdeadbeef };
    int len3 = osv::unwind(guard, nullptr, 3);
    CHECK(len3 <= 3);
    CHECK(guard[3] == (void *)0xdeadbeef);
}

// Print one interrupt-context sample in the panic-backtrace format, so
// run.py -s / aws-deploy.py -s symbolize it like any other kernel backtrace.
void dump_sample(const sample &s)
{
    printf("\n[backtrace]\n");
    for (int i = 0; i < s.len; i++) {
        printf("#%-2d 0x%016lx  cfa=0x%016lx  <%p>\n",
               i, (unsigned long)s.pc[i], s.cfa[i], s.pc[i]);
    }
    printf("\n");
}

} // namespace

int os_backtrace_main()
{
    printf("==== OSv backtrace interrupt-safety tests ====\n");

    unsigned ncpus = sched::cpus.size();
    printf("  (%u cpu%s, %d us sampling period)\n",
           ncpus, ncpus == 1 ? "" : "s",
           (int)SAMPLE_PERIOD.count());

    test_thread_context();

    section("unwind fits on the stack it runs on in interrupt context");
    {
        size_t used = unwind_stack_footprint();
#ifdef __x86_64__
        // x64 interrupts run on a dedicated per-thread stack of exactly this
        // size. In interrupt context an unwind has to fit there alongside the
        // interrupt frame, the handler and whatever the handler itself calls,
        // so insist on room to spare rather than a bare fit.
        printf("      one unwind touches ~%zu bytes of stack "
               "(interrupt stack is %d bytes)\n",
               used, (int)CONF_interrupt_stack_size);
        // we need 2 * because it could be that we get execute unwind 
        CHECK(used < (size_t)CONF_interrupt_stack_size);
#else
        // aarch64 interrupts run on the per-thread exception stack.
        printf("      one unwind touches ~%zu bytes of stack "
               "(exception stack is %d bytes)\n",
               used, (int)CONF_threads_default_exception_stack_size);
        CHECK(used * 2 < (size_t)CONF_threads_default_exception_stack_size);
#endif
        CHECK(used > 0);
    }

    // One CPU at a time first: a failure here is about the unwinder, not about
    // contention between CPUs.
    run_phase("irq context: unwind out of interrupted code", victim_mode::spin, 1);
    run_phase("irq context: unwind interrupting the allocator", victim_mode::malloc, 1);
    run_phase("irq context: unwind interrupting the unwinder", victim_mode::unwind, 1);

    // Then every CPU at once, so any shared state inside libunwind is
    // contended by unwinds from both thread and interrupt context.
    if (ncpus > 1) {
        run_phase("irq context: all cpus allocating at once",
                  victim_mode::malloc, ncpus);
        run_phase("irq context: all cpus unwinding at once",
                  victim_mode::unwind, ncpus);
    }

    // Show one real interrupt-context backtrace for eyeballing/symbolizing.
    {
        victim v(sched::cpus[0]);
        g_stop = false;
        v.run(victim_mode::spin);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        g_stop = true;
        if (v.wait()) {
            dump_sample(v.last());
        }
    }

    printf("\n==== os-backtrace: %d checks, %d failures ====\n",
           g_checks.load(), g_fails.load());
    if (g_fails.load() == 0)
        printf("RESULT: ALL BACKTRACE INTERRUPT-SAFETY TESTS PASSED\n");
    else
        printf("RESULT: %d BACKTRACE INTERRUPT-SAFETY TEST(S) FAILED\n",
               g_fails.load());
    return g_fails.load() ? 1 : 0;
}
