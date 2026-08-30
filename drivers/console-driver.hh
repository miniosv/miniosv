/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef DRIVERS_CONSOLE_DRIVER_HH
#define DRIVERS_CONSOLE_DRIVER_HH

#include <cstddef>

namespace console {

// A driver emits bytes, and may also supply them. start() performs any device
// initialization via dev_start().
//
// Input is optional and polls instead of relying on interrrupts.
//
// read_char() returns the next byte, or -1 when none is waiting.
// Drivers that cannot read (early console) inherit the default and simply never produce input.
class console_driver {
public:
    virtual ~console_driver() {}
    virtual void write(const char *str, size_t len) = 0;
    virtual void flush() = 0;
    virtual int read_char() { return -1; }
    //! Whether a byte is waiting, without consuming it. select()/poll() need
    //! to answer "is input ready" without taking the byte away from read().
    virtual bool input_available() { return false; }
    void start();
private:
    virtual void dev_start() = 0;
};

};

#endif
