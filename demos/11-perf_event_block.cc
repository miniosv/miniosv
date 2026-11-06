// This demo exemplifies hardware performance counter measurements on a single
// core.

#include <osv/power.hh>
#include <osv/perf.hh>
#include <osv/sched.hh>

constexpr unsigned iterations{1u << 20};

extern "C" void osv_app_main() {
  // Pin main thread to core
  sched::thread::pin(sched::cpu::current());

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
