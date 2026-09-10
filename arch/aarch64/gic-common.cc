/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 * Copyright (C) 2024 Waldemar Kozaczuk
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/mmio.hh>
#include <osv/mem/phys.hh>

#include "gic-common.hh"
#include <osv/mem/mapping.hh>
#include <osv/mem/frames.hh>

namespace gic {

gic_dist::gic_dist(mem::frames::phys_addr b, size_t l) : _base(b)
{
    mem::map_phys_at((void *)_base, _base, l, mem::mapping::page_size, mem::mattr::dev);
}

u32 gic_dist::read_reg(gicd_reg reg)
{
    return mmio_getl(mmio_a((mmioaddr_t)_base, (u32)reg));
}

void gic_dist::write_reg(gicd_reg reg, u32 value)
{
    mmio_setl(mmio_a((mmioaddr_t)_base, (u32)reg), value);
}

u32 gic_dist::read_reg_at_offset(u32 reg, u32 offset)
{
    return mmio_getl(mmio_a((mmioaddr_t)_base, reg + offset));
}

void gic_dist::write_reg_at_offset(u32 reg, u32 offset, u32 value)
{
    mmio_setl(mmio_a((mmioaddr_t)_base, reg + offset), value);
}

void gic_dist::write_reg64_at_offset(u32 reg, u32 offset, u64 value)
{
    mmio_setq(mmio_a((mmioaddr_t)_base, reg + offset), value);
}

unsigned int gic_dist::read_number_of_interrupts()
{
    return ((read_reg(gicd_reg::GICD_TYPER) & 0x1f) + 1) * 32;
}

class gic_driver *gic;
}
