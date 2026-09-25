/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
#include "apic.hh"
#include "msr.hh"
#include <osv/percpu.hh>
#include <cpuid.hh>
#include <processor.hh>
#include <osv/debug.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/frames.hh>
#include <osv/mem/phys.hh>

namespace processor {

static constexpr u32 msr_kvm_pv_eoi = 0x4b564d04;
static PERCPU(u32, kvm_pv_eoi_word);

void kvm_pv_eoi_init()
{
    if (features().kvm_pv_eoi) {
        wrmsr(msr_kvm_pv_eoi, mem::mapping::to_phys(&*kvm_pv_eoi_word) | 1);
    }
}

inline bool try_fast_eoi()
{
    u8 r;
    asm("btr %2, %0; setc %1" : "+m"(*kvm_pv_eoi_word), "=rm"(r) : "r"(0));
    return r;
}

class x2apic : public apic_driver {
public:
    x2apic() : apic_driver() { enable(); }
    virtual void self_ipi(unsigned vector);
    virtual void ipi(unsigned apic_id, unsigned vector);
    virtual void init_ipi(unsigned apic_id, unsigned vector);
    virtual void ipi_allbutself(unsigned vector);
    virtual void nmi_allbutself();
    virtual void eoi();
    virtual u32 id();

    virtual u32 read(apicreg reg)
        { return rdmsr(0x800 + unsigned(reg) / 0x10); }
    virtual void write(apicreg reg, u32 value)
        { wrmsr(0x800 + unsigned(reg) / 0x10, value); }

protected:
    virtual void enable();
};

class xapic final : public apic_driver {
public:
    xapic();

    virtual void self_ipi(unsigned vector)
        { xapic::ipi(APIC_ICR_TYPE_FIXED | APIC_SHORTHAND_SELF, vector); }

    virtual void init_ipi(unsigned apic_id, unsigned vector)
        { xapic::ipi(apic_id, vector);}

    virtual void ipi_allbutself(unsigned vector);

    virtual void nmi_allbutself();

    virtual void ipi(unsigned apic_id, unsigned vector);

    virtual void eoi();
    virtual u32 id();

    virtual void write(apicreg reg, u32 value) { *reg_ptr(reg) = value; }
    virtual u32 read(apicreg reg) { return *reg_ptr(reg); }

protected:
    virtual void enable();

private:
    volatile u32* reg_ptr(apicreg r)
        {return reinterpret_cast<u32*>(&_base_virt[static_cast<unsigned>(r)]);}

    u8 *_base_virt = nullptr;

    static constexpr unsigned ICR2_DESTINATION_SHIFT = 24;
    static constexpr unsigned XAPIC_ID_SHIFT = 24;
};

void apic_driver::software_enable()
{
    write(apicreg::LVT0, 0);
    write(apicreg::SPIV, 0x1ff); // FIXME: allocate real vector
}

void apic_driver::read_base()
{
    static constexpr u64 base_addr_mask = 0xFFFFFF000;
    _apic_base = rdmsr(msr::IA32_APIC_BASE) & base_addr_mask;
}

xapic::xapic()
    : apic_driver()
{
    _base_virt = static_cast<u8*>(mem::map_phys(_apic_base, 4096, mem::mattr::dev));
    xapic::enable();
}

void xapic::enable()
{
    wrmsr(msr::IA32_APIC_BASE, rdmsr(msr::IA32_APIC_BASE) | APIC_BASE_GLOBAL_ENABLE);
    software_enable();
}

void xapic::nmi_allbutself()
{
    xapic::write(apicreg::ICR, APIC_ICR_TYPE_FIXED | APIC_SHORTHAND_ALLBUTSELF |
                               apic_delivery(NMI_DELIVERY));
}

void xapic::ipi_allbutself(unsigned vector)
{
    xapic::write(apicreg::ICR, APIC_ICR_TYPE_FIXED | APIC_SHORTHAND_ALLBUTSELF |
                               vector | APIC_ICR_LEVEL_ASSERT);
}

void xapic::ipi(unsigned apic_id, unsigned vector)
{
    xapic::write(apicreg::ICR2, apic_id << ICR2_DESTINATION_SHIFT);
    xapic::write(apicreg::ICR, vector | APIC_ICR_LEVEL_ASSERT);
}

void xapic::eoi()
{
    if (try_fast_eoi()) {
        return;
    }

    xapic::write(apicreg::EOR, 0);
}

u32 xapic::id()
{
    return xapic::read(apicreg::ID) >> XAPIC_ID_SHIFT;
}

u32 x2apic::id()
{
    u32 id = read(apicreg::ID);
    return id;
}

void x2apic::enable()
{
    wrmsr(msr::IA32_APIC_BASE, rdmsr(msr::IA32_APIC_BASE) | APIC_BASE_GLOBAL_ENABLE | (1 << 10));
    software_enable();
}

void x2apic::self_ipi(unsigned vector)
{
    wrmsr(msr::X2APIC_SELF_IPI, vector);
}

void x2apic::ipi(unsigned apic_id, unsigned vector)
{
    wrmsr(msr::X2APIC_ICR, vector | (u64(apic_id) << 32) | APIC_ICR_LEVEL_ASSERT);
}

void x2apic::init_ipi(unsigned apic_id, unsigned vector)
{
    wrmsr_safe(msr::X2APIC_ICR, vector | (u64(apic_id) << 32));
}

void x2apic::ipi_allbutself(unsigned vector)
{
    wrmsr(msr::X2APIC_ICR, vector | APIC_SHORTHAND_ALLBUTSELF | APIC_ICR_LEVEL_ASSERT);
}

void x2apic::nmi_allbutself()
{
    wrmsr(msr::X2APIC_ICR, APIC_SHORTHAND_ALLBUTSELF |
                           apic_delivery(NMI_DELIVERY) |
                           APIC_ICR_LEVEL_ASSERT);
}

void x2apic::eoi()
{
    if (try_fast_eoi()) {
        return;
    }
    wrmsr(msr::X2APIC_EOI, 0);
}

apic_driver* create_apic_driver()
{
    // TODO: Some Xen versions do not expose x2apic CPU feature
    //       but still support it. Should we do more precise detection?
    if (features().x2apic) {
        return new x2apic;
    } else {
        return new xapic;
    };
}

apic_driver* apic = create_apic_driver();

msi_message apic_driver::compose_msix(u8 vector, u8 dest_id)
{
    msi_message msg;

    if (vector <= 15) {
        return msg;
    }

    msg._addr =
        ( _apic_base & 0xFFF00000 ) |
        ( dest_id << 12 );

    msg._data =
        ( delivery_mode::FIXED_DELIVERY << msi_data_fields::MSI_DELIVERY_MODE ) |
        ( level_assertion::MSI_ASSSERT << msi_data_fields::MSI_LEVEL_ASSERTION ) |
        ( trigger_mode::TRIGGER_MODE_EDGE << msi_data_fields::MSI_TRIGGER_MODE ) |
        vector;

    return msg;
}

}
