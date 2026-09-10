#pragma once

// Perf-counter front-end. Targets recent chips only; no backward compat.
// Tested: x86 = AMD Zen 4/5, Intel Skylake;
//         ARM = Ampere-1a, Neoverse V1/V2, Cortex-A76/A55.
// Arch-specific PMU access lives in arch/$(arch)/arch-perf.hh.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace perf {

// Class of PMC. ARM has a dedicated cycle counter register; x86 only has CORE.
enum class PMClass { CORE, CYCLES };

// Event descriptor: bitmap goes into the counter's event-select register.
struct PMCEvent {
  uint64_t bitmap;
  PMClass pmClass;
  const char *name;
};

// Neoverse V1 MIDR (Implementer=0x41, Arch=0xF, Part=0xD40). Front-end owns
// this because it's used by the arch-neutral PMCSelectCore workaround.
inline constexpr uint32_t midr_neoverse_v1 = 0x410F'D400u;

} // namespace perf

// Arch back-end provides:
//   - pmc_read / pmc_write_counter / pmc_start_with_conf / pmc_stop
//   - enable_pmu, pmu_num_counters, pmc_overflow_width, pmu_design_id, is_midr
//   - x86 only: cpu_vendor, is_intel, is_amd
//   - namespace perf::PERF_COUNT_HW event catalogue
#include <arch-perf.hh>

namespace perf {

// Physical counter + its config register.
struct PMC {
  // Event-select register; on ARM also indexes pmevtyperN_el0.
  uint32_t perfEvtSel;
  // Counter-value register.
  uint32_t perfCtr;
  PMClass pmClass;
  // True when the counter is available for reservation.
  mutable std::atomic<bool> free{true};
  // Times this counter has overflowed since wrap counting was enabled. Only
  // meaningful when the selection is counting wraps; see enable_wrap_counting.
  mutable std::atomic<uint64_t> wraps{0};

  PMC(uint32_t perfEvtSel, uint32_t perfCtr, PMClass pmClass)
      : perfEvtSel(perfEvtSel), perfCtr(perfCtr), pmClass(pmClass) {}

  // std::atomic is not copyable; provide copy semantics manually.
  PMC(const PMC &pmc)
      : perfEvtSel(pmc.perfEvtSel), perfCtr(pmc.perfCtr), pmClass(pmc.pmClass) {
  }

  PMC &operator=(const PMC &pmc) {
    perfEvtSel = pmc.perfEvtSel;
    perfCtr = pmc.perfCtr;
    pmClass = pmc.pmClass;
    free.store(true);
    return *this;
  }

  uint64_t read() const { return pmc_read(perfCtr); }

  bool start_with_conf(uint64_t value, uint64_t initial = 0) {
    pmc_write_counter(perfCtr, initial);
    return pmc_start_with_conf(perfCtr, perfEvtSel, value);
  }

  void stop() { pmc_stop(perfEvtSel); }
};

struct PMCSelect {
  explicit PMCSelect(std::vector<PMC> pmcs)
      : design_id(pmu_design_id()), pmcs(std::move(pmcs)) {}

  bool erase_counter(uint32_t perfEvtSel, uint32_t perfCtr, PMClass pmClass) {
    auto it = std::find_if(pmcs.begin(), pmcs.end(), [&](const auto &pmc) {
      return pmc.perfEvtSel == perfEvtSel && pmc.perfCtr == perfCtr &&
             pmc.pmClass == pmClass;
    });
    if (it == pmcs.end())
      return false;
    pmcs.erase(it);
    return true;
  }

  bool erase_last_n_of_x(uint32_t n, PMClass x) {
    auto it = pmcs.end();
    while (n > 0 && it != pmcs.begin()) {
      --it;
      if (it->pmClass == x) {
        it = pmcs.erase(it);
        --n;
      }
    }
    return n == 0;
  }

  // Try to reserve any free PMC of the given class. Retries a bounded number
  // of times to tolerate transient contention with another thread.
  PMC *acquire(PMClass cls) {
    constexpr int max_retries = 7;
    for (int attempt = 0; attempt < max_retries; ++attempt) {
      for (auto &pmc : pmcs) {
        bool expected = true;
        if (pmc.pmClass == cls &&
            pmc.free.compare_exchange_strong(expected, false))
          return &pmc;
      }
    }
    return nullptr;
  }

