// The OSv application.
//
// Unlike upstream OSv, the application is not a separate ELF shared object
// loaded at runtime from a filesystem image. It is compiled and statically
// linked directly into the kernel image (app.o in loader.elf). The kernel
// calls osv_app_main() once, after early initialization, on a dedicated thread.
//
// This demo exemplifies interrupt-based sampling leveraging hardware
// performance counter overflows. Every overflow records a call chain,
// similar to `perf record -g`.
// Resolve addresses with `llvm-symbolizer --obj=build/last/loader.elf <addr>`.

#include <osv/execinfo.hh>
#include <osv/perf.hh>
#include <osv/power.hh>

#include <cstdio>

constexpr unsigned iterations{1u << 20};

// One sample is taken every `sample_period` counter increments.
constexpr unsigned sample_period{1u << 19};

// Preallocated: the handler runs in interrupt context.
constexpr unsigned max_samples{4};
constexpr unsigned max_depth{10};
void *stacks[max_samples][max_depth];
int depths[max_samples];
unsigned n_samples{0};

// Interrupt Handler
void on_sample(exception_frame *frame) {
  if (n_samples == max_samples)
    return;
  stacks[n_samples][0] = frame->get_pc();
  depths[n_samples] = 1 + backtrace_safe(stacks[n_samples] + 1, max_depth - 1);
  n_samples += 1;
}

__attribute__((noinline)) unsigned inner() {
  unsigned sum{0};
  for (volatile unsigned i{0}; i < iterations; i += 1)
    sum += i;
  return sum;
}
__attribute__((noinline)) unsigned middle() { return inner() + 1; }
__attribute__((noinline)) unsigned outer() { return middle() + 1; }

extern "C" void osv_app_main() {
  perf::PMCSampler sampler{sample_period, on_sample,
                           perf::PERF_COUNT_HW::CPU_CYCLES};

  if (!sampler.start()) {
    printf("Could not start the sampler.\n");
    osv::poweroff();
  }

  for (unsigned rep{0}; rep < 8; rep += 1)
    outer();

  sampler.stop();

  for (unsigned s{0}; s < n_samples; s += 1) {
    printf("\nsample %u: %d frames\n", s, depths[s]);
    for (int f{0}; f < depths[s]; f += 1)
      printf("  #%d %p\n", f, stacks[s][f]);
  }

  osv::poweroff();
}
