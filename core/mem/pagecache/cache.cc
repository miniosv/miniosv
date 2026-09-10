/*
 * The demand-paged cache: faults, loads, eviction.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <osv/debug.hh>
#include <osv/sched.hh>

#include "processor.hh"

#include "internal.hh"
#include "stats.hh"
#include "../linear.hh"

namespace mem {
namespace pagecache {

mutex caches_mutex;
std::atomic<cache *> caches;
std::atomic<unsigned> walkers;

namespace {

// Loads started before the batch waits for any of them.
constexpr unsigned batch_max = 8;

bool cache_fault(vspace::region &r, uintptr_t addr, unsigned);
const vspace::region_ops cache_ops = { cache_fault };

uint64_t window_bytes(uint64_t store_bytes)
{
    return align_up(store_bytes, uint64_t(page_size));
}

size_t chunk_bytes(const chunk &k)
{
    return size_t(k.pages) * page_size;
}

// Write [off, off + len) of the object out of the frame, clipped to what the
// object has.
int64_t write_piece(cache &c, frames::phys_addr phys, uint64_t off, size_t len)
{
    size_t valid = stored(c, off, len);
    return valid ? c.s->write_now(frames::to_linear(phys), off, valid) : 0;
}

// Fill the frame from the object, zeroing what the object does not have, and
// start the read.
io *start_read(cache &c, frames::phys_addr phys, uint64_t off, size_t len)
{
    void *dst = frames::to_linear(phys);
    size_t valid = stored(c, off, len);
    if (valid < len) {
        memset(static_cast<char *>(dst) + valid, 0, len - valid);
    }
    if (!valid) {
        return nullptr;
    }
    io *r;
    while (!(r = c.s->read(dst, off, valid))) {
        processor::spin_hint();
    }
    return r;
}

/*
 * Cut [start, start + len) into chunks: the two ragged edge pages, then huge
 * frames over every aligned 2 MiB span, small runs either side. With "out"
 * null, just count.
 */
uint32_t plan(uint64_t start, uint64_t len, chunk *out)
{
    uint64_t p0 = page_of(start), p1 = page_of(start + len - 1);
    bool lp = start % page_size != 0, rp = (start + len) % page_size != 0;
    uint32_t n = 0;
    auto emit = [&](uint64_t pid, uint32_t pages, bool leaf, bool edge) {
        if (out) {
            out[n] = chunk{frames::no_memory, nullptr, pid, pages, 0,
                           leaf, edge, false, false};
        }
        n++;
    };
    if (lp) {
        emit(p0, 1, false, true);
    }
    uint64_t lo = lp ? p0 + 1 : p0;
    uint64_t hi = rp ? p1 : p1 + 1;
    if (lo < hi) {
        uint64_t hlo = align_up(lo, uint64_t(huge_pages));
        uint64_t hhi = align_down(hi, uint64_t(huge_pages));
        if (hlo >= hhi) {
            emit(lo, hi - lo, false, false);
        } else {
            if (lo < hlo) {
                emit(lo, hlo - lo, false, false);
            }
            for (uint64_t k = hlo; k < hhi; k += huge_pages) {
                emit(k, huge_pages, true, false);
            }
            if (hhi < hi) {
                emit(hhi, hi - hhi, false, false);
            }
        }
    }
    if (rp && !(lp && p1 == p0)) {
        emit(p1, 1, false, true);
    }
    return n;
}

// Make an entry present with a clean record, keeping the software bits.
void publish_entry(mapping::pte_ref s, unsigned perm)
{
    mapping::pte e = s.read();
    for (;;) {
        mapping::pte want = mapping::pte_with_perm(e, perm);
        want = mapping::pte_set_accessed(want, false);
        want = mapping::pte_set_dirty(want, false);
        if (s.compare_exchange(e, want)) {
            return;
        }
    }
}

void publish_chunk(cache &c, chunk &k)
{
    if (k.adopted) {
        return;
    }
    if (k.leaf) {
        publish_entry(mapping::find(va_of(c, k.pid * page_size)), c.r.perm);
        return;
    }
    for (uint32_t j = 0; j < k.pages; j++) {
        publish_entry(mapping::find(va_of(c, (k.pid + j) * page_size)), c.r.perm);
    }
}

