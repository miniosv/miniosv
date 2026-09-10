/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * The C allocation interface, over mem::heap.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mem/early.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/heap.hh>
#include <osv/mem/mapping.hh>
#include <osv/ilog2.hh>
#include <cassert>
#include <cstdint>
#include <new>
#include <string.h>
#include "libc/libc.hh"
#include <osv/align.hh>
#include <osv/debug.hh>
#include <osv/kernel_config.h>
#include <atomic>
#include <osv/trace.hh>
#include <osv/sched.hh>
#include <algorithm>
#include <stdlib.h>
#include <osv/export.h>

#include "core/mem/linear.hh"
#include "core/mem/heap/histogram.hh"

TRACEPOINT(trace_memory_malloc, "buf=%p, len=%d, align=%d", void *, size_t,
           size_t);
TRACEPOINT(trace_memory_malloc_heap, "buf=%p, req_len=%d, alloc_len=%d,"
           " align=%d", void*, size_t, size_t, size_t);
TRACEPOINT(trace_memory_free, "buf=%p", void *);
TRACEPOINT(trace_memory_realloc, "in=%p, newlen=%d, out=%p", void *, size_t, void *);

extern bool smp_allocator;

// C linkage for the definitions below; malloc_usable_size has no declaration
// in any header this includes.
extern "C" {
    void* malloc(size_t size);
    void free(void* object);
    size_t malloc_usable_size(void *object);
}

static inline void* std_malloc(size_t size, size_t alignment)
{
    if ((ssize_t)size < 0)
        return libc_error_ptr<void *>(ENOMEM);
    void *ret;
    if (mem::heap::ready() && mem::heap::takes(size, alignment)) {
        ret = mem::heap::alloc(size, alignment);
        trace_memory_malloc_heap(ret, size, ret ? mem::heap::size_of(ret) : 0,
                                 alignment);
    } else {
        ret = mem::early::alloc(size, alignment);
    }
    mem::heap::hist_alloc(size);
    return ret;
}

void* calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return malloc(0);
    if (nmemb > std::numeric_limits<size_t>::max() / size)
        return nullptr;
    auto n = nmemb * size;
    auto p = malloc(n);
    if (!p)
        return nullptr;
    memset(p, 0, n);
    return p;
}

static size_t object_size(void *object)
{
    if (mem::heap::owns(object)) {
        return mem::heap::size_of(object);
    }
    // Anything else came from before the heap existed.
    return mem::early::size_of(object);
}

static inline void* std_realloc(void* object, size_t size)
{
    if (!object)
        return malloc(size);
    if (!size) {
        free(object);
        return nullptr;
    }

    size_t old_size = object_size(object);
    size_t copy_size = size > old_size ? old_size : size;
    void* ptr = malloc(size);
    if (ptr) {
        memcpy(ptr, object, copy_size);
        free(object);
    }

    return ptr;
}

// Everything free() does before it decides who the object goes back to.
// False if there is nothing to give back.
static inline bool free_bookkeeping(void *object)
{
    trace_memory_free(object);
    if (!object) {
        return false;
    }
    mem::heap::hist_freed();
    return true;
}


void free(void* object)
{
    if (!free_bookkeeping(object)) {
        return;
    }
    if (mem::heap::owns(object)) {
        return mem::heap::free(object);
    }
    // Anything else came from before the heap existed.
    mem::early::free(object);
}

// The same with the size the caller kept, which is what operator delete has.
static inline void free_sized(void *object, size_t bytes)
{
    if (!free_bookkeeping(object)) {
        return;
    }
    mem::heap::hist_freed_sized(bytes);
    if (mem::heap::owns(object)) {
        return mem::heap::free(object, bytes);
    }
    mem::early::free(object);
}

void* malloc(size_t size)
{
    static_assert(alignof(max_align_t) >= 2 * sizeof(size_t),
                  "alignof(max_align_t) smaller than glibc alignment guarantee");
    auto alignment = alignof(max_align_t);
    if (alignment > size) {
        alignment = 1ul << ilog2_roundup(size);
    }
    void* buf = std_malloc(size, alignment);

    trace_memory_malloc(buf, size, alignment);
    return buf;
}

OSV_LIBC_API
void* realloc(void* obj, size_t size)
{
    void* buf = std_realloc(obj, size);
    trace_memory_realloc(obj, size, buf);
    return buf;
}

extern "C" OSV_LIBC_API
void *reallocarray(void *ptr, size_t nmemb, size_t elem_size)
{
    size_t bytes;
    if (__builtin_mul_overflow(nmemb, elem_size, &bytes)) {
        errno = ENOMEM;
        return 0;
    }
    return realloc(ptr, nmemb * elem_size);
}

OSV_LIBC_API
size_t malloc_usable_size(void* obj)
{
    if ( obj == nullptr ) {
        return 0;
    }
    return object_size(obj);
}

// posix_memalign() and C11's aligned_alloc() return an aligned memory block
// that can be freed with an ordinary free().

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    // posix_memalign() but not aligned_alloc() adds an additional requirement
    // that alignment is a multiple of sizeof(void*). We don't verify this
    // requirement, and rather always return memory which is aligned at least
    // to sizeof(void*), even if lesser alignment is requested.
    if (!is_power_of_two(alignment)) {
        return EINVAL;
    }
    void* ret = std_malloc(size, alignment);
    trace_memory_malloc(ret, size, alignment);
    if (!ret) {
        return ENOMEM;
    }
    // Until we have a full implementation, just croak if we didn't get
    // the desired alignment.
    assert (!(reinterpret_cast<uintptr_t>(ret) & (alignment - 1)));
    *memptr = ret;
    return 0;

}

void *aligned_alloc(size_t alignment, size_t size)
{
    void *ret;
    int error = posix_memalign(&ret, alignment, size);
    if (error) {
        errno = error;
        return NULL;
    }
    return ret;
}

// memalign() is an older variant of aligned_alloc(), which does not require
// that size be a multiple of alignment.
// memalign() is considered to be an obsolete SunOS-ism, but Linux's glibc
// supports it, and some applications still use it.
OSV_LIBC_API
void *memalign(size_t alignment, size_t size)
{
    return aligned_alloc(alignment, size);
}

/*
 * The sized forms of operator delete. libc++ defines these weakly as a plain
 * free(), which drops the one thing the compiler went to the trouble of
 * supplying: the compiler emits the size at every delete of a known type, and
 * the histogram says that is 99.6% of the frees two real applications make.
 * Defining them here keeps it and hands it to the heap.
 */
#include <new>

void operator delete(void *p, size_t n) noexcept
{
    free_sized(p, n);
}

void operator delete[](void *p, size_t n) noexcept
{
    free_sized(p, n);
}

void operator delete(void *p, size_t n, std::align_val_t) noexcept
{
    free_sized(p, n);
}

void operator delete[](void *p, size_t n, std::align_val_t) noexcept
{
    free_sized(p, n);
}
