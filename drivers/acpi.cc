/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/drivers_config.h>
#include <cstring>

#include <osv/align.hh>
#include <osv/debug.h>
#include <osv/prio.hh>
#include "processor.hh"

#include "acpi.hh"
#include <osv/mem/frames.hh>
#include <osv/mem/phys.hh>
#include <osv/mem/mapping.hh>

#define acpi_tag "acpi"
#define acpi_w(...)   tprintf_w(acpi_tag, __VA_ARGS__)

namespace acpi {

uint64_t pvh_rsdp_paddr = 0;

static bool enabled = false;

bool is_enabled()
{
    return enabled;
}

// Root System Description Pointer - the entry point into the ACPI tables.
#pragma pack(push, 1)
struct rsdp {
    char     signature[8];   // "RSD PTR "
    uint8_t  checksum;       // covers the first 20 bytes (ACPI 1.0)
    char     oem_id[6];
    uint8_t  revision;       // 0 -> use rsdt_address, >=2 -> use xsdt_address
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
};
#pragma pack(pop)

// Fixed ACPI Description Table ("FACP"). Only the leading fields up to the
// PM1 control blocks are declared - that and the DSDT address are all we need
// to drive an S5 power-off.
#pragma pack(push, 1)
struct fadt {
    table_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_request;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
};
#pragma pack(pop)

// The parsed tables. A plain static array (rather than a std::vector) so that
// it is zero-initialized at load time - early_init() runs as a constructor and
// must not race with the initialization of a non-trivial global.
static const table_header *tables[64];
static unsigned ntables;

#ifdef __x86_64__
// ACPI S5 ("soft off") parameters, extracted from the FADT and DSDT. The
// SLP_TYP values are already shifted into their PM1_CNT bit position. This
// soft-off path is x86-only (it pokes PM1 control I/O ports); aarch64 powers
// off through PSCI.
static constexpr uint16_t SLP_EN = 1 << 13;
static uint16_t pm1a_cnt_port;
static uint16_t pm1b_cnt_port;
static uint16_t slp_typ_a;
static uint16_t slp_typ_b;
static bool s5_ready;
#endif

// Linear-map a physical range (huge-page aligned) and return a virtual pointer
// to it. Each huge page is mapped at most once - ACPI tables are tiny and
// clustered, so this typically only maps a handful of pages overall.
static void *map_phys(uint64_t pa, size_t len)
{
    static uint64_t mapped[16];
    static unsigned nmapped;

    uint64_t base = align_down(pa, mem::mapping::huge_page_size);
    uint64_t end = align_up(pa + len, mem::mapping::huge_page_size);
    for (uint64_t p = base; p < end; p += mem::mapping::huge_page_size) {
        bool already = false;
        for (unsigned i = 0; i < nmapped; i++) {
            if (mapped[i] == p) {
                already = true;
                break;
            }
        }
        if (!already) {
            mem::map_phys(p, mem::mapping::huge_page_size);
            if (nmapped < 16) {
                mapped[nmapped++] = p;
            }
        }
    }
    return mem::map_phys(pa, len);
}

#ifdef __x86_64__
static bool checksum_ok(const void *p, size_t len)
{
    uint8_t sum = 0;
    auto b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < len; i++) {
        sum += b[i];
    }
    return sum == 0;
}

static const rsdp *scan_rsdp(uint64_t start, uint64_t end)
{
    for (uint64_t pa = start; pa < end; pa += 16) {
        auto r = static_cast<const rsdp *>(map_phys(pa, sizeof(rsdp)));
        if (!memcmp(r->signature, "RSD PTR ", 8) && checksum_ok(r, 20)) {
            return r;
        }
    }
    return nullptr;
}
#endif

static const rsdp *find_rsdp()
{
    // A PVH boot hands us the RSDP address directly.
    if (pvh_rsdp_paddr) {
        return static_cast<const rsdp *>(map_phys(pvh_rsdp_paddr, sizeof(rsdp)));
    }

#ifdef __x86_64__
    // Otherwise scan the two BIOS regions the spec allows the RSDP to live in:
    // the first KB of the Extended BIOS Data Area, and the BIOS read-only area.
    // (Legacy BIOS-only fallback; under UEFI the RSDP always arrives above.)
    uint16_t ebda_seg = *static_cast<const uint16_t *>(map_phys(0x40e, sizeof(uint16_t)));
    uint64_t ebda = static_cast<uint64_t>(ebda_seg) << 4;
    if (ebda) {
        if (auto r = scan_rsdp(ebda, ebda + 1024)) {
            return r;
        }
    }
    return scan_rsdp(0xe0000, 0x100000);
#else
    return nullptr;
#endif
}

// Map a table given its physical address and remember its header. The first
// mapping only covers the fixed header; once we know the real length we remap
// to make sure the whole table is reachable.
static void add_table(uint64_t pa)
{
    if (!pa || ntables >= 64) {
        return;
    }
    auto hdr = static_cast<const table_header *>(map_phys(pa, sizeof(table_header)));
    hdr = static_cast<const table_header *>(map_phys(pa, hdr->length));
    tables[ntables++] = hdr;
}