/*
 * Give up one hold on an edge entry. If the other side still holds it, only
 * the caller's bit goes and the page stays mapped. Otherwise present is
 * cleared -- the caller's bit stays until the entry is zeroed, so a refault
 * waits -- and the frame is the caller's once the tlbs have been told.
 */
frames::phys_addr edge_release(cache &c, uintptr_t va, unsigned mine, unsigned other,
                               mapping::pending_invalidation &stale)
{
    auto s = mapping::find(va);
    mapping::pte e = s.read();
    for (;;) {
        if (mapping::pte_sw_bit(e, other)) {
            if (s.compare_exchange(e, mapping::pte_set_sw_bit(e, mine, false))) {
                return frames::no_memory;
            }
        } else {
            if (s.compare_exchange(e, mapping::pte_set_present(e, false))) {
                stale.add(va);
                return mapping::pte_addr(e, 0);
            }
        }
    }
}

// Start the write, if the chunk was written to. After the invalidation
// covering it, when the record is final.
void chunk_write_start(cache &c, chunk &k, const buffer *b)
{
    uintptr_t va = va_of(c, k.pid * page_size);
    bool wet;
    if (b->hooked) {
        wet = b->hook_wet;
    } else {
        wet = !mapping::tracks_writes;
        if (k.leaf) {
            wet = wet || mapping::pte_dirty(mapping::find(va).read());
        } else {
            for (uint32_t j = 0; !wet && j < k.pages; j++) {
                wet = mapping::pte_dirty(mapping::find(va + j * page_size).read());
            }
        }
    }
    if (!wet) {
        return;
    }
    uint64_t off = k.pid * page_size;
    size_t valid = stored(c, off, chunk_bytes(k));
    if (!valid) {
        return;
    }
    if (k.req) {
        c.s->wait(k.req);
        k.req = nullptr;
    }
    while (!(k.req = c.s->write(frames::to_linear(k.phys), off, valid))) {
        processor::spin_hint();
    }
}

// Wait for the write, zero the entries, free the frame.
void chunk_settle(cache &c, chunk &k)
{
    if (k.req) {
        c.s->wait(k.req);
        k.req = nullptr;
    }
    uintptr_t va = va_of(c, k.pid * page_size);
    if (k.leaf) {
        mapping::find(va).write(0);
    } else {
        for (uint32_t j = 0; j < k.pages; j++) {
            mapping::find(va + j * page_size).write(0);
        }
    }
    frames::free(k.phys, chunk_bytes(k));
    c.resident_bytes.fetch_sub(chunk_bytes(k), std::memory_order_relaxed);
}

void finish_chunk(cache &c, chunk &k, const buffer *b)
{
    sched::migrate_disable();
    chunk_write_start(c, k, b);
    chunk_settle(c, k);
    sched::migrate_enable();
}

enum class claim_result { fresh, adopted, taken, nomem };

/*
 * Claim the tile by its first entry. Empty: install the anchor chunk's frame
 * in transit, sw_right set, in one CAS. Present without sw_right: the left
 * neighbour's page; adopting it claims the tile. Anything else is another
 * cpu's tile, arriving or leaving.
 */
// Eviction reads phys, so a frame claimed before the neighbour won must go.
void give_up_claim(chunk &k)
{
    if (k.phys != frames::no_memory) {
        frames::free(k.phys, chunk_bytes(k));
        k.phys = frames::no_memory;
    }
}

claim_result anchor_claim(cache &c, chunk &a)
{
    uintptr_t va = va_of(c, a.pid * page_size);
    for (;;) {
        auto s = mapping::prepare(va, a.leaf ? huge_size : page_size);
        if (!s) {
            return claim_result::nomem;
        }
        mapping::pte e = s.read();
        if (a.leaf && !mapping::pte_empty(e) && !mapping::pte_is_leaf(e, 1)) {
            // An old table stands here; claim a small entry through it.
            a.leaf = false;
            continue;
        }
        if (mapping::pte_sw_bit(e, sw_right)) {
            return claim_result::taken;
        }
        if (mapping::pte_present(e)) {
            if (!a.edge) {
                return claim_result::taken;
            }
            if (s.compare_exchange(e, mapping::pte_set_sw_bit(e, sw_right, true))) {
                give_up_claim(a);
                a.adopted = true;
                return claim_result::adopted;
            }
            continue;
        }
        if (!mapping::pte_empty(e)) {
            return claim_result::taken;
        }
        if (a.phys == frames::no_memory) {
            a.phys = frames::alloc(chunk_bytes(a), a.leaf ? huge_size : page_size);
            if (a.phys == frames::no_memory) {
                return claim_result::nomem;
            }
        }
        mapping::pte want = mapping::pte_set_sw_bit(s.leaf_for(a.phys, 0), sw_right, true);
        mapping::pte have = 0;
        if (s.compare_exchange(have, want)) {
            c.resident_bytes.fetch_add(chunk_bytes(a), std::memory_order_relaxed);
            a.placed = a.leaf ? a.pages : 1;
            return claim_result::fresh;
        }
    }
}

