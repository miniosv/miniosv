#pragma once

// Perf-counter front-end. Targets recent chips only; no backward compat.
// Tested: x86 = AMD Zen 4/5, Intel Skylake; ARM = Ampere-1a, Neoverse V1/V2.
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
//   - enable_pmu, pmu_num_counters, pmc_overflow_width, is_midr
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

  uint64_t probe() const { return pmc_read(perfCtr); }

  void start_with_conf(uint64_t value, uint64_t initial = 0) {
    pmc_write_counter(perfCtr, initial);
    pmc_start_with_conf(perfCtr, perfEvtSel, value);
  }

  void stop() { pmc_stop(perfEvtSel); }
};

struct PMCSelect {
  explicit PMCSelect(std::vector<PMC> pmcs) : pmcs(std::move(pmcs)) {}

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

  size_t size() const { return pmcs.size(); }

  size_t size_of_x(PMClass x) const {
    size_t n = 0;
    for (const auto &pmc : pmcs)
      if (pmc.pmClass == x)
        ++n;
    return n;
  }

protected:
  std::vector<PMC> pmcs;
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
    pmc->start_with_conf(pmce.bitmap);
    before = pmc->probe();
  }

  void stop() {
    if (!pmc)
      return;
    after = pmc->probe();
    pmc->stop();
    pmcs.release(pmc);
  }

  uint64_t report() const { return valid ? after - before : 0; }

private:
  PMCSelect &pmcs;
  PMC *pmc = nullptr;
};

struct PerfEvent {
private:
  // Owned when no external selection is supplied. Sharing a PMCSelect between
  // PerfEvents lets multiple collections coordinate uncore counters.
  PMCSelectCore default_pmcs{make_default_core_pmcs()};

public:
  PMCSelect &pmcs = default_pmcs;
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
    for (auto &event : events)
      event.start();
    startTime = std::chrono::steady_clock::now();
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

struct PerfSampler {
  PerfSampler(uint64_t period, std::function<void(exception_frame *)> handler,
              PMCEvent pmce = PERF_COUNT_HW::CPU_CYCLES)
      : period(period), handler(std::move(handler)), pmce(pmce) {
    enable_pmu();
  }

  ~PerfSampler() { stop(); }

  bool start() {
    if (pmc || !(pmc = pmcs.acquire(pmce.pmClass)))
      return false;
    ack = pmc_overflow_ack_conf(pmc->perfCtr);
    vector = pmc_attach_overflow_handler([this] {
      pmc_write_counter(pmc->perfCtr, pmc_period_value(pmc->perfCtr, period));
      pmc_ack_overflow(ack, vector);
      handler(current_interrupt_frame);
    });
    pmc->start_with_conf(pmce.bitmap | pmc_int_enable,
                         pmc_period_value(pmc->perfCtr, period));
    return true;
  }

  void stop() {
    if (!pmc)
      return;
    pmc->stop();
    pmc_detach_overflow_handler(vector);
    pmcs.release(pmc);
    pmc = nullptr;
  }

private:
  PMCSelectCore pmcs{make_default_core_pmcs()};
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
