/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include "libc.hh"
#include "drivers/console.hh"
#include <osv/sched.hh>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <type_traits>
#include <limits>
#include <sys/resource.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <osv/debug.h>
#include <sched.h>
#include <termios.h>
#include <unistd.h>
#include <osv/clock.hh>
#include <osv/mempool.hh>
#include <osv/version.h>
#include <osv/stubbing.hh>

// FIXME: If we ever support multiple different executables we will have to maybe put those
// on a shared library
char *program_invocation_name;
char *program_invocation_short_name;

weak_alias(program_invocation_name, __progname_full);
weak_alias(program_invocation_short_name, __progname);

int libc_error(int err)
{
    errno = err;
    return -1;
}

#undef errno

int __thread errno;

int* __errno_location()
{
    return &errno;
}

extern "C" int* ___errno_location()
{
    return &errno;
}

// llvm-libc in external-errno mode (LIBC_CONF_ERRNO_MODE_EXTERNAL, the
// baremetal default) routes all its errno accesses through this hook, so
// llvm-libc entrypoints share OSv's TLS errno.
extern "C" int* __llvm_libc_errno() noexcept
{
    return &errno;
}

int getrlimit(int resource, struct rlimit *rlim)
{
    auto set = [=] (rlim_t r) { rlim->rlim_cur = rlim->rlim_max = r; };
    switch (resource) {
    case RLIMIT_STACK: {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        size_t stacksize;
        pthread_attr_getstacksize(&attr, &stacksize);
        set(stacksize);
        pthread_attr_destroy(&attr);
        break;
    }
    case RLIMIT_NOFILE:
        set(1024*10); // FIXME: larger?
        break;
    case RLIMIT_CORE:
        set(RLIM_INFINITY);
        break;
    case RLIMIT_NPROC:
        set(RLIM_INFINITY);
        break;
    case RLIMIT_AS:
    case RLIMIT_DATA:
    case RLIMIT_RSS:
        set(RLIM_INFINITY);
        break;
    case RLIMIT_CPU:
    case RLIMIT_FSIZE:
    case RLIMIT_MEMLOCK:
    case RLIMIT_MSGQUEUE:
        set(RLIM_INFINITY);
        break;
    case RLIMIT_NICE:
        set(0);
        break;
    default:
        abort("getrlimit(): resource %d not supported. aborting.\n", resource);
    }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    // osv - no limits
    return 0;
}

int prlimit(pid_t pid, int resource, const struct rlimit *new_limit, struct rlimit *old_limit)
{
    if (pid != getpid() && pid != 0) {
        errno = EINVAL;
        return -1;
    }

    if (old_limit && getrlimit(resource, old_limit)) {
        return -1;
    }

    if (new_limit && setrlimit(resource, new_limit)) {
        return -1;
    }

    return 0;
}
LFS64(getrlimit);
LFS64(setrlimit);
#undef prlimit64
LFS64(prlimit);

uid_t geteuid()
{
    return 0;
}

int sched_yield()
{
    sched::thread::yield();
    return 0;
}

extern "C" int sched_getcpu()
{
    return sched::cpu::current()->id;
}

extern "C"
int getloadavg(double loadavg[], int nelem)
{
    int i;

    for (i = 0; i < nelem; i++)
        loadavg[i] = 0.5;

    return 0;
}

extern "C" int sysinfo(struct sysinfo *info)
{
    info->uptime = std::chrono::duration_cast<std::chrono::seconds>(
                osv::clock::uptime::now().time_since_epoch()).count();
    info->loads[0] = info->loads[1] = info->loads[2] = 0; // TODO
    info->totalram = memory::stats::total();
    info->freeram = memory::stats::free();
    info->sharedram = 0; // TODO: anything more meaningful to return?
    info->bufferram = 0; // TODO: anything more meaningful to return?
    info->totalswap = 0; // Swap not supported in OSv
    info->freeswap = 0; // Swap not supported in OSv
    info->procs = 1; // Only one process in OSv
    info->totalhigh = info->freehigh = 0; // No such concept in OSv
    info->mem_unit = 1;
    return 0;
}

// The terminal calls talk to the console directly.
// Only the standard streams are terminals here; there is no fd table and
// nothing else to be a tty.
static bool is_std_fd(int fd)
{
    return fd >= 0 && fd <= 2;
}

int tcgetattr(int fd, termios *p)
{
    if (!is_std_fd(fd)) {
        errno = ENOTTY;
        return -1;
    }
    if (!p) {
        errno = EFAULT;
        return -1;
    }
    *p = console::tio;
    return 0;
}

// The console is always ready to accept output, and has no output queue to drain.
int tcsetattr(int fd, int action, const termios *p)
{
    if (!is_std_fd(fd)) {
        errno = ENOTTY;
        return -1;
    }
    switch (action) {
    case TCSANOW:
    case TCSADRAIN:
    case TCSAFLUSH:
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    // Just accept the new settings: the console is always ready to accept output,
    // and has no output queue to drain.
    return 0;
}

int tcdrain(int fd)
{
    // Output is written straight through to the device; nothing is queued.
    return is_std_fd(fd) ? 0 : (errno = ENOTTY, -1);
}

int tcflush(int fd, int what)
{
    // Nothing is buffered in either direction, so there is nothing to discard.
    return is_std_fd(fd) ? 0 : (errno = ENOTTY, -1);
}

speed_t cfgetospeed(const termios *p)
{
    return p->__c_ospeed;
}

int cfsetospeed(struct termios *tio, speed_t speed)
{
    if (!tio) {
        errno = EINVAL;
        return -1;
    }
    tio->__c_ospeed = speed;
    return 0;
}

speed_t cfgetispeed(const termios *p)
{
    return p->__c_ispeed;
}

int cfsetispeed(struct termios *tio, speed_t speed)
{
    if (!tio) {
        errno = EINVAL;
        return -1;
    }
    tio->__c_ispeed = speed;
    return 0;
}

int cfsetspeed(struct termios *tio, speed_t speed)
{
    cfsetispeed(tio, speed);
    return cfsetospeed(tio, speed);
}

int tcsendbreak(int fd, int dur)
{
    // A break condition has no meaning for the console.
    return is_std_fd(fd) ? 0 : (errno = ENOTTY, -1);
}

void cfmakeraw(struct termios *t)
{
	t->c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
	t->c_oflag &= ~OPOST;
	t->c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	t->c_cflag &= ~(CSIZE|PARENB);
	t->c_cflag |= CS8;
	t->c_cc[VMIN] = 1;
	t->c_cc[VTIME] = 0;
}

int system(const char *command)
{
    WARN_STUBBED();
    return -1;
}

extern "C" {
    const char *gnu_get_libc_version(void)
    {
        return OSV_VERSION;
    }

    const char *gnu_get_libc_release(void)
    {
        return "OSv";
    }
}
