/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <sys/mman.h>
#include <osv/align.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/vspace.hh>
#include <memory>
#include <new>
#include <osv/debug.hh>
#include "osv/trace.hh"
#include <osv/stubbing.hh>
#include "libc/libc.hh"
#include <safe-ptr.hh>
#include <atomic>
#include <osv/kernel_config.h>

#ifndef MAP_UNINITIALIZED
#define MAP_UNINITIALIZED 0x4000000
#endif

TRACEPOINT(trace_memory_mmap, "addr=%p, length=%d, prot=%d, flags=%d, fd=%d, offset=%d", void *, size_t, int, int, int, off_t);
TRACEPOINT(trace_memory_mmap_err, "%d", int);
TRACEPOINT(trace_memory_mmap_ret, "%p", void *);
TRACEPOINT(trace_memory_munmap, "addr=%p, length=%d", void *, size_t);
TRACEPOINT(trace_memory_munmap_err, "%d", int);
TRACEPOINT(trace_memory_munmap_ret, "");

unsigned libc_prot_to_perm(int prot)
{
    unsigned perm = 0;
    if (prot & PROT_READ) {
        perm |= mem::perm_read;
    }
    if (prot & PROT_WRITE) {
        perm |= mem::perm_write;
    }
    if (prot & PROT_EXEC) {
        perm |= mem::perm_exec;
    }
    return perm;
}

static bool page_aligned(const void *p)
{
    return !(reinterpret_cast<uintptr_t>(p) & (mem::mapping::page_size - 1));
}

// Anonymous memory: a reservation of its own, mapped eagerly. The ops mark
// tells these apart from every other reservation.
static const mem::vspace::region_ops anon_ops = { .fault = nullptr };

static mem::vspace::region *anon_at(const void *addr)
{
    auto *r = mem::vspace::lookup(reinterpret_cast<uintptr_t>(addr));
    return r && r->ops == &anon_ops ? r : nullptr;
}

static void *anon_map(size_t length, unsigned perm)
{
    // Huge leaves once there is enough to fill one.
    size_t leaf = length >= mem::mapping::huge_page_size ?
                  mem::mapping::huge_page_size : mem::mapping::page_size;
    auto *r = new (std::nothrow) mem::vspace::region();
    if (!r) {
        return nullptr;
    }
    r->perm = perm;
    r->ops = &anon_ops;
    if (mem::vspace::reserve(*r, align_up(length, leaf), leaf) !=
        mem::vspace::resa_result::success) {
        delete r;
        return nullptr;
    }
    if (!mem::mapping::populate(r->span, perm, leaf)) {
        mem::mapping::depopulate(r->span);
        mem::vspace::release(*r);
        delete r;
        return nullptr;
    }
    return reinterpret_cast<void *>(r->span.start);
}

// depopulate invalidates before the frames go back, so the addresses this is
// giving up cannot be reached through a stale translation.
static void anon_unmap(mem::vspace::region *r)
{
    mem::mapping::depopulate(r->span);
    mem::vspace::release(*r);
    delete r;
}

OSV_LIBC_API
int mprotect(void *addr, size_t len, int prot)
{
    if (!page_aligned(addr)) {
        return libc_error(EINVAL);
    }

    // Only a mapping this made can be reprotected: anything else shares its
    // pages with the allocation next to it.
    len = align_up(len, mem::mapping::page_size);
    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    auto *r = anon_at(addr);
    if (!r || !r->span.contains({start, start + len})) {
        return libc_error(ENOMEM);
    }
    mem::mapping::protect({start, start + len}, libc_prot_to_perm(prot));
    return 0;
}

int mmap_validate(void *addr, size_t length, int flags, off_t offset)
{
    int type = flags & (MAP_SHARED|MAP_PRIVATE);
    // Either MAP_SHARED or MAP_PRIVATE must be set, but not both.
    if (!type || type == (MAP_SHARED|MAP_PRIVATE)) {
        return EINVAL;
    }
    if ((flags & MAP_FIXED && !page_aligned(addr)) ||
        !page_aligned(reinterpret_cast<void *>(offset)) || length == 0) {
        return EINVAL;
    }
    return 0;
}

OSV_LIBC_API
void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset)
{
    trace_memory_mmap(addr, length, prot, flags, fd, offset);

    int err = mmap_validate(addr, length, flags, offset);
    if (err) {
        errno = err;
        trace_memory_mmap_err(err);
        return MAP_FAILED;
    }

    void *ret;

    auto mmap_perm = libc_prot_to_perm(prot);

    // There is no filesystem, so only anonymous mappings are supported;
    // file-backed mmap is not available.
    if (!(flags & MAP_ANONYMOUS)) {
        errno = ENODEV;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    // MAP_FIXED has no answer here, since the address space manager picks
    // addresses.
    if (flags & MAP_FIXED) {
        errno = ENOTSUP;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    ret = anon_map(length, mmap_perm);
    if (!ret) {
        errno = ENOMEM;
        trace_memory_mmap_err(errno);
        return MAP_FAILED;
    }
    trace_memory_mmap_ret(ret);
    return ret;
}

int munmap_validate(void *addr, size_t length)
{
    if (!page_aligned(addr) || length == 0) {
        return EINVAL;
    }
    return 0;
}

OSV_LIBC_API
int munmap(void *addr, size_t length)
{
    trace_memory_munmap(addr, length);
    int error = munmap_validate(addr, length);
    if (error) {
        errno = error;
        trace_memory_munmap_err(error);
        return -1;
    }
    int ret = 0;
    // A mapping is given back whole, at the address mmap() returned.
    auto *r = anon_at(addr);
    if (r && r->span.start == reinterpret_cast<uintptr_t>(addr)) {
        anon_unmap(r);
    } else {
        errno = EINVAL;
        ret = -1;
        trace_memory_munmap_err(errno);
    }
    trace_memory_munmap_ret();
    return ret;
}

// Anonymous memory has no backing store, so this only reports whether the
// range is there at all.
OSV_LIBC_API
int msync(void *addr, size_t length, int flags)
{
    if (!anon_at(addr)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

// Nothing is given back: the frames under a mapping belong to it until it is
// unmapped. MADV_DONTNEED is accepted and does nothing.
OSV_LIBC_API
int madvise(void *addr, size_t length, int advice)
{
    if (!anon_at(addr)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

// brk/sbrk are not supported: nothing asks for them, and a program break wants
// a lazily backed region, which anonymous memory here is not.
OSV_LIBC_API
int brk(void *)
{
    errno = ENOMEM;
    return -1;
}

OSV_LIBC_API
void *sbrk(intptr_t)
{
    errno = ENOMEM;
    return (void *)-1;
}

OSV_LIBC_API
int posix_madvise(void *addr, size_t len, int advice) {
    return anon_at(addr) ? 0 : ENOMEM;
}