// Take the tail edge, sw_left being this tile's hold on it.
claim_result tail_take(cache &c, chunk &k)
{
    uintptr_t va = va_of(c, k.pid * page_size);
    for (;;) {
        auto s = mapping::prepare(va);
        if (!s) {
            return claim_result::nomem;
        }
        mapping::pte e = s.read();
        if (mapping::pte_present(e) && !mapping::pte_sw_bit(e, sw_left)) {
            if (s.compare_exchange(e, mapping::pte_set_sw_bit(e, sw_left, true))) {
                give_up_claim(k);
                k.adopted = true;
                return claim_result::adopted;
            }
            continue;
        }
        if (!mapping::pte_empty(e)) {
            processor::spin_hint();
            continue;
        }
        if (k.phys == frames::no_memory) {
            k.phys = frames::alloc(page_size, page_size);
            if (k.phys == frames::no_memory) {
                return claim_result::nomem;
            }
        }
        mapping::pte want = mapping::pte_set_sw_bit(s.leaf_for(k.phys, 0), sw_left, true);
        mapping::pte have = 0;
        if (s.compare_exchange(have, want)) {
            c.resident_bytes.fetch_add(page_size, std::memory_order_relaxed);
            k.placed = 1;
            return claim_result::fresh;
        }
    }
}

// Put an interior chunk's entries in transit, spinning out an old instance of
// the tile that is still being cleared. Continues from k.placed.
bool install(cache &c, chunk &k)
{
    if (k.placed == k.pages) {
        return true;
    }
    if (k.leaf) {
        uintptr_t va = va_of(c, k.pid * page_size);
        for (;;) {
            auto s = mapping::prepare(va, huge_size);
            if (!s) {
                return false;
            }
            mapping::pte e = s.read();
            if (mapping::pte_empty(e)) {
                if (s.compare_exchange(e, s.leaf_for(k.phys, 0))) {
                    k.placed = k.pages;
                    return true;
                }
                continue;
            }
            if (!mapping::pte_is_leaf(e, 1)) {
                // An old table stands here; map small entries through it.
                k.leaf = false;
                break;
            }
            processor::spin_hint();
        }
    }
    for (; k.placed < k.pages; k.placed++) {
        uintptr_t va = va_of(c, (k.pid + k.placed) * page_size);
        for (;;) {
            auto s = mapping::prepare(va);
            if (!s) {
                return false;
            }
            mapping::pte have = 0;
            if (s.compare_exchange(have, s.leaf_for(k.phys + k.placed * page_size, 0))) {
                break;
            }
            processor::spin_hint();
        }
    }
    return true;
}

// Undo a load that will not arrive. Own entries were never present and just
// go; an adopted hold is released the way eviction releases one, since the
// neighbour may have left in the meantime.
void load_abort(cache &c, buffer *b)
{
    chunk *ch = chunks_of(b);
    mapping::pending_invalidation stale;
    bool any = false;
    for (uint32_t i = 0; i < b->chunks; i++) {
        chunk &k = ch[i];
        if (!k.adopted) {
            continue;
        }
        uintptr_t va = va_of(c, k.pid * page_size);
        unsigned mine = i == 0 ? sw_right : sw_left;
        unsigned other = i == 0 ? sw_left : sw_right;
        k.phys = edge_release(c, va, mine, other, stale);
        k.taken = k.phys != frames::no_memory;
        any |= k.taken;
    }
    if (any) {
        stale.epoch = mapping::flush_epoch();
        stale.invalidate();
    }
    for (uint32_t i = 0; i < b->chunks; i++) {
        chunk &k = ch[i];
        if (k.adopted) {
            if (k.taken) {
                finish_chunk(c, k, b);
            }
            continue;
        }
        if (k.phys == frames::no_memory) {
            continue;
        }
        if (k.leaf && k.placed) {
            mapping::find(va_of(c, k.pid * page_size)).write(0);
        } else {
            for (uint32_t j = 0; j < k.placed; j++) {
                mapping::find(va_of(c, (k.pid + j) * page_size)).write(0);
            }
        }
        frames::free(k.phys, chunk_bytes(k));
        if (k.placed) {
            c.resident_bytes.fetch_sub(chunk_bytes(k), std::memory_order_relaxed);
        }
    }
    std::free(b);
}

} // namespace

