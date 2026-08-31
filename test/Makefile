# The test application: a small driver (osv_tests.cc) plus the OS/libc++
# correctness and stress suites. Linked into the kernel instead of the normal
# application with:
#
#     make app=tests
#
# This file is included by the top-level Makefile, which compiles these sources
# with the kernel's own flags and links them into the kernel image.
#
#   - osv_tests.cc : the osv_app_main() driver that runs the suites below
#   - os-features  : OS facility conformance checks
#   - os-libc      : C libc surface conformance checks (string/stdlib/math/...)
#   - os-iostream  : C++ iostreams / localization checks (std::cout etc.)
#   - os-memmove   : memmove() overlap-correctness regression test
#   - os-string    : memset()/memcmp() correctness + throughput
#   - os-stress    : concurrency / allocator / TLS stress tests

app-objects =  test/osv_tests.o
app-objects += test/os-features.o
app-objects += test/os-libc.o
app-objects += test/os-iostream.o
app-objects += test/os-memmove.o
app-objects += test/os-string.o
app-objects += test/os-stress.o
