/*
 * Memory pressure notification.
 *
 * Subsystems that can relinquish memory register a callback here and are notified
 * when free memory falls below a threshold.
 *
 * Callbacks run on the allocation path, so must not block or allocate.
 */

#include <algorithm>
#include <atomic>

#include <osv/kernel_config.h>
#include <osv/mem/frames.hh>

#include "internal.hh"

namespace mem {
namespace frames {

namespace {

std::atomic<pressure_watcher *> watchers;
size_t threshold;
std::atomic<bool> in_callback;

} // namespace

void pressure_init(size_t total_bytes)
{
    threshold = total_bytes / 100 * CONF_memory_pressure_percent;
}

void watch_pressure(pressure_watcher &w, pressure_fn cb, unsigned order)
{
    w.fn = cb;
    w.order = order;
    w.next = watchers.load(std::memory_order_relaxed);
    while (!watchers.compare_exchange_weak(w.next, &w, std::memory_order_release,
                                           std::memory_order_relaxed)) {
    }
}

bool reclaim(size_t bytes)
{
    pressure_watcher *head = watchers.load(std::memory_order_acquire);
    if (!head) {
        return false;
    }
    // One responder at a time: the callbacks free memory, and re-entering from
    // inside one would recurse.
    bool expected = false;
    if (!in_callback.compare_exchange_strong(expected, true)) {
        return false;
    }
    size_t have = free_bytes();
    size_t target = bytes > SIZE_MAX - have ? SIZE_MAX : have + bytes;
    bool gave = false;
    unsigned level = ~0u;
    for (pressure_watcher *w = head; w; w = w->next) {
        level = std::min(level, w->order);
    }
    // Lowest order first, and no further once the request is covered.
    while (level != ~0u && free_bytes() < target) {
        unsigned next = ~0u;
        for (pressure_watcher *w = head; w; w = w->next) {
            if (w->order == level) {
                gave |= w->fn();
            } else if (w->order > level) {
                next = std::min(next, w->order);
            }
        }
        level = next;
    }
    in_callback.store(false);
    return gave;
}

bool under_pressure()
{
    return threshold && free_bytes() < threshold;
}

void check_pressure()
{
    size_t have = free_bytes();
    if (!threshold || have >= threshold) {
        return;
    }
    reclaim(threshold - have);
}

} // namespace frames
} // namespace mem
