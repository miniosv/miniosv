/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef PRIO_HH_
#define  PRIO_HH_

namespace init_prio {
enum {
    dtb = 101,
    console,
    sort,
    cpus,
    fpranges,
    pt_root,
    mem,
    threadlist,
    pthread,
    notifiers,
    acpi,
    psci,
    gic,
    reclaimer,
    sched,
    clock,
    tracepoint_base,
    frame_allocator,
    idt,
};
}

#endif
