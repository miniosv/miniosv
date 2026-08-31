// The OSv application.
//
// Unlike upstream OSv, the application is not a separate ELF shared object
// loaded at runtime from a filesystem image. It is compiled and statically
// linked directly into the kernel image (app.o in loader.elf). The kernel
// calls osv_app_main() once, after early initialization, on a dedicated thread.
//
// This is a minimal "hello world" example: it prints a greeting and powers
// the machine off so a run exits cleanly.

#include <osv/perf.hh>
#include <osv/power.hh>

extern "C" void osv_app_main() {
  printf("Hello, world from OSv!\n");
  osv::poweroff();
}
