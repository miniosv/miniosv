/*
 * Eviction over multiple cache regions
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/sched.hh>

#include "processor.hh"

#include "internal.hh"
#include "stats.hh"

namespace mem {
namespace pagecache {

namespace {

// Victims one call to the policy may name, and what one pass asks for.
constexpr unsigned victims_max = 64;
constexpr size_t pass_bytes = 16ul << 20;

frames::pressure_watcher pressure;
bool watching;

// One proportional pass over every cache.
size_t manager_pass(size_t want)
{
    walkers.fetch_add(1, std::memory_order_seq_cst);
    size_t total = 0;
    for (cache *c = caches.load(std::memory_order_acquire); c;
         c = c->next.load(std::memory_order_acquire)) {
        total += c->resident_bytes.load(std::memory_order_relaxed);
    }
    size_t freed = 0;
    if (total) {
        for (cache *c = caches.load(std::memory_order_acquire); c;
             c = c->next.load(std::memory_order_acquire)) {
            size_t held = c->resident_bytes.load(std::memory_order_relaxed);
            size_t share = size_t((unsigned __int128)want * held / total);
            if (share) {
                freed += evict_bytes(*c, share);
            }
        }
    }
    walkers.fetch_sub(1, std::memory_order_release);
    return freed;
}

bool give_back()
{
    return manager_pass(pass_bytes) != 0;
}

} // namespace

size_t evict_bytes(cache &c, size_t bytes)
{
    if (!c.p->evict) {
        return 0;
    }
    buffer *v[victims_max];
    buffer_list victims{v, 0, victims_max};
    c.p->evict(c.state, bytes, victims);

    size_t before = c.resident_bytes.load(std::memory_order_relaxed);
    mapping::pending_invalidation stale;
    buffer *taken[victims_max];
    unsigned n = 0;
    for (unsigned i = 0; i < victims.count; i++) {
        if (evict_take(c, v[i], stale)) {
            taken[n++] = v[i];
        } else if (c.p->on_fault) {
            // Held by a writer; hand it back the way it came.
            c.p->on_fault(c.state, *v[i]);
        }
    }
    if (!n) {
        return 0;
    }

    stale.invalidate();
    PAGECACHE_COUNT(evicted, n);
    // Every write goes out before the first is waited for.
    sched::migrate_disable();
    for (unsigned i = 0; i < n; i++) {
        evict_write_start(c, taken[i]);
    }
    for (unsigned i = 0; i < n; i++) {
        evict_settle(c, taken[i]);
    }
    sched::migrate_enable();
    const size_t freed = before - c.resident_bytes.load(std::memory_order_relaxed);
    PAGECACHE_COUNT(evicted_bytes, freed);
    return freed;
}

void make_room(cache &c, size_t bytes)
{
    PAGECACHE_PHASE(room_ticks);
    if (frames::under_pressure()) {
        manager_pass(pass_bytes);
    }
    while (c.limit) {
        size_t held = c.resident_bytes.load(std::memory_order_relaxed);
        if (held + bytes <= c.limit) {
            break;
        }
        if (evict_bytes(c, held + bytes - c.limit)) {
            continue;
        }
        
        buffer *p = pending_take(c, 0, true);
        if (!p) {
            break;
        }
        load_finish(c, p);
    }
}

void quiesce_manager()
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    while (walkers.load(std::memory_order_acquire)) {
        processor::spin_hint();
    }
}

void watch_start()
{
    WITH_LOCK(caches_mutex) {
        if (!watching) {
            watching = true;
            frames::watch_pressure(pressure, give_back, frames::pressure_cache);
        }
    }
}

}
}
