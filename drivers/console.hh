/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef DRIVERS_CONSOLE_HH
#define DRIVERS_CONSOLE_HH

#include <termios.h>

#include "console-driver.hh"

namespace console {

// The terminal settings of the console, for the termios calls.
extern ::termios tio;

void write(const char *msg, size_t len);
void write_ll(const char *msg, size_t len);
void console_init();
void console_driver_add(console_driver *driver);

}

#endif
