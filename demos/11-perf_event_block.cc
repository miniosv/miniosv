// The OSv application.
//
// Unlike upstream OSv, the application is not a separate ELF shared object
// loaded at runtime from a filesystem image. It is compiled and statically
// linked directly into the kernel image (app.o in loader.elf). The kernel
// calls osv_app_main() once, after early initialization, on a dedicated thread.
//
// This demo exemplifies hardware performance counter measurements on a single
// core.

#include <osv/power.hh>
#include <osv/perf.hh>

constexpr unsigned iterations{1u << 20};

extern "C" void osv_app_main() {
  perf::PerfEvent perf;
  perf::BenchmarkParameters params;
  // Repeat some measurement 4 times
  for (uint64_t reps{0}; reps < 4; ++reps) {
    // Add index of current run to output
    params.setParam("run", reps);
    // Counters are started in constructor
    perf::PerfEventBlock perfBlock(perf, iterations, params, reps == 0);
    // Your benchmark goes here
    for (volatile unsigned i{0}; i < iterations; i += 1) {
    }
    // Counters are automatically stopped and printed on destruction of
    // perfEventBlock
  }
  osv::poweroff();
}
