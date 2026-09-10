#include <osv/shutdown.hh>
#include <osv/power.hh>
#include <osv/debug.hh>
#include <osv/sched.hh>
#include <osv/strace.hh>
#include <osv/kernel_config.h>
#include <osv/mem/pagecache.hh>

namespace osv {

void shutdown()
{
#if CONF_tracepoints_strace
    wait_strace_complete();
#endif

    // Stop all other application threads before powering off, so they cannot
    // run partially-torn-down code during shutdown.
    bool stopped_others = false;
    auto current = sched::thread::current();
    while (!stopped_others) {
        stopped_others = true;
        sched::with_all_threads([&](sched::thread &t) {
            if (&t != current && t.is_app()) {
                stopped_others &= t.unsafe_stop();
            }
        });
    }

    mem::pagecache::stats_dump();

    debug("Powering off.\n");
    osv::poweroff();
}

}