cache *cache_of(vspace::region *r)
{
    return r && r->ops == &cache_ops ? reinterpret_cast<cache *>(r) : nullptr;
}

/* Buffers, as the policy sees them. */

uint64_t offset(const buffer &b) { return b.off; }
size_t size(const buffer &b) { return b.bytes; }
void *data(const buffer &b) { return reinterpret_cast<void *>(va_of(*b.owner, b.off)); }
void *policy_data(const buffer &b) { return const_cast<buffer *>(&b) + 1; }

bool accessed(const buffer &b) { return mapping::accessed(range_of(&b)); }
bool dirty(const buffer &b) { return mapping::dirty(range_of(&b)); }
void clear_accessed(buffer &b) { mapping::clear_accessed(range_of(&b)); }

bool prefetched(const buffer &b) { return b.prefetched; }
void clear_prefetched(buffer &b) { b.prefetched = false; }

/* Reading a buffer in. */

uint64_t tile_of(cache &c, uint64_t off, uint64_t &start)
{
    if (!c.p->fault_extent) {
        start = align_down(off, uint64_t(page_size));
        return std::min<uint64_t>(page_size, c.store_bytes - start);
    }

    uint64_t len = page_size;
    start = off;
    c.p->fault_extent(c.state, off, &start, &len);

    // Clamping the start to the offset
    if (start > off) {
        start = off;
    }
    len = std::min<uint64_t>(std::max<uint64_t>(len, page_size), tile_max);

    if (off - start >= len) {
        len = align_up(off - start + 1, uint64_t(page_size));
        len = std::min<uint64_t>(len, tile_max);
        if (off - start >= len) {
            start = align_down(off, uint64_t(page_size));
            len = page_size;
        }
    }
    return std::min<uint64_t>(len, c.store_bytes - start);
}

