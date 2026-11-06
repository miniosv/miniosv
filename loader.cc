/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
#include <osv/kernel_config.h>
#include <cctype>
#include <osv/elf.hh>
#include "arch-tls.hh"
#include <osv/debug.hh>
#include <osv/clock.hh>
#include <osv/version.hh>

#include "smp.hh"

#include <osv/sched.hh>
#include <osv/barrier.hh>
#include "arch.hh"
#include "arch-setup.hh"
#include "osv/trace.hh"
#include <osv/strace.hh>
#include <osv/power.hh>
#include <osv/rcu.hh>
#include <osv/mempool.hh>
#include <osv/version.h>
#include <osv/shutdown.hh>
#include <osv/boot.hh>
#include <osv/sampler.hh>
#include <osv/firmware.hh>

#include "drivers/random.hh"
#include "drivers/console.hh"

#include <processor.hh>
#include <dlfcn.h>

using namespace osv;
using namespace osv::clock::literals;

asm(".pushsection \".debug_gdb_scripts\", \"MS\",@progbits,1 \n"
    ".byte 1 \n"
    ".asciz \"scripts/loader.py\" \n"
    ".popsection \n");

elf::Elf64_Ehdr* elf_header __attribute__ ((aligned(8)));

size_t elf_size;
void* elf_start;
elf::tls_data tls_data;

boot_time_chart boot_time;

static void setup_tls(elf::init_table& inittab)
{
    tls_data = inittab.tls;
    sched::init_tls(tls_data);

    extern char tcb0[]; // defined by linker script
    arch_setup_tls(tcb0, tls_data);
}

extern "C" {
    void premain();
    // The statically linked-in application entry point (see app.cc).
    void osv_app_main();
}

void premain()
{
    arch_init_early_console();

    /* besides reporting the OSV version, this string has the function
       to check if the early console really works early enough,
       without depending on prior initialization. */
    debug_early("OSv " OSV_VERSION "\n");

    arch_init_premain();

    extern u64 kernel_vm_shift;
    auto elf_header_virt_address = (char*)elf_header + kernel_vm_shift;

    // Publish the kernel ELF header address for the dynamic-linker
    // introspection used by the C++ exception unwinder (see libc/dlfcn.cc).
    extern void* __kernel_ehdr;
    __kernel_ehdr = elf_header_virt_address;

    auto inittab = elf::get_init(reinterpret_cast<elf::Elf64_Ehdr*>(
        elf_header_virt_address));

    if (inittab.tls.start == nullptr) {
        debug_early("premain: failed to get TLS data from ELF\n");
        arch::halt_no_interrupts();
    }

    setup_tls(inittab);
    boot_time.event(3,"TLS initialization");
    for (auto init = inittab.start; init < inittab.start + inittab.count; ++init) {
        (*init)();
    }
    boot_time.event(".init functions");
}

int main()
{
    smp_initial_find_current_cpu()->init_on_cpu();
    void main_cont();
    sched::init([] { main_cont(); });
}

static bool opt_noshutdown = false;
bool opt_power_off_on_abort = false;
#if CONF_tracepoints
static bool opt_log_backtrace = false;
static bool opt_list_tracepoints = false;
#if CONF_tracepoints_strace
static bool opt_strace = false;
#endif
#endif
static bool opt_random = true;
static std::string opt_console = "all";
static bool opt_bootchart = false;
static std::chrono::nanoseconds boot_delay;
bool opt_maxnic = false;
int maxnic;
bool opt_pci_disabled = false;

#if CONF_tracepoints_sampler && !defined(AARCH64_PORT_STUB)
static int sampler_frequency;
static bool opt_enable_sampler = false;
#endif



void* do_main_thread(void *_main_args)
{
    if (!arch_setup_console(opt_console)) {
        abort("Unknown console:%s\n", opt_console.c_str());
    }
    arch_init_drivers();
    console::console_init();
    if (opt_random) {
        randomdev::randomdev_init();
    }
    boot_time.event("drivers loaded");

    // There is no filesystem: nothing to mount.
    // Networking was removed (Phase 9.2): no interfaces to bring up.

    boot_time.event("Total time");
#ifdef __x86_64__
    // Some hypervisors like firecracker when booting OSv
    // look for this write to this port as a signal of end of
    // boot time.
    processor::outb(123, 0x3f0);
#endif /* __x86_64__ */

    if (opt_bootchart) {
        boot_time.print_chart();
    } else {
        boot_time.print_total_time();
    }

    // Run the deferred constructors (init_priority 100, i.e. libc++'s standard
    // stream initialization) now that the full runtime - allocator, scheduler,
    // console - is up. The linker script brackets them outside the normal
    // init-array (see _init_array_late_{start,end} in arch/$(arch)/loader.ld) so
    // premain() skipped them; libc++ needs cout/cin/cerr ready before the app's
    // entry point.
    extern void (*_init_array_late_start[])();
    extern void (*_init_array_late_end[])();
    for (auto init = _init_array_late_start; init < _init_array_late_end; ++init) {
        (*init)();
    }

    // Enter the statically linked-in application.
    osv_app_main();

    return nullptr;
}

void main_cont()
{
    osv::firmware_probe();

    debugf("Firmware vendor: %s\n", osv::firmware_vendor().c_str());

    setenv("OSV_VERSION", osv::version().c_str(), 1);

#if CONF_drivers_xen
    xen::irq_init();
#endif
    smp_launch();
    setenv("OSV_CPUS", std::to_string(sched::cpus.size()).c_str(), 1);
    boot_time.event("SMP launched");

    auto end = osv::clock::uptime::now() + boot_delay;
    while (end > osv::clock::uptime::now()) {
        // spin
    }

    memory::enable_debug_allocator();

    if (sched::cpus.size() > sched::max_cpus) {
        printf("Too many cpus, can't boot with greater than %u cpus.\n", sched::max_cpus);
        poweroff();
    }

#if CONF_tracepoints
    if (opt_list_tracepoints) {
        list_all_tracepoints();
    }

    enable_trace();
    if (opt_log_backtrace) {
        // can only do this after smp_launch, otherwise the IDT is not initialized,
        // and backtrace_safe() fails as soon as we get an exception
        enable_backtraces();
    }
#if CONF_tracepoints_strace
    if (opt_strace) {
        start_strace();
    }
#endif
#endif
    sched::init_detached_threads_reaper();

    arch::irq_enable();

#ifndef AARCH64_PORT_STUB
#if CONF_tracepoints_sampler
    if (opt_enable_sampler) {
        prof::config config{std::chrono::nanoseconds(1000000000 / sampler_frequency)};
        prof::start_sampler(config);
    }
#endif
#endif /* !AARCH64_PORT_STUB */

    // multiple programs can be run -> separate their arguments

    pthread_t pthread;
    // run the payload in a pthread, so pthread_self() etc. work
    // start do_main_thread unpinned (== pinned to all cpus)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (size_t ii=0; ii<sched::cpus.size(); ii++) {
        CPU_SET(ii, &cpuset);
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
    pthread_create(&pthread, &attr, do_main_thread, nullptr);
    void* retval;
    pthread_join(pthread, &retval);

    if (opt_noshutdown) {
        // If the --noshutdown option is given, continue running the system,
        // and whatever threads might be running, even after main returns
        debug("main() returned.\n");
        sched::thread::wait_until([] { return false; });
    }

        osv::shutdown();
}