const table_header *find_table(const char *signature)
{
    for (unsigned i = 0; i < ntables; i++) {
        if (!memcmp(tables[i]->signature, signature, 4)) {
            return tables[i];
        }
    }
    return nullptr;
}

uint16_t arm_boot_flags()
{
    auto fadt = find_table("FACP");
    if (!fadt) {
        return 0;
    }
    // ARM Boot Architecture Flags are a UINT16 at offset 0x81 in the FADT
    // (after Flags/RESET_REG/RESET_VALUE; offset 0x70 is the x86 Flags field).
    return *reinterpret_cast<const uint16_t *>(
        reinterpret_cast<const char *>(fadt) + 0x81);
}

// Locate the FADT, grab its PM1 control ports and the DSDT, then scan the DSDT
// bytecode for the predefined "\_S5_" object to extract the S5 sleep-type
// values. This is enough to issue an ACPI soft-off without a full AML
// interpreter. The scan recognizes the handful of byte sequences that compilers
// emit for "Name (_S5, Package () { ... })".
#ifdef __x86_64__
static void parse_s5()
{
    auto fadt = reinterpret_cast<const struct fadt *>(find_table("FACP"));
    if (!fadt || !fadt->pm1a_control_block || !fadt->dsdt) {
        return;
    }

    auto dsdt = static_cast<const table_header *>(
            map_phys(fadt->dsdt, sizeof(table_header)));
    dsdt = static_cast<const table_header *>(map_phys(fadt->dsdt, dsdt->length));

    auto aml = reinterpret_cast<const uint8_t *>(dsdt);
    uint32_t len = dsdt->length;
    for (uint32_t i = sizeof(table_header); i + 6 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4)) {
            continue;
        }
        // Must be introduced by a NameOp (0x08), possibly with a leading root
        // prefix ('\'), and followed by a PackageOp (0x12).
        bool name_op = aml[i - 1] == 0x08 ||
                       (i >= 2 && aml[i - 2] == 0x08 && aml[i - 1] == '\\');
        if (!name_op || aml[i + 4] != 0x12) {
            continue;
        }
        const uint8_t *p = aml + i + 5;
        p += (*p >> 6) + 1;   // skip the PkgLength encoding bytes
        p++;                  // skip the element count
        if (*p == 0x0a) {     // optional BytePrefix before SLP_TYPa
            p++;
        }
        slp_typ_a = static_cast<uint16_t>(*p++) << 10;
        if (*p == 0x0a) {     // optional BytePrefix before SLP_TYPb
            p++;
        }
        slp_typ_b = static_cast<uint16_t>(*p) << 10;
        pm1a_cnt_port = fadt->pm1a_control_block;
        pm1b_cnt_port = fadt->pm1b_control_block;
        s5_ready = true;
        return;
    }
}
#endif

bool power_off()
{
#ifdef __x86_64__
    if (!s5_ready) {
        return false;
    }
    processor::outw(slp_typ_a | SLP_EN, pm1a_cnt_port);
    if (pm1b_cnt_port) {
        processor::outw(slp_typ_b | SLP_EN, pm1b_cnt_port);
    }
    return true;
#else
    // aarch64 powers off through PSCI, not ACPI S5 I/O ports.
    return false;
#endif
}

void early_init()
{
    auto r = find_rsdp();
    if (!r) {
        acpi_w("Warning: Failed to find ACPI root pointer!\n");
        return;
    }

    // Prefer the 64-bit XSDT (ACPI 2.0+) and fall back to the RSDT.
    bool use_xsdt = r->revision >= 2 && r->xsdt_address;
    uint64_t sdt_pa = use_xsdt ? r->xsdt_address : r->rsdt_address;
    size_t entry_size = use_xsdt ? sizeof(uint64_t) : sizeof(uint32_t);

    auto sdt = static_cast<const table_header *>(map_phys(sdt_pa, sizeof(table_header)));
    uint32_t sdt_length = sdt->length;
    sdt = static_cast<const table_header *>(map_phys(sdt_pa, sdt_length));

    // The entry array follows the header. It is only 4-byte aligned, so copy
    // each (possibly 8-byte) entry out with memcpy to avoid unaligned access.
    auto entries = reinterpret_cast<const char *>(sdt) + sizeof(table_header);
    unsigned count = (sdt_length - sizeof(table_header)) / entry_size;
    for (unsigned i = 0; i < count; i++) {
        uint64_t table_pa = 0;
        memcpy(&table_pa, entries + i * entry_size, entry_size);
        add_table(table_pa);
    }

    enabled = ntables > 0;

#ifdef __x86_64__
    parse_s5();
#endif
}

}

void __attribute__((constructor(init_prio::acpi))) acpi_init_early()
{
    acpi::early_init();
}
