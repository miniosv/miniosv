/*
 * The synchronous forms of a backend's transfers.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mem/store.hh>
#include <osv/sched.hh>
#include "processor.hh"

namespace mem {

// A null answer is a full queue and not a failure, so there is nothing to do
// but let whoever is draining it get on with it.
int64_t store::read_now(void *buf, uint64_t offset, size_t bytes)
{
    sched::migrate_disable();
    io *req;
    while (!(req = read(buf, offset, bytes))) {
        processor::spin_hint();
    }
    int64_t r = wait(req);
    sched::migrate_enable();
    return r;
}

int64_t store::write_now(const void *buf, uint64_t offset, size_t bytes)
{
    sched::migrate_disable();
    io *req;
    while (!(req = write(buf, offset, bytes))) {
        processor::spin_hint();
    }
    int64_t r = wait(req);
    sched::migrate_enable();
    return r;
}

}
