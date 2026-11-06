/* Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/uio.h>
#include <osv/debug.hh>
#include <signal.h>
#include "console-multiplexer.hh"

namespace console {

console_multiplexer::console_multiplexer(const termios *tio, console_driver *early_driver)
    : _tio(tio)
    , _early_driver(early_driver)
{
}

void console_multiplexer::driver_add(console_driver *driver)
{
    _drivers.push_back(driver);
}

void console_multiplexer::start()
{
    for (auto driver : _drivers) {
        driver->start();
    }
    _started = true;
}

void console_multiplexer::drivers_write(const char *str, size_t len)
{
    for (auto driver : _drivers)
        driver->write(str, len);
}

int console_multiplexer::read_char()
{
    SCOPE_LOCK(_mutex);
    for (auto driver : _drivers) {
        int c = driver->read_char();
        if (c >= 0) {
            return c;
        }
    }
    return -1;
}

bool console_multiplexer::input_available()
{
    SCOPE_LOCK(_mutex);
    for (auto driver : _drivers) {
        if (driver->input_available()) {
            return true;
        }
    }
    return false;
}

void console_multiplexer::drivers_flush()
{
    for (auto driver : _drivers)
        driver->flush();
}

void console_multiplexer::write_ll(const char *str, size_t len)
{
    if (!_started) {
        if (_early_driver != nullptr) {
            while (len-- > 0) {
                if ((*str == '\n')) {
                    _early_driver->write("\r", 1);
                }
                _early_driver->write(str++, 1);
            }
            _early_driver->flush();
        }
    } else {
        // Cooked output: translate \n -> \r\n (OPOST | ONLCR), then fan out
        // to every registered driver.
        while (len-- > 0) {
            if (*str == '\n' &&
                (_tio->c_oflag & OPOST) && (_tio->c_oflag & ONLCR)) {
                drivers_write("\r", 1);
            }
            drivers_write(str++, 1);
        }
        drivers_flush();
    }
}

void console_multiplexer::write(const char *str, size_t len)
{
    if (!_started) {
        WITH_LOCK(_early_lock) {
            write_ll(str, len);
        }
    } else {
        WITH_LOCK(_mutex) {
            write_ll(str, len);
        }
    }
}

void console_multiplexer::write(struct uio *uio, int ioflag)
{
    linearize_uio_write(uio, ioflag, [&] (const char *str, size_t len) {
        write(str, len);
    });
}

}
