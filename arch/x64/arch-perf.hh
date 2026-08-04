#pragma once

// x86 PMU back-end for osv/perf.hh.
// Targets AMD Zen 4/5 and Intel Skylake and newer.

#include <cstdint>
#include <cstring>
#include <functional>

#include "apic.hh"
#include "exceptions.hh"
#include "osv/perf.hh"
#include "processor.hh"

namespace perf {

enum class CpuVendor { AMD, INTEL, UNKNOWN };

inline CpuVendor cpu_vendor() {
  processor::cpuid_result r = processor::cpuid(0);
  char v[13];
  std::memcpy(v + 0, &r.b, sizeof(r.b));
  std::memcpy(v + 4, &r.d, sizeof(r.d));
  std::memcpy(v + 8, &r.c, sizeof(r.c));
  v[12] = '\0';
  if (std::memcmp(v, "GenuineIntel", 12) == 0)
    return CpuVendor::INTEL;
  if (std::memcmp(v, "AuthenticAMD", 12) == 0)
    return CpuVendor::AMD;
  return CpuVendor::UNKNOWN;
}

inline bool is_intel() { return cpu_vendor() == CpuVendor::INTEL; }
inline bool is_amd() { return cpu_vendor() == CpuVendor::AMD; }

inline uint32_t pmu_num_counters() {
  if (is_intel()) {
    // Intel: CPUID leaf 0x0A EAX[15:8] = number of general-purpose PMCs.
    processor::cpuid_result r = processor::cpuid(0x0A);
    return (r.a >> 8) & 0xFF;
  }
  // AMD: prefer PerfMonV2 (CPUID Fn8000_0022h) which reports the counter
  // count directly. This is architectural from Zen 4 onwards.
  processor::cpuid_result ext_max = processor::cpuid(0x80000000);
  if (ext_max.a >= 0x80000022u) {
    processor::cpuid_result r = processor::cpuid(0x80000022);
    if (r.a & 0x1u)
      return r.b & 0xFu;
  }
  // Fall back: PerfCtrExtCore (CPUID Fn8000_0001h_ECX[23]) gates the extended
  // 6-counter MSR range; legacy AMDs expose 4.
  processor::cpuid_result r = processor::cpuid(0x80000001);
  return (r.c & (1u << 23)) ? 6 : 4;
}

// x86 has no MIDR register — shim for arch-neutral callers.
inline bool is_midr(uint32_t) { return false; }

// KVM-guest probing is only used by the ARM Neoverse V1 workaround.
inline bool is_kvm_guest() { return false; }

inline constexpr uint32_t intel_msr_perf_global_ctrl = 0x38Fu;

inline void enable_pmu() {
  // AMD PMU is enabled by default; nothing to do.
  if (!is_intel())
    return;
  uint32_t n = pmu_num_counters();
  if (n == 0)
    return;
  uint64_t mask = (n >= 32) ? 0xFFFF'FFFFull : ((1ull << n) - 1);
  processor::wrmsr(intel_msr_perf_global_ctrl, mask);
}

inline void pmc_stop(uint32_t evt_sel) { processor::wrmsr(evt_sel, 0); }

inline void pmc_write_counter(uint32_t ctr, uint64_t value) {
  processor::wrmsr(ctr, value);
}

inline void pmc_start_with_conf(uint32_t /*ctr*/, uint32_t evt_sel,
                                uint64_t value) {
  processor::wrmsr(evt_sel, value);
}

inline uint64_t pmc_read(uint32_t ctr) { return processor::rdmsr(ctr); }

inline constexpr uint64_t pmc_int_enable = 1ull << 20;
inline constexpr uint64_t pmc_counter_mask = (1ull << 48) - 1;
inline constexpr uint32_t amd_msr_perf_cntr_global_status_clr = 0xC0000302u;

inline uint64_t pmc_overflow_status_mask() {
  processor::cpuid_result ext_max = processor::cpuid(0x80000000);
  if (ext_max.a >= 0x80000022u && (processor::cpuid(0x80000022).a & 0x1u))
    return (1ull << pmu_num_counters()) - 1;
  return 0;
}

inline unsigned pmc_attach_overflow_handler(std::function<void()> handler) {
  unsigned vector = idt.register_handler(std::move(handler));
  processor::apic->write(processor::apicreg::LVTPC, vector);
  return vector;
}

inline void pmc_detach_overflow_handler(unsigned vector) {
  processor::apic->write(processor::apicreg::LVTPC, 1u << 16);
  idt.unregister_handler(vector);
}

inline void pmc_ack_overflow(uint64_t status_mask, unsigned vector) {
  if (status_mask)
    processor::wrmsr(amd_msr_perf_cntr_global_status_clr, status_mask);
  processor::apic->write(processor::apicreg::LVTPC, vector);
}

namespace PERF_COUNT_HW {
// PerfEvtSel encoding: bit22=EN, bit17=OS, bit16=USR, bits8-15=UMask,
// bits0-7=Event Select. Both vendors share this layout.
constexpr uint64_t encode(uint8_t event, uint8_t umask) {
  return 0x430000ull | (static_cast<uint64_t>(umask) << 8) | event;
}

using enum PMClass;

// Vendor-specific event catalogues. Codes and UMasks are taken from:
//   - AMD Family 19h (Zen 3/4) Processor Programming Reference, PMCx event
//     table.
//   - Intel SDM Vol 3B, chapters 19-20 (architectural + Skylake/Ice Lake
//     table). Events picked here work on Skylake and later.
// clang-format off
namespace AMD {
constexpr PMCEvent CPU_CYCLES        = {encode(0x76, 0x00), CORE, "cpu-cycles"};
constexpr PMCEvent INSTRUCTIONS      = {encode(0xC0, 0x00), CORE, "instructions"};
constexpr PMCEvent UOPS_RETIRED      = {encode(0xC1, 0x00), CORE, "uops-retired"};
constexpr PMCEvent BRANCH_PREDICTION = {encode(0xC2, 0x00), CORE, "branch-predictions"};
constexpr PMCEvent BRANCH_MISS       = {encode(0xC3, 0x00), CORE, "branch-misses"};
constexpr PMCEvent BRANCH_TAKEN      = {encode(0xC4, 0x00), CORE, "branch-taken"};
constexpr PMCEvent STALL_FRONTEND    = {encode(0xA9, 0x00), CORE, "stall-frontend"};
// PMCx029 (LsDispatch): loads+stores+load-stores dispatched to the LS unit —
// i.e. all L1 data cache accesses.
constexpr PMCEvent L1D_ACCESSES      = {encode(0x29, 0x07), CORE, "l1d-accesses"};
// PMCx045 (LsL1DTlbMiss). UMask 0xFF = any L1 DTLB miss;
// UMask 0xF0 = L1+L2 DTLB miss, i.e. a full page walk was required
// (parallel to Intel's *_WALK_COMPLETED).
constexpr PMCEvent DTLB_L1_MISS      = {encode(0x45, 0xFF), CORE, "dtlb-l1-miss"};
constexpr PMCEvent DTLB_WALK         = {encode(0x45, 0xF0), CORE, "dtlb-walk"};
constexpr PMCEvent TLB_FLUSHES       = {encode(0x78, 0xFF), CORE, "tlb-flushes"};
// PMCx064 (L2CacheReqStat) UMask 0x09 = cacheable read blocks, i.e. L2 read
// misses. AMD has no true L3-miss core PMC, so this backs LL_CACHE_MISS too.
constexpr PMCEvent L2D_CACHE_MISS   = {encode(0x64, 0x09), CORE, "l2d-cache-misses"};
} // namespace AMD

namespace INTEL {
constexpr PMCEvent CPU_CYCLES        = {encode(0x3C, 0x00), CORE, "cpu-cycles"};
constexpr PMCEvent INSTRUCTIONS      = {encode(0xC0, 0x00), CORE, "instructions"};
constexpr PMCEvent UOPS_RETIRED      = {encode(0xC2, 0x02), CORE, "uops-retired"};
constexpr PMCEvent BRANCH_PREDICTION = {encode(0xC4, 0x00), CORE, "branch-predictions"};
constexpr PMCEvent BRANCH_MISS       = {encode(0xC5, 0x00), CORE, "branch-misses"};
constexpr PMCEvent STALL_FRONTEND    = {encode(0x9C, 0x01), CORE, "stall-frontend"};
// MEM_LOAD_RETIRED.L1_MISS.
constexpr PMCEvent L1D_CACHE_MISS    = {encode(0xD1, 0x08), CORE, "l1d-cache-misses"};
// MEM_LOAD_RETIRED.L2_MISS.
constexpr PMCEvent L2D_CACHE_MISS    = {encode(0xD1, 0x10), CORE, "l2d-cache-misses"};
// MEM_INST_RETIRED.ALL_LOADS / ALL_STORES.
constexpr PMCEvent LOADS_RETIRED     = {encode(0xD0, 0x81), CORE, "loads-retired"};
constexpr PMCEvent STORES_RETIRED    = {encode(0xD0, 0x82), CORE, "stores-retired"};
// DTLB_LOAD_MISSES.WALK_COMPLETED / ITLB_MISSES.WALK_COMPLETED — count of
// fully-completed page walks (any page size).
constexpr PMCEvent DTLB_LOAD_WALK    = {encode(0x08, 0x0E), CORE, "dtlb-load-walk"};
constexpr PMCEvent ITLB_WALK         = {encode(0x85, 0x0E), CORE, "itlb-walk"};
// LONGEST_LAT_CACHE.REFERENCE / .MISS.
constexpr PMCEvent LL_CACHE         = {encode(0x2E, 0x4F), CORE, "ll-cache-accesses"};
constexpr PMCEvent LL_CACHE_MISS    = {encode(0x2E, 0x41), CORE, "ll-cache-misses"};
} // namespace INTEL
// clang-format on

// Vendor-neutral aliases picked at runtime. Events without a good match on
// both vendors are exposed only under AMD::/INTEL:: above.
inline const PMCEvent CPU_CYCLES        = is_intel() ? INTEL::CPU_CYCLES        : AMD::CPU_CYCLES;
inline const PMCEvent INSTRUCTIONS      = is_intel() ? INTEL::INSTRUCTIONS      : AMD::INSTRUCTIONS;
inline const PMCEvent UOPS_RETIRED      = is_intel() ? INTEL::UOPS_RETIRED      : AMD::UOPS_RETIRED;
inline const PMCEvent BRANCH_PREDICTION = is_intel() ? INTEL::BRANCH_PREDICTION : AMD::BRANCH_PREDICTION;
inline const PMCEvent BRANCH_MISS       = is_intel() ? INTEL::BRANCH_MISS       : AMD::BRANCH_MISS;
inline const PMCEvent STALL_FRONTEND    = is_intel() ? INTEL::STALL_FRONTEND    : AMD::STALL_FRONTEND;
inline const PMCEvent L2D_CACHE_MISS    = is_intel() ? INTEL::L2D_CACHE_MISS   : AMD::L2D_CACHE_MISS;
// AMD has no true L3-miss core PMC; the LLC alias falls back to L2 there.
inline const PMCEvent LL_CACHE_MISS     = is_intel() ? INTEL::LL_CACHE_MISS    : AMD::L2D_CACHE_MISS;
// AMD does not expose a direct L2/LL access counter through the core PMCs; on
// AMD this alias reports L1D accesses instead (matches pre-existing behaviour).
inline const PMCEvent LL_CACHE          = is_intel() ? INTEL::LL_CACHE         : AMD::L1D_ACCESSES;
} // namespace PERF_COUNT_HW

} // namespace perf
