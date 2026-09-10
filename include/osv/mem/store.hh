/*
 * Generic interface to a backing store for the page cache
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_STORE_HH
#define OSV_MEM_STORE_HH

#include <cstddef>
#include <cstdint>

namespace mem {

// One transfer in flight.
struct io;

struct store {
    virtual ~store() = default;

    virtual uint64_t size() = 0;
    virtual size_t granularity() { return 4096; }

    // async IO operations
    virtual io *read(void *buf, uint64_t offset, size_t bytes) = 0;
    virtual io *write(const void *buf, uint64_t offset, size_t bytes) = 0;
    virtual bool done(io *req) = 0;
    virtual int64_t wait(io *req) = 0;

    virtual bool subscribe(io *, void (*)(void *), void *) { return false; }

    // Sync IO operations
    int64_t read_now(void *buf, uint64_t offset, size_t bytes);
    int64_t write_now(const void *buf, uint64_t offset, size_t bytes);
};

}

#endif /* OSV_MEM_STORE_HH */
