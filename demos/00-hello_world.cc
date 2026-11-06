#include <osv/perf.hh>
#include <osv/power.hh>

extern "C" void osv_app_main() {
  printf("Hello, world from OSv!\n");
  osv::poweroff();
}
