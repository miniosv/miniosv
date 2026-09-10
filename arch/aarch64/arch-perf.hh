#pragma once

// ARMv8-A PMU back-end for osv/perf.hh.
// Targets Ampere-1a, Neoverse V1/V2 and Cortex-A76/A55.

#include "drivers/acpi.hh"
#include "exceptions.hh"
#include "osv/perf.hh"
#include <cstdint>
#include <functional>
#include <iostream>
#include <osv/interrupt.hh>

namespace perf {

inline constexpr uint32_t midr_fixed_mask = 0xFF0F'FFF0u;
inline constexpr uint32_t midr_cortex_a76 = 0x410F'D0B0u;
inline constexpr uint32_t midr_cortex_a55 = 0x410F'D050u;

// Number of hardware event counters (PMCR_EL0.N, bits 15:11).
inline uint32_t pmu_num_counters() {
  uint64_t pmcr;
  asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
  return (pmcr >> 11) & 0x1F;
}

inline uint32_t midr_read() {
  uint64_t midr;
  asm volatile("mrs %0, midr_el1" : "=r"(midr));
  return static_cast<uint32_t>(midr) & midr_fixed_mask;
}

inline bool is_midr(uint32_t to_check) { return midr_read() == to_check; }

// Identifies the cpu design. An asymmetric SoC (Cortex-A76 + A55) gives its
// clusters different counter counts and different event support, so anything
// cached per-PMU has to be keyed on this rather than probed once on the boot
// cpu.
inline uint32_t pmu_design_id() { return midr_read(); }

inline const char *midr_core_name(uint32_t midr) {
  switch (midr) {
  case midr_neoverse_v1:
    return "neoverse-v1";
  case midr_cortex_a76:
    return "cortex-a76";
  case midr_cortex_a55:
    return "cortex-a55";
  default:
    return "unknown";
  }
}

// SMCCC vendor hypervisor UID (function 0x8600FF01); KVM's UUID comes from
// Linux arch/arm64/kvm/hypercalls.c.
inline bool is_kvm_guest() {
  register uint64_t x0 asm("x0") = 0x8600FF01ull;
  register uint64_t x1 asm("x1");
  register uint64_t x2 asm("x2");
  register uint64_t x3 asm("x3");
  asm volatile("hvc #0" : "+r"(x0), "=r"(x1), "=r"(x2), "=r"(x3));
  return x0 == 0xb66fb428ull && x1 == 0xe911c52eull &&
         x2 == 0x564bcaa9ull && x3 == 0x743a004dull;
}

inline uint64_t pmceid0_read() {
  uint64_t v;
  asm volatile("mrs %0, pmceid0_el0" : "=r"(v));
  return v;
}

inline uint64_t pmceid1_read() {
  uint64_t v;
  asm volatile("mrs %0, pmceid1_el0" : "=r"(v));
  return v;
}

enum class event_support { yes, no, unknown };

inline event_support pmu_event_support(uint64_t value) {
  uint64_t pmceid;
  uint64_t bit;
  if (value < 0x20) {
    bit = value;
    pmceid = pmceid0_read();
  } else if (value < 0x40) {
    bit = value - 0x20;
    pmceid = pmceid1_read();
  } else if (value < 0x4000) {
    return event_support::unknown;
  } else if (value < 0x4020) {
    bit = value - 0x4000 + 32;
    pmceid = pmceid0_read();
  } else if (value < 0x4040) {
    bit = value - 0x4020 + 32;
    pmceid = pmceid1_read();
  } else {
    return event_support::unknown;
  }
  return ((1ull << bit) & pmceid) ? event_support::yes : event_support::no;
}

inline bool is_event_supported(uint64_t value) {
  switch (pmu_event_support(value)) {
  case event_support::yes:
    return true;
  case event_support::no:
    std::cerr << "Requested event 0x" << std::hex << value
              << " is not implemented by this PMU." << std::dec << std::endl;
    return false;
  default:
    std::cout << "Warning: Requested event 0x" << std::hex << value
              << " could not be checked for compatibility" << std::dec
              << std::endl;
    return true;
  }
}

// PMCR_EL0 control bits.
inline constexpr uint64_t pmcr_e = 1ull << 0;
inline constexpr uint64_t pmcr_p = 1ull << 1;
inline constexpr uint64_t pmcr_c = 1ull << 2;
inline constexpr uint64_t pmcr_d = 1ull << 3;
inline constexpr uint64_t pmcr_lc = 1ull << 6;
inline constexpr uint64_t pmcr_lp = 1ull << 7;

inline uint64_t pmcr_read() {
  uint64_t pmcr;
  asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
  return pmcr;
}

// Defined below; enable_pmu() probes the counter width while no counter has
// been reserved yet, because the probe writes PMEVCNTR0.
inline uint32_t pmu_probe_event_counter_width();

inline void enable_pmu() {
  // Clear all counter enables, interrupt enables and overflow flags: they are
  // unknown at reset, and a stale flag whose interrupt is still enabled keeps
  // the level-triggered PMU interrupt asserted forever.
  asm volatile("msr pmcntenclr_el0, %0\n\t"
               "msr pmintenclr_el1, %0\n\t"
               "msr pmovsclr_el0, %0\n\t"
               "isb" ::"r"((uint64_t)0xFFFFFFFF)
               : "memory");

  // LC|LP widen the counters to 64 bits; LP is RES0 before FEAT_PMUv3p5.
  uint64_t pmcr = (pmcr_read() | pmcr_e | pmcr_p | pmcr_c | pmcr_lc | pmcr_lp) &
                  ~pmcr_d;
  asm volatile("msr pmcr_el0, %0\n\tisb" ::"r"(pmcr) : "memory");

  static bool warned = false;
  if (!warned && !(pmcr_read() & pmcr_lp)) {
    warned = true;
    std::cout << "This PMU has no FEAT_PMUv3p5: the event counters are 32 bits "
                 "wide and wrap after 2^32 events."
              << std::endl;
  }

  // Probe here, while no counter has been handed out: the probe writes
  // PMEVCNTR0, and doing it lazily from Event::start() would corrupt a
  // measurement that is already running on counter 0.
  (void)pmu_probe_event_counter_width();
}

inline void pmc_stop(uint32_t counter) {
  uint64_t mask = counter == (1u << 31) ? (1ull << 31) : (1ull << counter);
  asm volatile("msr pmcntenclr_el0, %0" : : "r"(mask));
  asm volatile("isb" ::: "memory");
}

inline void pmc_write_counter(uint32_t counter, uint64_t value) {
  switch (counter) {
    // clang-format off
  case 0: asm volatile("msr pmevcntr0_el0, %0" : : "r"(value)); break;
  case 1: asm volatile("msr pmevcntr1_el0, %0" : : "r"(value)); break;
  case 2: asm volatile("msr pmevcntr2_el0, %0" : : "r"(value)); break;
  case 3: asm volatile("msr pmevcntr3_el0, %0" : : "r"(value)); break;
  case 4: asm volatile("msr pmevcntr4_el0, %0" : : "r"(value)); break;
  case 5: asm volatile("msr pmevcntr5_el0, %0" : : "r"(value)); break;
  case (1u << 31): asm volatile("msr pmccntr_el0, %0" : : "r"(value)); break;
    // clang-format on
  }
}

inline bool pmc_start_with_conf(uint32_t counter, uint32_t evt_sel,
                                uint64_t value) {
  // PMEVTYPER<n>_EL0.evtCount is bits [15:0]; the rest are exception filters.
  if (!is_event_supported(value & 0xFFFF))
    return false;

  // Write event config into the counter's pmevtyperN_el0.
  switch (evt_sel) {
    // clang-format off
  case 0: asm volatile("msr pmevtyper0_el0, %0" : : "r"(value)); break;
  case 1: asm volatile("msr pmevtyper1_el0, %0" : : "r"(value)); break;
  case 2: asm volatile("msr pmevtyper2_el0, %0" : : "r"(value)); break;
  case 3: asm volatile("msr pmevtyper3_el0, %0" : : "r"(value)); break;
  case 4: asm volatile("msr pmevtyper4_el0, %0" : : "r"(value)); break;
  case 5: asm volatile("msr pmevtyper5_el0, %0" : : "r"(value)); break;
    // clang-format on
  case (1u << 31):
    asm volatile("msr pmcntenset_el0, %0\n\t"
                 "isb" ::"r"((uint64_t)(1u << 31))
                 : "memory");
    return true;
  }
  asm volatile("isb" ::: "memory");
  asm volatile("msr pmcntenset_el0, %0" : : "r"(1ull << counter));
  asm volatile("isb" ::: "memory");
  return true;
}

inline uint64_t pmc_read(uint32_t counter) {
  uint64_t value;
  switch (counter) {
    // clang-format off
  case 0: asm volatile("mrs %0, pmevcntr0_el0" : "=r"(value)); break;
  case 1: asm volatile("mrs %0, pmevcntr1_el0" : "=r"(value)); break;
  case 2: asm volatile("mrs %0, pmevcntr2_el0" : "=r"(value)); break;
  case 3: asm volatile("mrs %0, pmevcntr3_el0" : "=r"(value)); break;
  case 4: asm volatile("mrs %0, pmevcntr4_el0" : "=r"(value)); break;
  case 5: asm volatile("mrs %0, pmevcntr5_el0" : "=r"(value)); break;
  case (1u << 31): asm volatile("mrs %0, pmccntr_el0" : "=r"(value)); break;
    // clang-format on
  }
  return value;
}

inline constexpr uint64_t pmc_int_enable = 0;
inline constexpr unsigned pmu_default_irq_id = 23;

using PMCIntHandle = ppi_interrupt *;

struct PMCOverflowAck {
  uint64_t mask;
};

inline uint64_t pmc_overflow_bit(uint32_t counter) {
  return counter == (1u << 31) ? (1ull << 31) : (1ull << counter);
}

inline PMCOverflowAck pmc_overflow_ack_conf(uint32_t counter) {
  uint64_t bit = pmc_overflow_bit(counter);
  asm volatile("msr pmintenset_el1, %0\n\tisb" ::"r"(bit) : "memory");
  return {bit};
}

inline void pmc_ack_overflow(PMCOverflowAck ack, PMCIntHandle) {
  asm volatile("msr pmovsclr_el0, %0\n\tisb" ::"r"(ack.mask) : "memory");
}

inline void pmc_ack_overflow_mask(uint64_t mask, PMCIntHandle) {
  asm volatile("msr pmovsclr_el0, %0\n\tisb" ::"r"(mask) : "memory");
}

inline uint32_t pmu_probe_event_counter_width() {
  static const uint32_t width = [] {
    uint64_t saved = pmc_read(0);
    pmc_write_counter(0, 1ull << 32);
    uint64_t back = pmc_read(0);
    pmc_write_counter(0, saved);
    return back ? 64u : 32u;
  }();
  return width;
}

// Where overflow is recorded, not the register size: PMCCNTR_EL0 counts 64-bit
// whatever LC says, only PMEVCNTR<n>_EL0 is 32-bit when LP is clear.
inline uint32_t pmc_overflow_width(uint32_t counter) {
  if (counter == (1u << 31))
    return (pmcr_read() & pmcr_lc) ? 64 : 32;
  return (pmcr_read() & pmcr_lp) ? pmu_probe_event_counter_width() : 32;
}

// PMOVSCLR_EL0 reads as the overflow status; writing clears the bits set.
inline uint64_t pmu_overflow_status() {
  uint64_t v;
  asm volatile("mrs %0, pmovsclr_el0" : "=r"(v));
  return v;
}

// Drop a pending overflow without touching the interrupt enables. Used before
// a handler is attached, so a flag left by the counter's previous owner cannot
// fire it immediately.
inline void pmc_clear_overflow(PMCOverflowAck ack) {
  asm volatile("msr pmovsclr_el0, %0\n\tisb" ::"r"(ack.mask) : "memory");
}

// The PMU interrupt is one PPI shared by every handler on the cpu, so a
// handler has to ask whether the overflow was its own.
inline bool pmc_overflow_pending(PMCOverflowAck ack) {
  return (pmu_overflow_status() & ack.mask) != 0;
}

// Stop taking overflow interrupts for the counters named by `mask`, leaving
// every other counter's interrupt enable alone.
inline void pmc_disable_overflow_int(uint64_t mask) {
  asm volatile("msr pmintenclr_el1, %0\n\tisb" ::"r"(mask) : "memory");
}

inline uint64_t pmc_period_value(uint32_t counter, uint64_t period) {
  uint32_t width = pmc_overflow_width(counter);
  return width >= 64 ? -period : (-period & ((1ull << width) - 1));
}

// The PMU overflow interrupt is a PPI whose id the firmware reports per cpu in
// the MADT GICC entries.
inline unsigned pmu_irq_id() {
  auto madt =
      reinterpret_cast<const acpi::madt *>(acpi::find_table(ACPI_SIG_MADT));
  if (!madt)
    return pmu_default_irq_id;
  auto subtable = reinterpret_cast<const char *>(madt + 1);
  auto madt_end = reinterpret_cast<const char *>(madt) + madt->header.length;
  while (subtable < madt_end) {
    auto s = reinterpret_cast<const acpi::madt_subtable *>(subtable);
    if (s->type == acpi::MADT_GICC) {
      auto gicc = reinterpret_cast<const acpi::madt_gicc *>(s);
      if ((gicc->flags & acpi::MADT_ENABLED) &&
          gicc->performance_interrupt_gsiv)
        return gicc->performance_interrupt_gsiv;
    }
    subtable += s->length;
  }
  return pmu_default_irq_id;
}

inline PMCIntHandle pmc_attach_overflow_handler(std::function<void()> handler) {
  return new ppi_interrupt(gic::irq_type::IRQ_TYPE_LEVEL, pmu_irq_id(),
                           std::move(handler));
}

// Disables only the counters named by `mask`. Clearing PMINTENSET wholesale
// would silently switch off the wrap-counting handler, which shares this
// interrupt and has no way to notice.
inline void pmc_detach_overflow_handler(PMCIntHandle irq, uint64_t mask) {
  pmc_disable_overflow_int(mask);
  delete irq;
}

namespace PERF_COUNT_HW {
using enum PMClass;

// Instruction architecturally executed, condition code check pass, software
// increment
constexpr PMCEvent SW_INCR = {0x0, CORE, "software-increase"};
// Attributable Level 1 instruction cache refill
constexpr PMCEvent L1I_CACHE_REFILL = {0x1, CORE, "l1i-cache-refill"};
// Attributable Level 1 instruction TLB refills
constexpr PMCEvent L1I_TLB_REFILL = {0x2, CORE, "l1i-tlb-refill"};
// Attributable Level 1 data cache refill
constexpr PMCEvent L1D_CACHE_REFILL = {0x3, CORE, "l1d-cache-refill"};
// Attributable Level 1 data cache access
constexpr PMCEvent L1D_CACHE = {0x4, CORE, "l1d-cache-access"};
// Attributable Level 1 data TLB refills
constexpr PMCEvent L1D_TLB_REFILL = {0x5, CORE, "l1d-tlb-refill"};
// Instruction architecturally executed, condition code check pass, load
constexpr PMCEvent LD_RETIRED = {0x6, CORE, "load-instructions"};
// Instruction architecturally executed, condition code check pass, store
constexpr PMCEvent ST_RETIRED = {0x7, CORE, "store-instructions"};
// Instruction architecturally executed
constexpr PMCEvent INSTRUCTIONS = {0x8, CORE, "instructions"};
// Exception Taken
constexpr PMCEvent EXC_TAKEN = {0x9, CORE, "exceptions-taken"};
// Instruction architecturally executed, condition code check pass,
// exception return
constexpr PMCEvent EXC_RETURN = {0xA, CORE, "exceptions-return"};
// Instruction architecturally executed, condition code check pass, write to
// CONTEXTIDR
constexpr PMCEvent CID_WRITE_RETIRED = {0xB, CORE, "context-id-writes"};
// Instruction architecturally executed, condition code check pass, software
// change of the PC
constexpr PMCEvent PC_WRITE_RETIRED = {0xC, CORE, "software-pc-writes"};
// Instruction architecturally executed, immediate branch
constexpr PMCEvent BR_IMMED_RETIRED = {0xD, CORE,
                                       "immediate-branch-instructions"};
// Instruction architecturally executed, condition code check pass,
// procedure return
constexpr PMCEvent BR_RETURN_RETIRED = {0xE, CORE,
                                        "procedure-return-instructions"};
// Instruction architecturally executed, condition code check pass,
// unaligned load or store
constexpr PMCEvent UNALIGNED_LDST_RETIRED = {0xF, CORE,
                                             "unaligned-loadstore-instruction"};
// Mispredicted or not predicted branch speculatively executed
constexpr PMCEvent BR_MIS_PRED = {0x10, CORE, "branch-misses-issued"};
// Cycle
constexpr PMCEvent CPU_CYCLES = {0x11, CYCLES, "cpu-cycles"};
// Predictable branch speculatively executed
constexpr PMCEvent BR_PRED = {0x12, CORE, "branch-predictions-issued"};
// Data memory access
constexpr PMCEvent MEM_ACCESS = {0x13, CORE, "memory-accesses"};
// Attributable Level 1 instruction cache access
constexpr PMCEvent L1I_CACHE = {0x14, CORE, "l1i-cache-accesses"};
// Attributable Level 1 data cache write-back
constexpr PMCEvent L1D_CACHE_WB = {0x15, CORE, "l1d-cache-writebacks"};
// Attributable Level 2 data cache access
constexpr PMCEvent L2D_CACHE = {0x16, CORE, "l2d-cache-accesses"};
// Attributable Level 2 data cache refill
constexpr PMCEvent L2D_CACHE_REFILL = {0x17, CORE, "l2d-cache-refills"};
// Attributable Level 2 data cache write-back
constexpr PMCEvent L2D_CACHE_WB = {0x18, CORE, "l2d-cache-writebacks"};
// Attributable Bus access
constexpr PMCEvent BUS_ACCESS = {0x19, CORE, "bus-accesses"};
// Local memory error
constexpr PMCEvent MEMORY_ERROR = {0x1A, CORE, "memory-errors"};
// Operation speculatively executed
constexpr PMCEvent INST_SPEC = {0x1B, CORE, "speculative-instructions"};
// Instruction architecturally executed, condition code check pass, write to
// TTBR
constexpr PMCEvent TTBR_WRITE_RETIRED = {0x1C, CORE, "ttbr-writes"};
// Bus cycle
constexpr PMCEvent BUS_CYCLES = {0x1D, CORE, "bus-cycles"};
// For an odd numbered counter, increment when an overflow occurs on the
// preceding even-numbered counter on the same PE
constexpr PMCEvent CHAIN = {0x1E, CORE, "chain"};
// Attributable Level 1 data cache allocation without refill
constexpr PMCEvent L1D_CACHE_ALLOCATE = {0x1F, CORE, "l1d-cache-allocations"};
// Attributable Level 2 data cache allocation without refill
constexpr PMCEvent L2D_CACHE_ALLOCATE = {0x20, CORE, "l2d-cache-allocations"};
// Instruction architecturally executed, branch
constexpr PMCEvent BRANCH_PREDICTION = {0x21, CORE, "branch-predictions"};
// Instruction architecturally executed, mispredicted branch
constexpr PMCEvent BRANCH_MISS = {0x22, CORE, "branch-misses"};
// No operation issued because of the frontend
constexpr PMCEvent STALL_FRONTEND = {0x23, CORE, "frontend-stalls"};
// No operation issued because of the backend
constexpr PMCEvent STALL_BACKEND = {0x24, CORE, "backend-stalls"};
// Attributable Level 1 data TLB access
constexpr PMCEvent L1D_TLB = {0x25, CORE, "l1d-tlb-accesses"};
// Attributable Level 1 instruction TLB access
constexpr PMCEvent L1I_TLB = {0x26, CORE, "l1i-tlb-accesses"};
// Attributable Level 2 instruction cache access
constexpr PMCEvent L2I_CACHE = {0x27, CORE, "l2i-cache-accesses"};
// Attributable Level 2 instruction cache refill
constexpr PMCEvent L2I_CACHE_REFILL = {0x28, CORE, "l2i-cache-refills"};
// Attributable Level 3 data cache allocation without refill
constexpr PMCEvent L3D_CACHE_ALLOCATE = {0x29, CORE, "l3d-cache-allocations"};
// Attributable Level 3 data cache refill
constexpr PMCEvent L3D_CACHE_REFILL = {0x2A, CORE, "l3d-cache-refills"};
// Attributable Level 3 data cache access
constexpr PMCEvent L3D_CACHE = {0x2B, CORE, "l3d-cache-accesses"};
// Attributable Level 3 data cache access write-back
constexpr PMCEvent L3D_CACHE_WB = {0x2C, CORE, "l3d-cache-writebacks"};
// Attributable Level 2 unified TLB refill
constexpr PMCEvent L2D_TLB_REFILL = {0x2D, CORE, "l2d-tlb-refills"};
// Attributable Level 2 unified TLB access
constexpr PMCEvent L2D_TLB = {0x2F, CORE, "l2d-tlb-accesses"};
// Access to another socket in a multi-socket system
constexpr PMCEvent REMOTE_ACCESS = {0x31, CORE, "remote-accesses"};
// Data TLB access with at least one translation table walk
constexpr PMCEvent DTLB_WALK = {0x34, CORE, "dtlb-walks"};
// Instruction TLB access with at least one translation table walk
constexpr PMCEvent ITLB_WALK = {0x35, CORE, "itlb-walks"};
// Last level data cache read
constexpr PMCEvent LL_CACHE = {0x36, CORE, "ll-cache-accesses"};
// Last level data cache read miss
constexpr PMCEvent LL_CACHE_MISS = {0x37, CORE, "ll-cache-misses"};
// Level 1 data cache read miss
constexpr PMCEvent L1D_CACHE_MISS = {0x39, CORE, "l1d-cache-misses"};
// Operation retired
constexpr PMCEvent OP_COMPLETE = {0x3A, CORE, "micro-operations-retired"};
// Operation speculated
constexpr PMCEvent OP_SPEC = {0x3B, CORE, "micro-operations-speculated"};
// No operation sent for execution
constexpr PMCEvent STALL = {0x3C, CORE, ""};
// No operation sent for execution on a slot because of the backend
constexpr PMCEvent STALL_OP_BACKEND = {0x3D, CORE, ""};
// No operation sent for execution on a slot because of the frontend
constexpr PMCEvent STALL_OP_FRONTEND = {0x3E, CORE, ""};
// No operation sent for execution on a slot
constexpr PMCEvent STALL_OP = {0x3F, CORE, ""};

// Level 2 data cache long-latency read miss (Armv8.4/Armv9 0x40xx range).
constexpr PMCEvent L2D_CACHE_LMISS_RD = {0x4009, CORE, "l2d-cache-misses"};

// Resolved on the cpu that reads it, not latched at static-init time: on an
// asymmetric SoC the two clusters implement different events, and a choice made
// once on the boot cpu would be wrong on the other cluster. Converts to
// PMCEvent, so it is still spelled like the plain event constants around it.
struct L2DCacheMissEvent {
  operator PMCEvent() const {
    return pmu_event_support(L2D_CACHE_LMISS_RD.bitmap) == event_support::yes
               ? L2D_CACHE_LMISS_RD
               : L2D_CACHE_REFILL;
  }
};
inline constexpr L2DCacheMissEvent L2D_CACHE_MISS{};
} // namespace PERF_COUNT_HW

inline void pmu_dump_state() {
  uint64_t pmcnten;
  asm volatile("mrs %0, pmcntenset_el0" : "=r"(pmcnten));
  std::cout << "PMU: core=" << midr_core_name(midr_read()) << " midr=0x"
            << std::hex << midr_read() << " pmcr=0x"
            << pmcr_read() << " pmceid0=0x" << pmceid0_read() << " pmceid1=0x"
            << pmceid1_read() << " pmcntenset=0x" << pmcnten << std::dec
            << " counters=" << pmu_num_counters()
            << " event-counter-width=" << pmu_probe_event_counter_width()
            << std::endl;

  std::cout << "PMU implemented events:";
  for (uint64_t e = 0; e < 0x40; ++e)
    if (pmu_event_support(e) == event_support::yes)
      std::cout << " 0x" << std::hex << e;
  for (uint64_t e = 0x4000; e < 0x4040; ++e)
    if (pmu_event_support(e) == event_support::yes)
      std::cout << " 0x" << std::hex << e;
  std::cout << std::dec << std::endl;
}

} // namespace perf
