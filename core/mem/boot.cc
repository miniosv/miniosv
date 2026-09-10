/*
 * Bringing the memory layers up, in the order they depend on each other.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/prio.hh>
#include <osv/sched.hh>

#include "arch-setup.hh"

std::atomic<unsigned int> smp_allocator_cnt{};
bool smp_allocator = false;

namespace mem {

namespace {

// Hand the boot regions over to llfree. Still single-threaded here, which is
// what that hand-over needs.
struct start_frame_allocator {
    start_frame_allocator() { frames::init(sched::cpus.size()); }
} s_start_frame_allocator __attribute__((init_priority((int)init_prio::frame_allocator)));

// The heap wants a per-cpu bump pointer and a reservation to fill, so it can
// only take over once every cpu is up. Everything before this came from the
// early allocator.
sched::cpu::notifier smp_allocator_notifier([] () {
    if (++smp_allocator_cnt == sched::cpus.size()) {
        frames::enable_percpu();
        heap::init();
        smp_allocator = true;
    }
});

}

void __attribute__((constructor(init_prio::mem))) setup()
{
    arch_setup_free_memory();
}

}