  void release(PMC *pmc) { pmc->free.store(true); }

  // Software-extend the hardware counters to 64 bits. An Armv8 PMU without
  // FEAT_PMUv3p5 has 32-bit event counters, which wrap after a couple of
  // seconds of any event that tracks the clock. No-op where the hardware
  // already counts wide enough.
  //
  // The PMU is per-cpu and so is this handler, so the totals are only right if
  // the measured thread stays on one cpu.
  void enable_wrap_counting() {
    if (counting_wraps.load(std::memory_order_acquire))
      return;
    // Two threads can start counters at once; only one handler may be
    // attached. counting_wraps is published last, so arm_wrap_counting never
    // runs before wrap_irq is set.
    std::lock_guard<std::mutex> guard(wrap_lock);
    if (counting_wraps.load(std::memory_order_relaxed))
      return;
    if (pmc_overflow_width(0) > 32)
      return;
    wrap_irq = pmc_attach_overflow_handler([this] {
      // One PMU interrupt is shared by every handler on the cpu: the other
      // clusters' tables and any PMCSampler are attached to it too. Touch only
      // the counters this table armed, and ack only those -- acking the whole
      // status word would swallow a sampler's overflow.
      if (pmu_design_id() != design_id)
        return;
      uint64_t mine =
          pmu_overflow_status() & armed_mask.load(std::memory_order_relaxed);
      if (!mine)
        return;
      for (auto &pmc : pmcs) {
        if (mine & pmc_overflow_bit(pmc.perfCtr))
          pmc.wraps.fetch_add(1, std::memory_order_relaxed);
      }
      pmc_ack_overflow_mask(mine, wrap_irq);
    });
    counting_wraps.store(true, std::memory_order_release);
  }

  bool counts_wraps() const {
    return counting_wraps.load(std::memory_order_acquire);
  }

  // Arm the overflow interrupt for one counter. Called once the counter is
  // running and zeroed, because arming it earlier would count wraps nobody is
  // measuring. The flag left behind by the counter's previous owner has to go
  // first: the PMU interrupt is level-triggered, so arming on top of a stale
  // flag keeps it asserted.
  void arm_wrap_counting(PMC *pmc) {
    if (!counts_wraps())
      return;
    uint64_t bit = pmc_overflow_bit(pmc->perfCtr);
    pmc_ack_overflow_mask(bit, wrap_irq);
    armed_mask.fetch_or(bit, std::memory_order_relaxed);
    pmc_overflow_ack_conf(pmc->perfCtr);
  }

  // Stop taking interrupts for a counter nobody is measuring any more.
  void disarm_wrap_counting(PMC *pmc) {
    if (!counts_wraps())
      return;
    uint64_t bit = pmc_overflow_bit(pmc->perfCtr);
    armed_mask.fetch_and(~bit, std::memory_order_relaxed);
    pmc_disable_overflow_int(bit);
  }

  size_t size() const { return pmcs.size(); }

