/*
 * In-kernel test application.
 *
 * Built instead of the normal application with `make app=tests`. Like any
 * application it is statically linked into the kernel image and entered through
 * osv_app_main(). It drives the two correctness suites that live under apps/:
 *
 *   - os-features: walks the full list of OS facilities the slimmed kernel is
 *                  supposed to support and checks each behaves (conformance).
 *   - os-libc:     covers the plain C libc surface (string/stdlib/stdio/math,
 *                  qsort, setjmp, errno, calendar time) (conformance).
 *   - os-stress:   hammers threads, the allocator, TLS, synchronization, files
 *                  and IPC concurrently to shake out races (stress).
 *
 * Both used to be standalone PIE programs with their own main(); those entry
 * points are renamed (os_features_main / os_stress_main) and invoked here.
 */

#include <cstdio>
#include <osv/power.hh>

int os_features_main();
int os_libc_main();
int os_stress_main();
int os_iostream_main();
int os_memmove_main();
int os_backtrace_main(); 

extern "C" void osv_app_main()
{
    printf("\n######## OSv test application ########\n\n");

    int rc = 0;
    rc |= os_features_main();
    printf("\n");
    rc |= os_libc_main();
    printf("\n");
    rc |= os_backtrace_main();
    printf("\n"); 
    rc |= os_iostream_main();
    printf("\n");
    rc |= os_memmove_main();
    printf("\n");
    rc |= os_stress_main();

    printf("\n######## OSv test application: %s ########\n\n",
           rc ? "FAILURE" : "SUCCESS");

    osv::poweroff();
}
