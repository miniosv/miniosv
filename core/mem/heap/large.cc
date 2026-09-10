/*
 * Allocations large enough to be worth a reservation of their own.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <new>

#include <algorithm>

#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/vspace.hh>

#include "internal.hh"
#include "../linear.hh"

namespace mem {
namespace heap {

namespace {

// Header for large allocation bookkeeping.
// This header is stored in a separated frame.
struct record {
    vspace::region r;
    size_t bytes;    // what the caller asked for
};

// Tells a large allocation's reservation from every other one.
const vspace::region_ops large_ops = { .fault = nullptr };

record *record_at(void *p)
{
    auto *r = vspace::lookup(reinterpret_cast<uintptr_t>(p));
    if (!r || r->ops != &large_ops || r->span.start != reinterpret_cast<uintptr_t>(p)) {
        return nullptr;
    }
    return reinterpret_cast<record *>(r);
}

} // namespace

void *large_alloc(size_t bytes, size_t alignment)
{
    if (!bytes) {
        return nullptr;
    }
    // Huge leaves once there is enough to fill one.
    size_t leaf = bytes >= large_min ? large_min : mapping::page_size;
    size_t mapped = align_up(bytes, leaf);
    size_t align = std::max(leaf, alignment);

    frames::phys_addr hp = frames::alloc();
    if (hp == frames::no_memory) {
        return nullptr;
    }
    auto *rec = new (frames::to_linear(hp)) record();
    rec->bytes = bytes;
    rec->r.perm = perm_rw;
    rec->r.ops = &large_ops;

    if (vspace::reserve(rec->r, mapped, align) != vspace::resa_result::success) {
        frames::free(hp);
        return nullptr;
    }
    // Not zeroed: malloc does not promise it, and at this size the memset is
    // the whole cost of the call.
    if (!mapping::populate(rec->r.span, perm_rw, leaf, false)) {
        mapping::depopulate(rec->r.span);
        vspace::release(rec->r);
        frames::free(hp);
        return nullptr;
    }
    return reinterpret_cast<void *>(rec->r.span.start);
}

void large_free(void *p)
{
    auto *rec = record_at(p);
    if (!rec) {
        abort("heap: %p was not handed out by large_alloc\n", p);
    }
    // depopulate invalidates before the frames go back, so the addresses this
    // is giving up cannot be reached through a stale translation.
    mapping::depopulate(rec->r.span);
    vspace::release(rec->r);
    frames::free(frames::from_linear(rec));
}

size_t large_size(void *p)
{
    auto *rec = record_at(p);
    return rec ? rec->bytes : 0;
}

bool is_large(void *p)
{
    return record_at(p) != nullptr;
}

}
}