  size_t size_of_x(PMClass x) const {
    size_t n = 0;
    for (const auto &pmc : pmcs)
      if (pmc.pmClass == x)
        ++n;
    return n;
  }

protected:
  // MIDR (ARM) or CPUID signature (x86) of the cpu this table was built on.
  uint32_t design_id;
  std::vector<PMC> pmcs;

private:
  std::mutex wrap_lock;
  PMCIntHandle wrap_irq{};
  std::atomic<bool> counting_wraps{false};
  // Counters currently armed for wrap counting, as PMOVSCLR bits.
  std::atomic<uint64_t> armed_mask{0};
};

// Core-local counter selection. Adjusts the counter count at runtime because
// AWS slices the number of counters per VM.
struct PMCSelectCore : PMCSelect {
  explicit PMCSelectCore(std::vector<PMC> pmcs) : PMCSelect(std::move(pmcs)) {
    uint32_t act_ctrs = pmu_num_counters();
    uint32_t exp_ctrs = 0;
    for (const auto &c : this->pmcs)
      if (c.pmClass == PMClass::CORE)
        ++exp_ctrs;

    if (act_ctrs < exp_ctrs) {
      std::cout << "Expected " << exp_ctrs << " hardware counters, but only "
                << act_ctrs << " are available.\n Assuming the first "
                << act_ctrs << " counters to be valid." << std::endl;
      erase_last_n_of_x(exp_ctrs - act_ctrs, PMClass::CORE);
    } else if (is_midr(midr_neoverse_v1) && is_kvm_guest()) {
      std::cout << "Detected ARM Neoverse V1 under KVM: disabling counter 0 "
                   "since it doesn't work reliably in this configuration. "
                   "You have "
                << act_ctrs - 1 << " counters available" << std::endl;
      erase_counter(0, 0, PMClass::CORE);
    }
  }
};

// Default core-local counter list. Counter MSRs are vendor-specific on x86
// (resolved at runtime via CPUID); ARM uses fixed indices.
inline std::vector<PMC> make_default_core_pmcs() {
  std::vector<PMC> pmcs;
#if defined(__x86_64__)
  uint32_t n = pmu_num_counters();
  if (is_intel()) {
    // IA32_A_PMCx when the CPU supports full-width counter writes.
    uint32_t ctr = intel_full_width_write() ? 0x4C1u : 0xC1u;
    for (uint32_t i = 0; i < n; ++i)
      pmcs.emplace_back(0x186u + i, ctr + i, PMClass::CORE);
  } else {
    // AMD "extended" core PMC range (Zen and later): counters live at
    // MSRC001_0200h + 2n / MSRC001_0201h + 2n.
    for (uint32_t i = 0; i < n; ++i)
      pmcs.emplace_back(0xC0010200u + 2 * i, 0xC0010201u + 2 * i,
                        PMClass::CORE);
  }
#elif defined(__aarch64__)
  // Armv8 PMUv3 usually has 6 counters (ids 0-5).
  for (uint32_t i = 0; i < 6; ++i)
    pmcs.emplace_back(i, i, PMClass::CORE);
  pmcs.emplace_back(1u << 31, 1u << 31, PMClass::CYCLES);
#endif
  return pmcs;
}

// The counters are hardware, so there has to be a single reservation table:
// a second PMCSelectCore would hand out counters that are already in use, and
// the two users would silently overwrite each other's event selection.
//
// One table per cpu design rather than one per machine, because an asymmetric
// part -- Cortex-A76 + A55, or an x86 hybrid -- gives its clusters different
// counter counts and different event support. A single table built on the boot
// cpu would hand the other cluster a counter it does not implement. Cores of
// the same design still share a table, which is conservative but never unsafe.
//
// Resolved on the calling cpu, so a PerfEvent belongs to the cluster it was
// constructed on -- the same pinning the counters themselves already require.
inline PMCSelectCore &default_core_pmcs() {
  static std::mutex lock;
  static std::map<uint32_t, std::unique_ptr<PMCSelectCore>> tables;

  std::lock_guard<std::mutex> guard(lock);
  auto &table = tables[pmu_design_id()];
  if (!table)
    table = std::make_unique<PMCSelectCore>(make_default_core_pmcs());
  return *table;
}

// ---------------- High-level measurement API ----------------

struct Event {
  PMCEvent pmce;

  uint64_t before;
  uint64_t after;

  bool valid = true;

  Event(PMCEvent pmce, PMCSelect &pmcs) : pmce(pmce), pmcs(pmcs) {}

  void start() {
    pmc = pmcs.acquire(pmce.pmClass);
    if (!pmc) {
      std::cerr << "[ERROR] All hardware counters are occupied ("
                << pmcs.size_of_x(pmce.pmClass) << "/"
                << pmcs.size_of_x(pmce.pmClass) << "). Event " << pmce.name
                << " will not be measured." << std::endl;
      valid = false;
      return;
    }
    width = pmc_overflow_width(pmc->perfCtr);
    polled = 0;
    if (!pmc->start_with_conf(pmce.bitmap)) {
      // The event is not implemented here; the counter stays disabled and
      // would otherwise report a plausible-looking zero.
      pmcs.release(pmc);
      pmc = nullptr;
      valid = false;
      return;
    }
    // Armed only now that start_with_conf has zeroed the counter: arming it
    // beforehand would fold in a wrap of whatever the previous owner left.
    wraps_before = pmc->wraps.load(std::memory_order_relaxed);
    pmcs.arm_wrap_counting(pmc);
    before = last = pmc->read();
  }