load_result load_start(cache &c, uint64_t off, buffer *&out)
{
    out = nullptr;
    uint64_t start;
    uint64_t len = tile_of(c, off, start);
    uint32_t n = plan(start, len, nullptr);

    auto *b = static_cast<buffer *>(
        std::malloc(sizeof(buffer) + c.policy_bytes + n * sizeof(chunk)));
    if (!b) {
        return load_result::failed;
    }
    memset(b, 0, sizeof(buffer) + c.policy_bytes);
    b->owner = &c;
    b->off = start;
    b->bytes = len;
    b->chunks = n;
    chunk *ch = chunks_of(b);
    plan(start, len, ch);

    make_room(c, (last_pid(b) - first_pid(b) + 1) * page_size);

    switch (anchor_claim(c, ch[0])) {
    case claim_result::taken:
        load_abort(c, b);
        return load_result::taken;
    case claim_result::nomem:
        load_abort(c, b);
        return load_result::failed;
    case claim_result::fresh:
        // The claim placed the first entry; the chunk may have more.
        if (!install(c, ch[0])) {
            load_abort(c, b);
            return load_result::failed;
        }
        break;
    case claim_result::adopted:
        break;
    }

    for (uint32_t i = 1; i < n; i++) {
        chunk &k = ch[i];
        if (k.edge) {
            claim_result r = tail_take(c, k);
            if (r == claim_result::nomem) {
                load_abort(c, b);
                return load_result::failed;
            }
            continue;
        }
        k.phys = frames::alloc(chunk_bytes(k), k.leaf ? huge_size : page_size);
        if (k.phys == frames::no_memory) {
            load_abort(c, b);
            return load_result::failed;
        }
        c.resident_bytes.fetch_add(chunk_bytes(k), std::memory_order_relaxed);
        if (!install(c, k)) {
            load_abort(c, b);
            return load_result::failed;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        chunk &k = ch[i];
        if (!k.adopted) {
            k.req = start_read(c, k.phys, k.pid * page_size, chunk_bytes(k));
        }
    }
    out = b;
    return load_result::started;
}

bool load_finish(cache &c, buffer *b)
{
    chunk *ch = chunks_of(b);
    bool ok = true;
    for (uint32_t i = 0; i < b->chunks; i++) {
        if (ch[i].req) {
            ok &= c.s->wait(ch[i].req) >= 0;
            ch[i].req = nullptr;
        }
    }
    if (!ok) {
        load_abort(c, b);
        return false;
    }

    // The anchor last: present there says the whole tile is.
    for (uint32_t i = 1; i < b->chunks; i++) {
        publish_chunk(c, ch[i]);
    }
    publish_chunk(c, ch[0]);
    mapping::barrier();

    if (c.p->on_fault) {
        c.p->on_fault(c.state, *b);
    }
    return true;
}

// Park a started prefetch claim.
int pending_park(cache &c, buffer *b)
{
    for (unsigned i = 0; i < pending_slots; i++) {
        buffer *empty = nullptr;
        if (c.pending[i].compare_exchange_strong(empty, b,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
            // The key last, and with a release: a taker that sees the key has
            // to see a slot already holding the buffer it names.
            c.pending_off[i].store(b->off, std::memory_order_release);
            return int(i);
        }
    }
    return -1;
}

// Take this buffer back out of its slot.
bool pending_cancel(cache &c, unsigned i, buffer *b)
{
    buffer *expected = b;
    if (!c.pending[i].compare_exchange_strong(expected, nullptr,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
        return false;
    }
    c.pending_off[i].store(0, std::memory_order_relaxed);
    return true;
}

//Publish a parked prefetch whose transfers have all landed.
void publish_parked(cache &c, buffer *b)
{
    if (!pending_cancel(c, b->slot, b)) {
        return;
    }
    chunk *ch = chunks_of(b);
    for (uint32_t i = 1; i < b->chunks; i++) {
        publish_chunk(c, ch[i]);
    }
    publish_chunk(c, ch[0]);
    mapping::barrier();

    PAGECACHE_COUNT(completed, 1);
    if (c.p->on_fault) {
        c.p->on_fault(c.state, *b);
    }
}

// One transfer of a parked prefetch has landed.
void prefetch_io_landed(void *arg)
{
    auto *b = static_cast<buffer *>(arg);
    if (b->ios_left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        publish_parked(*b->owner, b);
    }
}

// Claim slot
buffer *pending_claim(cache &c, unsigned i)
{
    buffer *b = c.pending[i].load(std::memory_order_acquire);
    if (!b) {
        return nullptr;
    }
    c.pending_off[i].store(0, std::memory_order_relaxed);
    if (!c.pending[i].compare_exchange_strong(b, nullptr,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
        return nullptr;
    }
    return b;
}

// Take a pending claim
buffer *pending_take(cache &c, uint64_t off, bool any)
{
    uint64_t start;
    tile_of(c, off, start);

    for (unsigned i = 0; i < pending_slots; i++) {
        if (c.pending_off[i].load(std::memory_order_acquire) != start) {
            continue;
        }
        if (buffer *b = pending_claim(c, i)) {
            return b;
        }
    }
    if (!any) {
        return nullptr;
    }
    for (unsigned i = 0; i < pending_slots; i++) {
        if (buffer *b = pending_claim(c, i)) {
            return b;
        }
    }
    return nullptr;
}

// Ask for prefetch candidates and launch IO operations
void prefetch_start(cache &c, buffer &b)
{
    unsigned want = std::min(c.p->prefetch_depth, prefetch_max);
    if (!c.p->prefetch || !want) {
        return;
    }
    uint64_t offs[prefetch_max];
    offset_list also{offs, 0, want};
    c.p->prefetch(c.state, b, also);

    for (unsigned i = 0; i < also.count; i++) {
        if (offs[i] >= c.store_bytes) {
            continue;
        }
        buffer *eb;
        if (load_start(c, offs[i], eb) != load_result::started) {
            continue;
        }
        // Tagged before it is reachable by anyone else: the policy reads this
        // when the buffer finally reaches it, to tell a guess from a demand.
        eb->prefetched = true;
        PAGECACHE_COUNT(prefetched, 1);
        PAGECACHE_COUNT(bytes_in, eb->bytes);

        chunk *ch = chunks_of(eb);
        unsigned nreq = 0;
        for (uint32_t i = 0; i < eb->chunks; i++) {
            if (ch[i].req) {
                nreq++;
            }
        }
        eb->ios_left.store(nreq + 1, std::memory_order_relaxed);
        bool subscribed = true;
        for (uint32_t i = 0; subscribed && i < eb->chunks; i++) {
            if (ch[i].req) {
                subscribed = c.s->subscribe(ch[i].req, prefetch_io_landed, eb);
            }
        }
        if (!subscribed) {
            // A store with no notification: park it for a fault to adopt, the
            // way this worked before stores could announce a completion.
            if (pending_park(c, eb) < 0) {
                load_finish(c, eb);
            }
            continue;
        }
        int slot = pending_park(c, eb);
        if (slot < 0) {
            load_finish(c, eb);
            continue;
        }
        eb->slot = unsigned(slot);
        if (eb->ios_left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            publish_parked(c, eb);
        }
    }
}

bool fault_in(cache &c, uint64_t off)
{
    PAGECACHE_PHASE(fault_ticks);
    spin_watch spin;
    uintptr_t va = va_of(c, align_down(off, uint64_t(page_size)));
    bool counted = false;
    for (;;) {
        auto s = mapping::find(va);
        if (s) {
            if (s.present()) {
                spin.stop();
                if (!counted) {
                    PAGECACHE_COUNT(hits, 1);
                }
                return true;
            }
            // Claimed but not published yet
            if (buffer *p = pending_take(c, off, false)) {
                spin.stop();
                PAGECACHE_PHASE(adopt_ticks);
                PAGECACHE_COUNT(adopted, 1);
                load_finish(c, p);
                continue;
            }
            spin.tick();
            processor::spin_hint();
            continue;
        }
        spin.stop();
        if (!counted) {
            counted = true;
            PAGECACHE_COUNT(faults, 1);
        }
        buffer *b;
        load_result r;
        {
            PAGECACHE_PHASE(claim_ticks);
            r = load_start(c, off, b);
        }
        switch (r) {
        case load_result::started:
            break;
        case load_result::taken:
            spin.tick();
            processor::spin_hint();
            continue;
        case load_result::failed:
            PAGECACHE_COUNT(nomem, 1);
            return false;
        }
        PAGECACHE_COUNT(bytes_in, b->bytes);

        // Submitted before this fault waits for its own read, so the device
        // has the whole batch queued rather than one transfer at a time.
        prefetch_start(c, *b);
        PAGECACHE_PHASE(wait_ticks);
        return load_finish(c, b);
    }
}

/* Giving a buffer up. */

bool evict_take(cache &c, buffer *b, mapping::pending_invalidation &stale)
{
    chunk *ch = chunks_of(b);
    chunk &a = ch[0];
    // A policy with its own idea of dirtiness is asked while still mapped.
    b->hooked = c.p->is_dirty != nullptr;
    if (b->hooked) {
        b->hook_wet = c.p->is_dirty(c.state, *b);
    }
    uintptr_t ava = va_of(c, a.pid * page_size);
    auto s = mapping::find(ava);
    mapping::pte e = s.read();
    for (;;) {
        if (!mapping::pte_present(e) || !mapping::pte_sw_bit(e, sw_right) ||
            mapping::pte_sw_bit(e, sw_busy)) {
            return false;
        }
        if (a.edge && mapping::pte_sw_bit(e, sw_left)) {
            if (s.compare_exchange(e, mapping::pte_set_sw_bit(e, sw_right, false))) {
                a.taken = false;
                break;
            }
        } else {
            if (s.compare_exchange(e, mapping::pte_set_present(e, false))) {
                a.taken = true;
                if (a.phys == frames::no_memory) {
                    a.phys = mapping::pte_addr(e, a.leaf ? 1 : 0);
                }
                stale.add(ava);
                break;
            }
        }
    }

    for (uint32_t i = 1; i < b->chunks; i++) {
        chunk &k = ch[i];
        uintptr_t va = va_of(c, k.pid * page_size);
        if (k.edge) {
            frames::phys_addr p = edge_release(c, va, sw_left, sw_right, stale);
            k.taken = p != frames::no_memory;
            if (k.taken) {
                k.phys = p;
            }
            continue;
        }
        k.taken = true;
        if (k.leaf) {
            auto se = mapping::find(va);
            mapping::pte ee = se.read();
            while (!se.compare_exchange(ee, mapping::pte_set_present(ee, false))) {
            }
            stale.add(va);
            continue;
        }
        for (uint32_t j = 0; j < k.pages; j++) {
            auto se = mapping::find(va + j * page_size);
            mapping::pte ee = se.read();
            while (!se.compare_exchange(ee, mapping::pte_set_present(ee, false))) {
            }
            stale.add(va + j * page_size);
        }
    }
    stale.epoch = mapping::flush_epoch();
    return true;
}

void evict_write_start(cache &c, buffer *b)
{
    chunk *ch = chunks_of(b);
    for (uint32_t i = 0; i < b->chunks; i++) {
        if (ch[i].taken) {
            chunk_write_start(c, ch[i], b);
        }
    }
}

void evict_settle(cache &c, buffer *b)
{
    chunk *ch = chunks_of(b);
    // The anchor last, so a refault waits until everything else is clean.
    for (uint32_t i = b->chunks; i-- > 0; ) {
        if (ch[i].taken) {
            chunk_settle(c, ch[i]);
        }
    }
    if (c.p->on_evicted) {
        c.p->on_evicted(c.state, *b);
    }
    std::free(b);
}

/* The interface. */

void *map(store &s, const policy &p, size_t limit)
{
    uint64_t bytes = s.size();
    if (!bytes || s.granularity() > page_size) {
        return nullptr;
    }
    auto *c = new cache();
    c->s = &s;
    c->p = &p;
    c->store_bytes = bytes;
    c->policy_bytes = align_up(size_t(p.bytes_per_buffer), sizeof(void *));
    c->limit = limit;
    c->r.ops = &cache_ops;
    c->r.perm = perm_rw;

    if (vspace::reserve(c->r, window_bytes(bytes), huge_size) !=
        vspace::resa_result::success) {
        delete c;
        return nullptr;
    }
    c->state = p.create ? p.create(bytes) : nullptr;

    WITH_LOCK(caches_mutex) {
        c->next.store(caches.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
        caches.store(c, std::memory_order_release);
    }
    watch_start();
    return reinterpret_cast<void *>(c->r.span.start);
}

// Everything goes back through the policy, which is the only holder of the
// buffers and so the only way to free them.
void unmap(void *addr)
{
    cache *c = cache_of(vspace::lookup(reinterpret_cast<uintptr_t>(addr)));
    if (!c) {
        return;
    }
    WITH_LOCK(caches_mutex) {
        std::atomic<cache *> *p = &caches;
        for (cache *i = p->load(std::memory_order_relaxed); i;
             p = &i->next, i = p->load(std::memory_order_relaxed)) {
            if (i == c) {
                p->store(c->next.load(std::memory_order_relaxed),
                         std::memory_order_release);
                break;
            }
        }
    }
    // An evictor that saw the cache before the unlink may still be in it.
    quiesce_manager();

    // Parked prefetches first.
    while (buffer *p = pending_take(*c, 0, true)) {
        load_finish(*c, p);
    }

    while (c->resident_bytes.load(std::memory_order_relaxed)) {
        if (!evict_bytes(*c, SIZE_MAX / 2)) {
            abort("pagecache: the policy stranded %zu bytes\n",
                  c->resident_bytes.load());
        }
    }

    mapping::detach(c->r.span);
    if (c->p->destroy) {
        c->p->destroy(c->state);
    }
    vspace::release(c->r);
    delete c;
}

int64_t sync(void *addr, size_t bytes)
{
    auto start = reinterpret_cast<uintptr_t>(addr);
    cache *c = cache_of(vspace::lookup(start));
    if (!c) {
        return -EINVAL;
    }
    uint64_t off = start - c->r.span.start;
    uint64_t end = std::min<uint64_t>(off + bytes, c->store_bytes);

    int64_t total = 0;
    mapping::pending_invalidation stale;
    while (off < end) {
        uint64_t tstart;
        uint64_t tlen = tile_of(*c, off, tstart);
        off = tstart + tlen;

        auto s = mapping::find(va_of(*c, page_of(tstart) * page_size));
        if (!s) {
            continue;
        }
        mapping::pte e = s.read();
        bool held = false;
        while (mapping::pte_present(e) && mapping::pte_sw_bit(e, sw_right) &&
               !mapping::pte_sw_bit(e, sw_busy)) {
            if (s.compare_exchange(e, mapping::pte_set_sw_bit(e, sw_busy, true))) {
                held = true;
                break;
            }
        }
        if (!held) {
            continue;
        }

        uint64_t p0 = page_of(tstart), p1 = page_of(tstart + tlen - 1);
        range v = {va_of(*c, p0 * page_size), va_of(*c, (p1 + 1) * page_size)};
        if (mapping::dirty(v)) {
            // Contiguous frames go back in one transfer each.
            frames::phys_addr rphys = frames::no_memory;
            uint64_t roff = 0;
            size_t rlen = 0;
            int64_t err = 0;
            for (uint64_t p = p0; p <= p1; ) {
                auto se = mapping::find(va_of(*c, p * page_size));
                size_t sz = se.size();
                frames::phys_addr ph = se.addr();
                if (rlen && rphys + rlen == ph) {
                    rlen += sz;
                } else {
                    if (rlen) {
                        int64_t r = write_piece(*c, rphys, roff, rlen);
                        if (r < 0) { err = r; break; }
                        total += r;
                    }
                    rphys = ph;
                    roff = p * page_size;
                    rlen = sz;
                }
                p += sz / page_size;
            }
            if (!err && rlen) {
                int64_t r = write_piece(*c, rphys, roff, rlen);
                if (r < 0) { err = r; } else { total += r; }
            }
            if (!err) {
                mapping::clear_dirty(v, stale);
            }
            e = s.read();
            while (!s.compare_exchange(e, mapping::pte_set_sw_bit(e, sw_busy, false))) {
            }
            if (err) {
                return err;
            }
            continue;
        }
        e = s.read();
        while (!s.compare_exchange(e, mapping::pte_set_sw_bit(e, sw_busy, false))) {
        }
    }
    stale.invalidate();
    return total;
}

bool resident(void *addr)
{
    auto a = reinterpret_cast<uintptr_t>(addr);
    cache *c = cache_of(vspace::lookup(a));
    if (!c) {
        return false;
    }
    auto s = mapping::find(a);
    return s && s.present();
}

size_t fetch(void *addr, size_t bytes)
{
    auto start = reinterpret_cast<uintptr_t>(addr);
    cache *c = cache_of(vspace::lookup(start));
    if (!c) {
        return 0;
    }
    uint64_t off = start - c->r.span.start;
    uint64_t end = std::min<uint64_t>(off + bytes, window_bytes(c->store_bytes));
    if (off >= end) {
        return 0;
    }

    // Install anything a prefetch parked before looking at the entries
    while (buffer *p = pending_take(*c, 0, true)) {
        load_finish(*c, p);
    }

    // A transfer is waited for on the cpu that started it.
    sched::migrate_disable();
    buffer *batch[batch_max];
    unsigned held = 0;
    for (uint64_t p = page_of(off); p * page_size < end; ) {
        auto s = mapping::find(va_of(*c, p * page_size));
        if (s && !s.empty()) {
            p++;
            continue;
        }
        bool done = false;
        uint64_t byte = std::min(p * page_size, c->store_bytes - 1);
        switch (load_start(*c, byte, batch[held])) {
        case load_result::started:
            p = last_pid(batch[held]) + 1;
            held++;
            break;
        case load_result::taken:
            p++;
            break;
        case load_result::failed:
            done = true;
            break;
        }
        if (held == batch_max || done || p * page_size >= end) {
            for (unsigned j = 0; j < held; j++) {
                load_finish(*c, batch[j]);
            }
            held = 0;
            if (done) {
                break;
            }
        }
    }
    sched::migrate_enable();

    size_t have = 0;
    for (uint64_t p = page_of(off); p * page_size < end; p++) {
        auto s = mapping::find(va_of(*c, p * page_size));
        if (s && s.present()) {
            have += std::min(end, (p + 1) * page_size) - std::max(off, p * page_size);
        }
    }
    return have;
}

namespace {

bool cache_fault(vspace::region &r, uintptr_t addr, unsigned)
{
    cache *c = cache_of(&r);
    if (!c) {
        return false;
    }
    uint64_t off = addr - r.span.start;
    if (off >= window_bytes(c->store_bytes)) {
        return false;
    }
    return fault_in(*c, std::min<uint64_t>(off, c->store_bytes - 1));
}

}

}
}
