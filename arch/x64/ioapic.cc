/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/interrupt.hh>
#include "exceptions.hh"
#include <osv/mutex.h>
#include <osv/mem/frames.hh>
#include <osv/mem/phys.hh>

namespace ioapic {

constexpr u64 base_phys = 0xfec00000;
volatile void* base;
constexpr unsigned index_reg_offset = 0;
constexpr unsigned data_reg_offset = 0x10;

mutex mtx;

volatile u32* index_reg()
{
    return reinterpret_cast<volatile u32*>(static_cast<volatile char*>(base) + index_reg_offset);
}

volatile u32* data_reg()
{
    return reinterpret_cast<volatile u32*>(static_cast<volatile char*>(base) + data_reg_offset);
}

u32 read(unsigned reg)
{
    *index_reg() = reg;
    return *data_reg();
}

void write(unsigned reg, u32 data)
{
    *index_reg() = reg;
    *data_reg() = data;
}

void init()
{
    base = mem::map_phys(base_phys, 4096, mem::mattr::dev);
}

}

using namespace ioapic;

void gsi_interrupt::set(unsigned gsi, unsigned vector)
{
    WITH_LOCK(mtx) {
        write(0x10 + gsi * 2 + 1, sched::cpus[0]->arch.apic_id << 24);
        write(0x10 + gsi * 2, vector);
    }
    _gsi = gsi;
}

void gsi_interrupt::clear()
{
    WITH_LOCK(mtx) {
        write(0x10 + _gsi * 2, 1 << 16);  // mask
    }
}