  // Read the counter and fold in an overflow if it has gone backwards. Cheap
  // enough to call from a timer (one system-register read per counter). Must
  // be called more often than the counter can wrap -- on a 32-bit PMU counting
  // cycles, that is every couple of seconds.
  void poll() {
    if (!pmc || width >= 64)
      return;
    uint64_t now = pmc->read();
    if (now < last)
      polled += 1ull << width;
    last = now;
  }

  void stop() {
    if (!pmc)
      return;
    poll();
    after = pmc->read();
    if (after < last)
      polled += 1ull << width;
    pmc->stop();
    pmcs.disarm_wrap_counting(pmc);
    wraps = pmc->wraps.load(std::memory_order_relaxed) - wraps_before;
    counted_wraps = pmcs.counts_wraps();
    pmcs.release(pmc);
    // With the overflows added back a counter only ever counts up, so this is
    // not one: it is a read of a different cpu's PMU, or of a counter that was
    // never enabled.
    if (!counted_wraps && !polled && after < before)
      valid = false;
  }

  uint64_t report() const {
    if (!valid)
      return 0;
    // Overflows come from the interrupt where KVM delivers it and from polling
    // otherwise; the two are alternatives, so taking the larger picks whichever
    // was actually working. A counter that is already 64 bits wide cannot wrap,
    // and shifting by its full width would be undefined.
    uint64_t wrapped = width < 64 ? wraps << width : 0;
    uint64_t overflowed = std::max(polled, wrapped);
    return overflowed + after - before;
  }

private:
  PMCSelect &pmcs;
  PMC *pmc = nullptr;
  uint64_t wraps_before = 0;
  uint64_t wraps = 0;
  uint64_t last = 0;
  uint64_t polled = 0;
  uint32_t width = 64;
  bool counted_wraps = false;
};

struct PerfEvent {
  // The machine-wide selection unless an external one is supplied. Sharing a
  // PMCSelect between PerfEvents lets multiple collections coordinate uncore
  // counters.
  PMCSelect &pmcs = default_core_pmcs();
  // Must not exceed the number of hardware counters in `pmcs`.
  std::vector<Event> events;

  std::chrono::time_point<std::chrono::steady_clock> startTime;
  std::chrono::time_point<std::chrono::steady_clock> stopTime;

  // Uses this instance's own core-local counter set.
  PerfEvent(bool set_default_counters = true) {
    enable_pmu();

    if (set_default_counters) {
      registerCounter(PERF_COUNT_HW::CPU_CYCLES);
      registerCounter(PERF_COUNT_HW::INSTRUCTIONS);
      registerCounter(PERF_COUNT_HW::L2D_CACHE_MISS);
      registerCounter(PERF_COUNT_HW::BRANCH_MISS);
    }
  }

  // Shares a counter selection with other PerfEvents.
  PerfEvent(PMCSelect &pmcSelect) : pmcs(pmcSelect) { enable_pmu(); }

  void registerCounter(PMCEvent pmce) { events.emplace_back(pmce, pmcs); }

  void registerCounter(PMCEvent pmce, const char *name) {
    pmce.name = name;
    registerCounter(pmce);
  }

  void registerCounter(uint64_t bitmap, PMClass pmClass, const char *name) {
    registerCounter({bitmap, pmClass, name});
  }

  void startCounters() {
    pmcs.enable_wrap_counting();
    for (auto &event : events)
      event.start();
    startTime = std::chrono::steady_clock::now();
  }

  // Fold in any counter that has overflowed since the last call. Only needed
  // where the PMU overflow interrupt does not reach the guest -- report() takes
  // whichever of the two mechanisms saw more wraps -- but on a 32-bit PMU it is
  // the only thing standing between a long run and a wrong answer. Call it from
  // a timer, more often than the counter can wrap; see Event::poll.
  void pollCounters() {
    for (auto &event : events)
      event.poll();
  }

