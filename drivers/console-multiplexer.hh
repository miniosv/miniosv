/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef DRIVERS_CONSOLE_MULTIPLEXER_HH
#define DRIVERS_CONSOLE_MULTIPLEXER_HH
#include <osv/spinlock.h>
#include <osv/mutex.h>
#include <termios.h>
#include <list>
#include "console-driver.hh"

namespace console {

// Fans writes out to all registered drivers, applying the cooked-output
// \n -> \r\n (ONLCR) translation.
//
// Input is raw: read_char() returns the first byte any driver offers, with no
// line discipline. (linenoise does its own editing).
class console_multiplexer {
public:
    explicit console_multiplexer(const termios *tio, console_driver *early_driver = nullptr);
    ~console_multiplexer() {};
    void driver_add(console_driver *driver);
    void start();
    void write_ll(const char *str, size_t len);
    void write(const char *str, size_t len);
    void write(struct uio *uio, int ioflag);
    int read_char();
    bool input_available();
private:
    void drivers_write(const char *str, size_t len);
    void drivers_flush();
    const termios *_tio;
    spinlock _early_lock;
    bool _started = false;
    console_driver *_early_driver;
    std::list<console_driver *> _drivers;
    mutex _mutex;
};

};

#endif
