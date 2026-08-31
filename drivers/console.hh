/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef DRIVERS_CONSOLE_HH
#define DRIVERS_CONSOLE_HH

#include "console-driver.hh"

struct termios;
struct winsize;

namespace console {

// The console's terminal settings, such as they are. Exposed so libc's
// ioctl(TCGETS/TCSETS/TIOCGWINSZ) can answer for the standard streams: an
// interactive program asks before it will do line editing.
extern ::termios tio;
extern ::winsize ws;

void write(const char *msg, size_t len);
// Blocking read of at least one byte; returns the number read. Yields between
// polls so a reader waiting at a prompt does not monopolise its CPU.
size_t read(char *buf, size_t len);
// Whether read() would return without blocking.
bool input_available();
void write_ll(const char *msg, size_t len);
void console_init();
void console_driver_add(console_driver *driver);

}

#endif