  void stopCounters() {
    stopTime = std::chrono::steady_clock::now();
    for (auto &event : events)
      event.stop();
  }

  double getDuration() const {
    return std::chrono::duration<double>(stopTime - startTime).count();
  }

  size_t getDurationUs() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(stopTime -
                                                                 startTime)
        .count();
  }

  // Returns NaN if either instructions or cycles isn't being counted.
  double getIPC() const {
    double res = getCounter(PERF_COUNT_HW::INSTRUCTIONS.name) /
                 getCounter(PERF_COUNT_HW::CPU_CYCLES.name);
    return res > 0 ? res : NAN;
  }

  double getCounter(const char *name) const {
    for (const auto &event : events)
      if (event.pmce.name == name)
        return event.report();
    return -1;
  }

  static void printCounter(std::ostream &headerOut, std::ostream &dataOut,
                           std::string name, std::string counterValue,
                           bool addComma = true) {
    auto width = std::max(name.length(), counterValue.length());
    headerOut << std::setw(static_cast<int>(width)) << name
              << (addComma ? "," : "") << " ";
    dataOut << std::setw(static_cast<int>(width)) << counterValue
            << (addComma ? "," : "") << " ";
  }

  template <typename T>
  static void printCounter(std::ostream &headerOut, std::ostream &dataOut,
                           std::string name, T counterValue,
                           bool addComma = true) {
    std::stringstream stream;
    stream << std::fixed << std::setprecision(2) << counterValue;
    PerfEvent::printCounter(headerOut, dataOut, name, stream.str(), addComma);
  }

  void printReport(std::ostream &out, uint64_t normalizationConstant) {
    std::stringstream header;
    std::stringstream data;
    printReport(header, data, normalizationConstant);
    out << header.str() << std::endl;
    out << data.str() << std::endl;
  }

  void printReport(std::ostream &headerOut, std::ostream &dataOut,
                   uint64_t normalizationConstant) {
    if (events.empty())
      return;

    printCounter(headerOut, dataOut, "duration", getDuration());
    for (const auto &event : events) {
      printCounter(headerOut, dataOut, event.pmce.name,
                   event.report() / static_cast<double>(normalizationConstant));
    }

    printCounter(headerOut, dataOut, "scale", normalizationConstant);

    // Derived metrics.
    printCounter(headerOut, dataOut, "IPC", getIPC());
  }

  template <typename T>
  static void printCounterVertical(std::ostream &infoOut, std::string name,
                                   T counterValue, int eNameWidth) {
    std::stringstream stream;
    stream << std::fixed << std::setprecision(2) << counterValue;
    infoOut << std::setw(eNameWidth) << std::left << name << " : "
            << stream.str() << std::endl;
  }

  void printReportVertical(std::ostream &out, uint64_t normalizationConstant) {
    std::stringstream info;
    printReportVerticalUtil(info, normalizationConstant);
    out << info.str() << std::endl;
  }

  void printReportVerticalUtil(std::ostream &infoOut,
                               uint64_t normalizationConstant) {
    if (events.empty())
      return;

    // Widest event name; minimum width is that of "scale".
    int eNameWidth = 5;
    for (const auto &event : events) {
      eNameWidth = std::max(
          static_cast<int>(std::char_traits<char>::length(event.pmce.name)),
          eNameWidth);
    }

    printCounterVertical(infoOut, "duration", getDuration(), eNameWidth);
    for (const auto &event : events) {
      printCounterVertical(infoOut, event.pmce.name,
                           event.report() /
                               static_cast<double>(normalizationConstant),
                           eNameWidth);
    }

    printCounterVertical(infoOut, "scale", normalizationConstant, eNameWidth);

    // Derived metrics.
    printCounterVertical(infoOut, "IPC", getIPC(), eNameWidth);
  }
};

struct PMCSampler {
  PMCSampler(uint64_t period, std::function<void(exception_frame *)> handler,
             PMCEvent pmce = PERF_COUNT_HW::CPU_CYCLES)
      : period(period), handler(std::move(handler)), pmce(pmce) {
    enable_pmu();
  }

