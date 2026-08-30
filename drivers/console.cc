/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */


#include <osv/prex.h>
#include <osv/debug.hh>
#include <osv/prio.hh>
#include <queue>
#include <deque>
#include <vector>
#include <sys/ioctl.h>
#include "console.hh"
#include "console-multiplexer.hh"
#include "early-console.hh"

#include <termios.h>
#include <signal.h>
#include <osv/sched.hh>
#include <osv/terminal.h>
#include <cerrno>


namespace console {

termios tio = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = 0,
    .c_lflag = ECHO | ECHOCTL | ICANON | ECHOE | ISIG,
    .c_line = 0,
    .c_cc = {/*VINTR*/'\3', /*VQUIT*/'\34', /*VERASE*/'\177', /*VKILL*/0,
            /*VEOF*/0, /*VTIME*/0, /*VMIN*/0, /*VSWTC*/0,
            /*VSTART*/0, /*VSTOP*/0, /*VSUSP*/0, /*VEOL*/0,
            /*VREPRINT*/0, /*VDISCARD*/0, /*VWERASE*/0,
            /*VLNEXT*/0, /*VEOL2*/0},
};

struct winsize ws = {
    .ws_row = 25,
    .ws_col = 80,
};

console_multiplexer mux __attribute__((init_priority((int)init_prio::console)))
    (&tio, &arch_early_console);

void write(const char *msg, size_t len)
{
    if (len == 0)
        return;

    mux.write(msg, len);
}

// lockless version
void write_ll(const char *msg, size_t len)
{
    if (len == 0)
        return;

    mux.write_ll(msg, len);
}

size_t read(char *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    // Block until something arrives: a reader at a prompt has nothing else to
    // do, and returning 0 would look like EOF.
    int c;
    while ((c = mux.read_char()) < 0) {
        sched::thread::yield();
    }

    size_t n = 0;
    do {
        buf[n++] = static_cast<char>(c);
    } while (n < len && (c = mux.read_char()) >= 0);
    return n;
}

bool input_available()
{
    return mux.input_available();
}

void console_driver_add(console_driver *driver)
{
    mux.driver_add(driver);
}

void console_init()
{
    // There is no filesystem and no /dev/console; the console is reached
    // directly through console::write()/write_ll() and the multiplexer.
    mux.start();
}

}

// Reached from application code (see include/osv/terminal.h), which is why it
// has C linkage and lives outside namespace console.
extern "C" int osv_terminal_size(struct winsize *out)
{
    if (!out) {
        errno = EFAULT;
        return -1;
    }
    *out = console::ws;
    return 0;
}
