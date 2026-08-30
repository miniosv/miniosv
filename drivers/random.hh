/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef RANDOM_DEVICE_H
#define RANDOM_DEVICE_H

#include <osv/types.h>
#include <sys/types.h>
#include <memory>

namespace randomdev {

class hw_rng;

class random_device {
public:

    random_device();
    virtual ~random_device();

    // Register/unregister a hardware entropy source (e.g. virtio-rng) that the
    // CSPRNG pulls from when reseeding.
    static void register_source(hw_rng* hwrng);
    static void deregister_source(hw_rng* hwrng);
};

class hw_rng {
public:
    virtual size_t get_random_bytes(char *buf, size_t size) = 0;
};

void randomdev_init();

}

extern "C" {
uint32_t arc4random(void);
void arc4rand(void *ptr, u_int len, int /*reseed*/);
}

#endif