  ~PMCSampler() { stop(); }

  bool start() {
    if (pmc || !(pmc = pmcs.acquire(pmce.pmClass)))
      return false;
    ack = pmc_overflow_ack_conf(pmc->perfCtr);
    // Drop a flag left by the counter's previous owner before the handler goes
    // in: the PMU interrupt is level-triggered, so a stale flag would fire the
    // handler the moment it is attached.
    pmc_clear_overflow(ack);
    vector = pmc_attach_overflow_handler([this] {
      // The interrupt is shared with the wrap-counting handler and with the
      // other clusters' tables, so it fires for overflows that are not ours.
      if (!pmc_overflow_pending(ack))
        return;
      pmc_write_counter(pmc->perfCtr, pmc_period_value(pmc->perfCtr, period));
      pmc_ack_overflow(ack, vector);
      handler(current_interrupt_frame);
    });
    if (!pmc->start_with_conf(pmce.bitmap | pmc_int_enable,
                              pmc_period_value(pmc->perfCtr, period))) {
      pmc_detach_overflow_handler(vector, ack.mask);
      pmcs.release(pmc);
      pmc = nullptr;
      return false;
    }
    return true;
  }

  void stop() {
    if (!pmc)
      return;
    pmc->stop();
    pmc_detach_overflow_handler(vector, ack.mask);
    pmcs.release(pmc);
    pmc = nullptr;
  }

private:
  PMCSelect &pmcs = default_core_pmcs();
  uint64_t period;
  std::function<void(exception_frame *)> handler;
  PMCEvent pmce;
  PMC *pmc = nullptr;
  PMCIntHandle vector{};
  PMCOverflowAck ack{};
};

struct BenchmarkParameters {

  void setParam(const std::string &name, const std::string &value) {
    params[name] = value;
  }

  void setParam(const std::string &name, const char *value) {
    params[name] = value;
  }

  template <typename T> void setParam(const std::string &name, T value) {
    setParam(name, std::to_string(value));
  }

  void printParams(std::ostream &header, std::ostream &data) {
    for (auto &p : params)
      PerfEvent::printCounter(header, data, p.first, p.second);
  }

  BenchmarkParameters(std::string name = "") {
    if (name.length())
      setParam("name", name);
  }

private:
  std::map<std::string, std::string> params;
};

struct PerfRef {
  union {
    PerfEvent instance;
    PerfEvent *pointer;
  };
  bool has_instance;

  PerfRef() : instance(), has_instance(true) {}
  PerfRef(PerfEvent *ptr) : pointer(ptr), has_instance(false) {}
  PerfRef(const PerfRef &) = delete;

  ~PerfRef() {
    if (has_instance)
      instance.~PerfEvent();
  }

  PerfEvent *operator->() { return has_instance ? &instance : pointer; }
};

struct PerfEventBlock {
  PerfRef e;
  uint64_t scale;
  BenchmarkParameters parameters;
  bool printHeader;

  PerfEventBlock(uint64_t scale = 1, BenchmarkParameters params = {},
                 bool printHeader = true)
      : scale(scale), parameters(params), printHeader(printHeader) {
    e->startCounters();
  }

  PerfEventBlock(PerfEvent &perf, uint64_t scale = 1,
                 BenchmarkParameters params = {}, bool printHeader = true)
      : e(&perf), scale(scale), parameters(params), printHeader(printHeader) {
    e->startCounters();
  }

  ~PerfEventBlock() {
    e->stopCounters();
    std::stringstream header;
    std::stringstream data;
    parameters.printParams(header, data);
    PerfEvent::printCounter(header, data, "time sec", e->getDuration());
    PerfEvent::printCounter(header, data, "micros", e->getDurationUs());
    PerfEvent::printCounter(header, data, "millis",
                            static_cast<double>(e->getDurationUs()) / 1000);
    e->printReport(header, data, scale);
    if (printHeader)
      std::cout << header.str() << std::endl;
    std::cout << data.str() << std::endl;
  }
};

} // namespace perf
