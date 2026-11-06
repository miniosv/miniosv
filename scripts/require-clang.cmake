# Enforce the minimum clang++ version for every cmake configure we drive
# (build-llvm-libc.sh, build-libcxx.sh, build-compiler-rt.sh).
#
# Injected with -DCMAKE_PROJECT_INCLUDE=<this file>, so it runs right after
# project() once the compiler has been detected. Older clang miscompiles or
# rejects parts of the pinned llvm-project runtimes and the kernel flags we
# pass, so fail early with a clear message instead of deep inside the build.
include_guard(GLOBAL)

set(OSV_MIN_CLANG_VERSION 20)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "OSv requires clang++ >= ${OSV_MIN_CLANG_VERSION}; "
        "got ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}).")
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS OSV_MIN_CLANG_VERSION)
    message(FATAL_ERROR
        "OSv requires clang++ >= ${OSV_MIN_CLANG_VERSION}; "
        "got clang++ ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER}).")
endif()
