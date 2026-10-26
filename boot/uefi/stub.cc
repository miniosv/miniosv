/*
 * Copyright (C) 2026 miniosv contributors
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

//
// miniosv UEFI boot stub.
//
// This is a small, freestanding UEFI application - the image the firmware
// loads from the EFI System Partition (\EFI\BOOT\BOOTX64.EFI /
// \EFI\BOOT\BOOTAA64.EFI). The kernel ELF is embedded into this image as a
// binary blob (see kernel-blob.S). The stub:
//
//   1. locates the ACPI RSDP from the UEFI configuration table,
//   2. reads the command line from the loaded-image LoadOptions,
//   3. loads the embedded kernel ELF's PT_LOAD segments to their physical
//      link addresses,
//   4. normalizes the UEFI memory map into osv::boot_memmap_entry[],
//   5. calls ExitBootServices(),
//   6. hands control to the kernel entry point with a pointer to osv::boot_info.
//
// It is the single boot path for both architectures; everything arch-specific
// is confined to the tiny handoff trampoline at the bottom and to the kernel
// side (arch/<arch>/boot.S uefi_entry).
//

#include "efi.hh"
#include <osv/boot-info.hh>

// The kernel links its segments to a fixed virtual window but is loaded at a
// firmware-chosen physical base (see load_kernel). The stub places each segment
// by its offset from the image's lowest virtual address, so it relies on
// neither ELF p_paddr (which the aarch64 position-independent link does not set
// usefully) nor a fixed load address.

// ---- embedded kernel ELF (kernel-blob.S) ---------------------------------
extern "C" const unsigned char kernel_elf_start[];
extern "C" const unsigned char kernel_elf_end[];

// ---- minimal ELF64 ---------------------------------------------------------
namespace {

struct Elf64_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

constexpr uint32_t PT_LOAD = 1;
constexpr uint64_t EFI_PAGE_SIZE = 4096;

EFI_SYSTEM_TABLE  *ST;
EFI_BOOT_SERVICES *BS;

} // namespace

// Freestanding mem helpers (the stub links no libc). The volatile destination
// stops clang's loop-idiom pass from rewriting these loops back into calls to
// memcpy/memset (which would recurse). The compiler may also emit calls to
// these for aggregate copies/zeroing, so they carry C linkage.
extern "C" void *memcpy(void *dst, const void *src, unsigned long n)
{
    auto d = static_cast<volatile unsigned char *>(dst);
    auto s = static_cast<const unsigned char *>(src);
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

extern "C" void *memset(void *dst, int c, unsigned long n)
{
    auto d = static_cast<volatile unsigned char *>(dst);
    for (unsigned long i = 0; i < n; i++)
        d[i] = static_cast<unsigned char>(c);
    return dst;
}

namespace {

void print(const char *s)
{
    if (!ST || !ST->ConOut)
        return;
    CHAR16 buf[2];
    buf[1] = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            buf[0] = '\r';
            ST->ConOut->OutputString(ST->ConOut, buf);
        }
        buf[0] = static_cast<CHAR16>(*s);
        ST->ConOut->OutputString(ST->ConOut, buf);
    }
}

// SPCR fields the kernel needs to pick and configure the right console driver.
// Interface Type (offset 36) tells PL011/SBSA (3/0x0e) apart from 16550 (0/1);
// the GAS Access Size (offset 43) gives the register access width (1=byte,
// 2=word, 3=dword) - AWS Graviton's 16550 is dword (mmio32).
unsigned char spcr_iface_type = 0xff;
unsigned char spcr_access_size = 0;     // GAS AccessSize: 0=undef,1=8b,2=16b,3=32b,4=64b
char acpi_oem[7] = "";                  // OEMID of the root table, e.g. "AMAZON"

// Unaligned little-endian reads (ACPI table fields are not naturally aligned).
uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
uint64_t rd64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

// Locate the ACPI SPCR (Serial Port Console Redirection) table and return the
// console UART's physical base address (0 if none). The kernel's early console
// targets this instead of a compiled-in default, so serial output works on
// platforms whose UART is not at the QEMU virt address (e.g. AWS Graviton).
uint64_t find_spcr_uart(uint64_t rsdp)
{
    if (!rsdp)
        return 0;
    auto r = reinterpret_cast<const unsigned char *>(rsdp);
    // RSDP: XsdtAddress at offset 24 (ACPI 2.0+), RsdtAddress at offset 16.
    uint64_t xsdt = rd64(r + 24);
    uint64_t rsdt = rd32(r + 16);
    uint64_t hdr = xsdt ? xsdt : rsdt;
    int entsize = xsdt ? 8 : 4;
    if (!hdr)
        return 0;
    auto h = reinterpret_cast<const unsigned char *>(hdr);
    uint32_t len = rd32(h + 4);
    uint32_t n = (len > 36) ? (len - 36) / entsize : 0;
    memcpy(acpi_oem, h + 10, 6);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t tbl = (entsize == 8) ? rd64(h + 36 + i * 8)
                                      : (uint64_t)rd32(h + 36 + i * 4);
        auto t = reinterpret_cast<const unsigned char *>(tbl);
        if (t[0] == 'S' && t[1] == 'P' && t[2] == 'C' && t[3] == 'R') {
            spcr_iface_type = t[36];  // Interface Type (0/1 = 16550, 3/0x0e = PL011/SBSA)
            spcr_access_size = t[43]; // GAS AccessSize (3 = dword/mmio32 on Graviton)
            return rd64(t + 44);      // Base Address (GAS Address field)
        }
    }
    return 0;
}

[[noreturn]] void die(const char *msg)
{
    print("UEFI stub: ");
    print(msg);
    print("\n");
    for (;;)
        ;
}

// Walk the UEFI configuration table for the ACPI RSDP, preferring ACPI 2.0+.
uint64_t find_rsdp()
{
    EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = EFI_ACPI_10_TABLE_GUID;
    uint64_t found = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        auto &e = ST->ConfigurationTable[i];
        if (efi_guid_eq(e.VendorGuid, acpi20))
            return reinterpret_cast<uint64_t>(e.VendorTable);
        if (efi_guid_eq(e.VendorGuid, acpi10))
            found = reinterpret_cast<uint64_t>(e.VendorTable);
    }
    return found;
}

// Wall-clock at boot from UEFI GetTime(), in nanoseconds since the Unix epoch
// (0 if the firmware has no time source). This gives the kernel a real-time
// base without an RTC driver - important on aarch64, whose generic timer is a
// monotonic counter only.
uint64_t read_boot_time()
{
    if (!ST->RuntimeServices) {
        return 0;
    }
    EFI_TIME t;
    if (EFI_ERROR(ST->RuntimeServices->GetTime(&t, nullptr))) {
        return 0;
    }
    // Days since 1970-01-01 (Howard Hinnant's days_from_civil).
    int64_t y = t.Year;
    unsigned m = t.Month, d = t.Day;
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    int64_t secs = days * 86400 + t.Hour * 3600 + t.Minute * 60 + t.Second;
    // EFI_TIME is local time; convert to UTC when the zone is specified.
    if (t.TimeZone != 0x07ff) {
        secs -= static_cast<int64_t>(t.TimeZone) * 60;
    }
    return static_cast<uint64_t>(secs) * 1000000000ull + t.Nanosecond;
}

// Copy the loaded-image LoadOptions (UTF-16) into an ASCII command line.
// Returns length (excluding NUL); cmdline buffer is caller-provided.
uint32_t read_cmdline(EFI_HANDLE image, char *out, uint32_t cap)
{
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = nullptr;
    if (EFI_ERROR(BS->HandleProtocol(image, &li_guid,
                                     reinterpret_cast<void **>(&li))) || !li)
        return 0;
    auto opts = static_cast<const CHAR16 *>(li->LoadOptions);
    uint32_t n16 = li->LoadOptionsSize / sizeof(CHAR16);
    uint32_t len = 0;
    for (uint32_t i = 0; i < n16 && len + 1 < cap; i++) {
        CHAR16 c = opts[i];
        if (c == 0)
            break;
        out[len++] = (c < 0x80) ? static_cast<char>(c) : '?';
    }
    out[len] = 0;
    return len;
}

// Load the embedded kernel ELF at a firmware-chosen physical base. Returns the
// physical entry point and reports the ELF image's physical base (the address
// of the lowest-vaddr PT_LOAD segment, i.e. where the ELF header lands) in
// *kernel_phys_base.
//
// The kernel is *not* pinned to a fixed physical address: the kernel's boot
// page tables map its fixed virtual link window to wherever we load it (the
// per-arch boot.S derives the virt->phys offset at runtime from
// kernel_phys_base). We only constrain the load to a 2 MiB-aligned region that
// the boot tables' identity map is guaranteed to cover, so that the kernel
// entry can keep executing at its physical address across the page-table
// switch. Relying on a single fixed address being free is exactly what breaks
// on firmware that reserves it (e.g. x86 Azure reserves low RAM at 2 MiB).
uint64_t load_kernel(uint64_t *kernel_phys_base)
{
    auto blob = kernel_elf_start;
    auto ehdr = reinterpret_cast<const Elf64_Ehdr *>(blob);
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F')
        die("embedded kernel is not an ELF");

    // Virtual span of the image (lowest segment vaddr .. highest segment end).
    uint64_t lowest_vaddr = ~0ull, highest_end = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        auto ph = reinterpret_cast<const Elf64_Phdr *>(
            blob + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_vaddr < lowest_vaddr)
            lowest_vaddr = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > highest_end)
            highest_end = ph->p_vaddr + ph->p_memsz;
    }
    if (lowest_vaddr == ~0ull)
        die("embedded kernel has no loadable segments");

    constexpr uint64_t TWO_MIB = 0x200000ull;
    // 2 MiB-aligned virtual base the image is laid out relative to; the kernel
    // links its window on a 2 MiB boundary, so the boot tables map it with
    // 2 MiB pages and the physical base must share that alignment.
    uint64_t vbase = lowest_vaddr & ~(TWO_MIB - 1);
    uint64_t span = highest_end - vbase;

    // Cap the load below the upper bound of the boot tables' identity map:
    // x64 boot.S identity-maps the low 1 GiB; aarch64 boot.S maps 0-63 GiB.
#if defined(__x86_64__)
    EFI_PHYSICAL_ADDRESS max_addr = 0x40000000ull;   // < 1 GiB
#elif defined(__aarch64__)
    EFI_PHYSICAL_ADDRESS max_addr = 0xfc0000000ull;  // < 63 GiB
#else
#error "unsupported architecture"
#endif

    // Pick the *lowest* free, 2 MiB-aligned region of at least `span` bytes below
    // that ceiling, by scanning the UEFI memory map. Loading low matters for two
    // reasons: it keeps the kernel within the boot identity map, and it keeps it
    // at the base of usable RAM - the aarch64 memory model (uefi_memory_setup)
    // anchors OSv's heap at the kernel's physical base and grows upward, so a
    // high placement would strand most of RAM. We must not, however, depend on
    // any single fixed address being free: that is what fails on firmware which
    // reserves it (e.g. x86 Azure reserves low RAM at 2 MiB). Keep clear of the
    // lowest 1 MiB (real-mode/firmware scratch, and the x86 SMP trampoline's
    // physical-zero target reclaimed later).
    constexpr uint64_t FLOOR = 0x100000ull;
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    BS->GetMemoryMap(&map_size, nullptr, &map_key, &desc_size, &desc_ver);
    map_size += 8 * desc_size;  // slack for the allocation this very query makes
    EFI_MEMORY_DESCRIPTOR *map = nullptr;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, map_size,
                                   reinterpret_cast<void **>(&map))))
        die("out of memory probing the memory map");
    if (EFI_ERROR(BS->GetMemoryMap(&map_size, map, &map_key, &desc_size,
                                   &desc_ver)))
        die("GetMemoryMap failed");

    uint64_t base = ~0ull;  // phys addr of vbase (the kernel's 2 MiB-aligned base)
    for (UINTN off = 0; off < map_size; off += desc_size) {
        auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(
            reinterpret_cast<unsigned char *>(map) + off);
        if (d->Type != EfiConventionalMemory)
            continue;
        uint64_t rstart = d->PhysicalStart;
        uint64_t rend = rstart + d->NumberOfPages * EFI_PAGE_SIZE;
        uint64_t astart = rstart < FLOOR ? FLOOR : rstart;
        astart = (astart + TWO_MIB - 1) & ~(TWO_MIB - 1);
        if (astart + span <= rend && astart + span <= max_addr && astart < base)
            base = astart;
    }
    BS->FreePool(map);
    if (base == ~0ull)
        die("no usable low memory for the kernel");

    // EfiLoaderCode keeps the image executable: the kernel entry runs a few
    // instructions under the firmware's identity map before installing its own
    // page tables, and strict firmware (e.g. AArch64 AAVMF) faults on
    // instruction fetch from non-executable EfiLoaderData pages.
    UINTN pages = ((span + TWO_MIB - 1) & ~(TWO_MIB - 1)) / EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS got = base;
    if (EFI_ERROR(BS->AllocatePages(AllocateAddress, EfiLoaderCode, pages, &got)))
        die("cannot reserve kernel physical memory");

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        auto ph = reinterpret_cast<const Elf64_Phdr *>(
            blob + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        uint64_t dst = base + (ph->p_vaddr - vbase);
        memcpy(reinterpret_cast<void *>(dst), blob + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset(reinterpret_cast<void *>(dst + ph->p_filesz), 0,
                    ph->p_memsz - ph->p_filesz);
    }

    *kernel_phys_base = base + (lowest_vaddr - vbase);
    return base + (ehdr->e_entry - vbase);
}

// Per-arch handoff: set the kernel's first argument register to boot_info and
// jump to its physical entry point. Runs after ExitBootServices, in the
// firmware's identity mapping. Never returns.
[[noreturn]] void handoff(uint64_t entry_phys, osv::boot_info *bi)
{
#if defined(__x86_64__)
    asm volatile("mov %0, %%rdi\n\t"
                 "jmp *%1\n\t"
                 : : "r"(bi), "r"(entry_phys) : "rdi", "memory");
#elif defined(__aarch64__)
    asm volatile("mov x0, %0\n\t"
                 "br %1\n\t"
                 : : "r"(bi), "r"(entry_phys) : "x0", "memory");
#else
#error "unsupported architecture"
#endif
    __builtin_unreachable();
}

} // namespace

extern "C" EFIAPI EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    BS = st->BootServices;

    print("miniosv UEFI stub starting\n");

    uint64_t rsdp = find_rsdp();
    uint64_t uart_base = find_spcr_uart(rsdp);

    // Diagnostic: what the firmware says about the console UART, printed
    // through UEFI's own console. The kernel picks its serial driver from
    // these, and a wrong pick is silent - the only trace is this line.
    {
        static const char hex[] = "0123456789abcdef";
        char line[] = "UEFI stub: spcr type=0x.. access=0x.. base=0x................\n";
        unsigned char vals[2] = {spcr_iface_type, spcr_access_size};
        char *t = line + 23; char *a = line + 35; char *b = line + 45;
        t[0] = hex[vals[0] >> 4]; t[1] = hex[vals[0] & 15];
        a[0] = hex[vals[1] >> 4]; a[1] = hex[vals[1] & 15];
        for (int i = 0; i < 16; ++i)
            b[i] = hex[(uart_base >> (60 - 4 * i)) & 15];
        print(line);
        print("UEFI stub: acpi oem="); print(acpi_oem); print("\n");
    }

    // Allocate the hand-off structures as EfiLoaderData so they survive
    // ExitBootServices. Size the memmap generously - the UEFI map is bounded
    // by available RAM regions and never approaches this.
    constexpr uint32_t MAX_MEMMAP = 512;
    constexpr uint32_t CMDLINE_CAP = 4096;

    // The kernel reads these hand-off structures (boot_info, the memory map
    // and the cmdline) very early, before it has mapped all of RAM - while it
    // still runs on the boot page tables, which only cover low physical memory
    // (the first 1 GiB on x64; the boot identity map on aarch64). AllocatePool
    // lets the firmware place them anywhere, and on multi-GiB guests it puts
    // them near the top of RAM - which the early kernel then misreads through
    // its low map, getting a zeroed memory map and dying. So reserve them with
    // an explicit upper bound the kernel's boot tables are guaranteed to map.
#ifdef __aarch64__
    constexpr uint64_t HANDOFF_MAX_ADDR = 0xfc0000000ull; // boot identity = 63 GiB
#else
    constexpr uint64_t HANDOFF_MAX_ADDR = 0x40000000ull;  // boot.S maps first 1 GiB
#endif
    constexpr UINTN page = EFI_PAGE_SIZE;
    constexpr UINTN bi_bytes = (sizeof(osv::boot_info) + page - 1) & ~(page - 1);
    constexpr UINTN mm_bytes =
        (sizeof(osv::boot_memmap_entry) * MAX_MEMMAP + page - 1) & ~(page - 1);
    constexpr UINTN cmd_bytes = (CMDLINE_CAP + page - 1) & ~(page - 1);
    constexpr UINTN handoff_bytes = bi_bytes + mm_bytes + cmd_bytes;

    EFI_PHYSICAL_ADDRESS handoff_base = HANDOFF_MAX_ADDR;
    if (EFI_ERROR(BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                    handoff_bytes / page, &handoff_base)))
        die("cannot reserve low memory for boot info");
    osv::boot_info *bi = reinterpret_cast<osv::boot_info *>(handoff_base);
    osv::boot_memmap_entry *mm =
        reinterpret_cast<osv::boot_memmap_entry *>(handoff_base + bi_bytes);
    char *cmdline =
        reinterpret_cast<char *>(handoff_base + bi_bytes + mm_bytes);

    uint32_t cmdline_len = read_cmdline(image, cmdline, CMDLINE_CAP);

    uint64_t kernel_phys_base = 0;
    uint64_t entry_phys = load_kernel(&kernel_phys_base);

    bi->magic = osv::boot_info_magic;
    bi->acpi_rsdp = rsdp;
    bi->memmap_addr = reinterpret_cast<uint64_t>(mm);
    bi->cmdline_addr = reinterpret_cast<uint64_t>(cmdline);
    bi->cmdline_len = cmdline_len;
    bi->kernel_phys_base = kernel_phys_base;
    bi->boot_unixtime_ns = read_boot_time();
    bi->uart_base = uart_base;
    bi->uart_type = spcr_iface_type;
    // GAS AccessSize -> register access width in bytes (1/2/4); default to byte
    // when the firmware leaves it undefined.
    bi->uart_access_width = spcr_access_size == 3 ? 4 :
                            spcr_access_size == 2 ? 2 : 1;

    // Fetch the UEFI memory map, normalize it, and ExitBootServices. The map
    // key can go stale between GetMemoryMap and ExitBootServices, so retry.
    // No allocation happens inside the loop: the EFI map buffer is sized once.
    UINTN efi_map_cap = 0, efi_desc_size = 0;
    UINTN map_key = 0;
    uint32_t desc_ver = 0;
    {
        // Probe required size, then over-allocate to absorb churn.
        UINTN need = 0;
        BS->GetMemoryMap(&need, nullptr, &map_key, &efi_desc_size, &desc_ver);
        efi_map_cap = need + 8 * efi_desc_size;
    }
    EFI_MEMORY_DESCRIPTOR *efi_map = nullptr;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, efi_map_cap,
                                   reinterpret_cast<void **>(&efi_map))))
        die("out of memory for EFI memory map");

    for (int attempt = 0; attempt < 8; attempt++) {
        UINTN map_size = efi_map_cap;
        if (EFI_ERROR(BS->GetMemoryMap(&map_size, efi_map, &map_key,
                                       &efi_desc_size, &desc_ver)))
            die("GetMemoryMap failed");

        uint32_t count = 0;
        UINTN entries = map_size / efi_desc_size;
        for (UINTN i = 0; i < entries && count < MAX_MEMMAP; i++) {
            auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(
                reinterpret_cast<unsigned char *>(efi_map) + i * efi_desc_size);
            bool usable = d->Type == EfiConventionalMemory ||
                          d->Type == EfiBootServicesCode ||
                          d->Type == EfiBootServicesData ||
                          d->Type == EfiLoaderCode;
            mm[count].addr = d->PhysicalStart;
            mm[count].size = d->NumberOfPages * EFI_PAGE_SIZE;
            mm[count].type = usable ? osv::boot_mem_type::usable
                                    : osv::boot_mem_type::reserved;
            count++;
        }
        bi->memmap_count = count;

        // A console UART is a device, not memory. Since 2026-10-14 the
        // Graviton3 firmware in eu-north-1 describes the console as a 16550
        // at 0x80000000 with byte access, which is exactly where its memory
        // map ends - the first byte past RAM - while the machine still has
        // the 16550 at 0x90a0000 with dword registers it always had.
        // Trusting the table hangs the kernel in the serial driver's first
        // poll before it can say anything. So on Amazon firmware an SPCR
        // address that the map covers, or that sits at the map's end, is
        // taken as the bug it is and replaced by the layout every Graviton
        // has shipped with. Other firmware is trusted as before.
        bool amazon = true;
        for (int i = 0; i < 6; i++)
            amazon = amazon && acpi_oem[i] == "AMAZON"[i];
        if (amazon && bi->uart_base) {
            // "Memory" here: inside an entry that is not MMIO, or exactly
            // where such an entry ends - the first byte past a RAM block,
            // which is what the broken table hands out.
            bool memory = false;
            for (UINTN i = 0; i < entries && !memory; i++) {
                auto d = reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(
                    reinterpret_cast<unsigned char *>(efi_map) + i * efi_desc_size);
                uint64_t start = d->PhysicalStart;
                uint64_t end = start + d->NumberOfPages * EFI_PAGE_SIZE;
                if (d->Type == EfiMemoryMappedIO ||
                    d->Type == EfiMemoryMappedIOPortSpace)
                    continue;
                memory = (bi->uart_base >= start && bi->uart_base <= end);
            }
            if (memory) {
                print("UEFI stub: SPCR console address is memory, not a "
                      "device; using the Graviton 16550 at 0x90a0000 (mmio32)\n");
                bi->uart_base = 0x90a0000ull;
                bi->uart_type = 0;
                bi->uart_access_width = 4;
            }
        }

        if (!EFI_ERROR(BS->ExitBootServices(image, map_key)))
            handoff(entry_phys, bi); // never returns
        // else: map changed, loop and try again
    }
    die("ExitBootServices failed repeatedly");
}
