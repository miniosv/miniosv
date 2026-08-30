/*
 * Copyright (C) 2014 Huawei Technologies Duesseldorf GmbH
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef SAFE_PTR_HH_
#define SAFE_PTR_HH_

#include <stdint.h>

#include <osv/compiler.h>

/*
 * Access an address that may not be mapped. A fault resumes at the fixup label
 * rather than reaching the fault handler, so the caller gets false instead of a
 * signal.
 *
 * The access has to be exactly sizeof(T) wide: a wider load can fault on a page
 * the caller never asked about, and a wider store runs past the destination
 * into whatever the compiler put after it.
 */

#define SAFE_PTR_FIXUP(body, ...)                                              \
    asm ("1: \n"                                                               \
         body                                                                  \
         "2: \n"                                                               \
         ".pushsection .text.fixup, \"ax\" \n"                                 \
         "3: \n"                                                               \
         "mov %w[ok], #0 \n"                                                   \
         "b 2b \n"                                                             \
         ".popsection \n"                                                      \
         ".pushsection .fixup, \"aw\" \n"                                      \
         ".quad 1b, 3b \n"                                                     \
         ".popsection\n"                                                       \
         __VA_ARGS__)

template <typename T>
static inline bool
safe_load(const T* potentially_bad_ptr, T& data)
{
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "safe_load covers the sizes one load instruction covers");

    // Read-write: the fixup path is the only writer, so the true set here has
    // to survive the asm when no fault happens.
    unsigned int ok = 1;
    uint64_t value = 0;

#define SAFE_LOAD(insn, width)                                                 \
    SAFE_PTR_FIXUP(insn " " width "[val], %[ptr] \n",                          \
                   : [val]"=&r"(value), [ok]"+r"(ok)                           \
                   : [ptr]"Q"(*potentially_bad_ptr)                            \
                   : "memory")

    if constexpr (sizeof(T) == 1) {
        SAFE_LOAD("ldrb", "%w");
    } else if constexpr (sizeof(T) == 2) {
        SAFE_LOAD("ldrh", "%w");
    } else if constexpr (sizeof(T) == 4) {
        SAFE_LOAD("ldr", "%w");
    } else {
        SAFE_LOAD("ldr", "%x");
    }
#undef SAFE_LOAD

    if (ok) {
        __builtin_memcpy(&data, &value, sizeof(T));
    }
    return ok;
}

template <typename T>
static inline bool
safe_store(const T* potentially_bad_ptr, const T& data)
{
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "safe_store covers the sizes one store instruction covers");

    unsigned int ok = 1;
    uint64_t value = 0;
    __builtin_memcpy(&value, &data, sizeof(T));

#define SAFE_STORE(insn, width)                                                \
    SAFE_PTR_FIXUP(insn " " width "[val], %[ptr] \n",                          \
                   : [ok]"+r"(ok), [ptr]"=Q"(*const_cast<T*>(potentially_bad_ptr)) \
                   : [val]"r"(value)                                           \
                   : "memory")

    if constexpr (sizeof(T) == 1) {
        SAFE_STORE("strb", "%w");
    } else if constexpr (sizeof(T) == 2) {
        SAFE_STORE("strh", "%w");
    } else if constexpr (sizeof(T) == 4) {
        SAFE_STORE("str", "%w");
    } else {
        SAFE_STORE("str", "%x");
    }
#undef SAFE_STORE

    return ok;
}

#endif /* SAFE_PTR_HH_ */
