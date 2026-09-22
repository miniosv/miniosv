#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/cdefs.h>
#include <assert.h>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define ll_align(align) __attribute__((aligned(align)))

#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRId64 "lld"
#define PRIuS "zu"
#define PRIxS "zx"

/// Number of Bytes in cacheline
#define LLFREE_CACHE_SIZE 64u

#define LLFREE_FRAME_BITS 12
/// Size of a base frame
#define LLFREE_FRAME_SIZE (1u << LLFREE_FRAME_BITS)

/// Order of a huge frame
#define LLFREE_HUGE_ORDER 9
/// Maximum order that can be allocated
#define LLFREE_MAX_ORDER (LLFREE_HUGE_ORDER + 1u)

/// Num of bits of the larges atomic type of the architecture
#define LLFREE_ATOMIC_ORDER 6u
#define LLFREE_ATOMIC_SIZE (1u << LLFREE_ATOMIC_ORDER)

/// Number of frames in a child
#define LLFREE_CHILD_ORDER LLFREE_HUGE_ORDER
#define LLFREE_CHILD_SIZE (1u << LLFREE_CHILD_ORDER)

/// Number of frames in a tree
#define LLFREE_TREE_CHILDREN_ORDER 3u
#define LLFREE_TREE_CHILDREN (1u << LLFREE_TREE_CHILDREN_ORDER)
#define LLFREE_TREE_ORDER (LLFREE_HUGE_ORDER + LLFREE_TREE_CHILDREN_ORDER)
#define LLFREE_TREE_SIZE (1u << LLFREE_TREE_ORDER)

/// Enable reserve on free heuristic
#define LLFREE_ENABLE_FREE_RESERVE false
/// Allocate first from already install huge frames, before falling back to
/// evicted ones
#define LLFREE_PREFER_INSTALLED false

/// Minimal alignment the llc requires for its memory range
#define LLFREE_ALIGN (1u << LLFREE_MAX_ORDER << LLFREE_FRAME_BITS)

#define llfree_warn(fmt, ...) do { } while (0)

#ifdef VERBOSE
#define llfree_info_start()
#define llfree_info_cont(str, ...) printf(str, ##__VA_ARGS__)
#define llfree_info_end()
#define llfree_info(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define llfree_info(str, ...)
#define llfree_info_start()
#define llfree_info_cont(str, ...)
#define llfree_info_end()
#endif

#ifdef DEBUG
#define llfree_debug(str, ...) pr_debug(str, ##__VA_ARGS__)
#else
#define llfree_debug(str, ...)
#endif

void __attribute__((noinline)) llfree_panic(void);

static const int ATOM_LOAD_ORDER = __ATOMIC_ACQUIRE;
static const int ATOM_UPDATE_ORDER = __ATOMIC_ACQ_REL;
static const int ATOM_STORE_ORDER = __ATOMIC_RELEASE;

/* GCC compatibility */
#if !defined(__clang__) && defined(__GNUC__)

#define __c11_atomic_compare_exchange_strong(obj, expected, desired,           \
                                             order_success, order_failure)     \
  __extension__({                                                              \
    __auto_type __atomic_compare_exchange_ptr = (obj);                         \
    __typeof__((void)0,                                                        \
               *__atomic_compare_exchange_ptr) __atomic_compare_exchange_tmp = \
        (desired);                                                             \
    __atomic_compare_exchange(__atomic_compare_exchange_ptr, (expected),       \
                              &__atomic_compare_exchange_tmp, 0,               \
                              (order_success), (order_failure));               \
  })

#define __c11_atomic_compare_exchange_weak(obj, expected, desired,             \
                                           order_success, order_failure)       \
  __extension__({                                                              \
    __auto_type __atomic_compare_exchange_ptr = (obj);                         \
    __typeof__((void)0,                                                        \
               *__atomic_compare_exchange_ptr) __atomic_compare_exchange_tmp = \
        (desired);                                                             \
    __atomic_compare_exchange(__atomic_compare_exchange_ptr, (expected),       \
                              &__atomic_compare_exchange_tmp, 1,               \
                              (order_success), (order_failure));               \
  })

#define __c11_atomic_load(obj, order)                                          \
  __extension__({                                                              \
    __auto_type __atomic_load_ptr = (obj);                                     \
    __typeof__((void)0, *__atomic_load_ptr) __atomic_load_tmp;                 \
    __atomic_load(__atomic_load_ptr, &__atomic_load_tmp, (order));             \
    __atomic_load_tmp;                                                         \
  })

#define __c11_atomic_store(obj, val, order)                                    \
  __extension__({                                                              \
    __auto_type __atomic_store_ptr = (obj);                                    \
    __typeof__((void)0, *__atomic_store_ptr) __atomic_store_tmp = (val);       \
    __atomic_store(__atomic_store_ptr, &__atomic_store_tmp, (order));          \
  })

#define __c11_atomic_exchange(obj, val, order)                                 \
  __extension__({                                                              \
    __auto_type __atomic_exchange_ptr = (obj);                                 \
    __typeof__((void)0, *__atomic_exchange_ptr) __atomic_exchange_tmp = (val); \
    __atomic_exchange(__atomic_exchange_ptr, &__atomic_exchange_tmp,           \
                      &__atomic_exchange_tmp, (order));                        \
    __atomic_exchange_tmp;                                                     \
  })

#endif

/// Iterates over a Range between multiples of len starting at idx.
///
/// Starting at idx up to the next Multiple of len (exclusive). Then the next
/// step will be the highest multiple of len less than idx. (_base_idx)
/// Loop will end after len iterations.
/// code will be executed in each loop.
/// The current loop value can accessed by current_i
#define for_offsetted(idx, len)                                                \
  for (size_t _i = 0, _offset = (idx) % (len), _base_idx = (idx) - _offset,    \
              current_i = (idx);                                               \
       _i < (len);                                                             \
       _i = _i + 1, current_i = _base_idx + ((_i + _offset) % (len)))

/// Checks if `obj` contains `expected` and writes `disired` to it if so.
#define atom_cmp_exchange(obj, expected, desired)                              \
  ({                                                                           \
    llfree_debug("cmpxchg");                                                   \
    __c11_atomic_compare_exchange_strong((obj), (expected), (desired),         \
                                         ATOM_UPDATE_ORDER, ATOM_LOAD_ORDER);  \
  })
/// Checks if `obj` contains `expected` and writes `disired` to it if so.
#define atom_cmp_exchange_weak(obj, expected, desired)                         \
  ({                                                                           \
    llfree_debug("cmpxchg");                                                   \
    __c11_atomic_compare_exchange_weak((obj), (expected), (desired),           \
                                       ATOM_UPDATE_ORDER, ATOM_LOAD_ORDER);    \
  })

#define atom_load(obj)                                                         \
  ({                                                                           \
    llfree_debug("load");                                                      \
    __c11_atomic_load(obj, ATOM_LOAD_ORDER);                                   \
  })
#define atom_store(obj, val)                                                   \
  ({                                                                           \
    llfree_debug("store");                                                     \
    __c11_atomic_store(obj, val, ATOM_STORE_ORDER);                            \
  })

#define atom_swap(obj, desired)                                                \
  ({                                                                           \
    llfree_debug("swap");                                                      \
    __c11_atomic_exchange((obj), (desired), ATOM_UPDATE_ORDER);                \
  })

#define atom_update(atom_ptr, old_val, fn, ...)                                \
  ({                                                                           \
    /* NOLINTBEGIN */                                                          \
    llfree_debug("update");                                                    \
    bool _ret = false;                                                         \
    (old_val) = atom_load(atom_ptr);                                           \
    while (true) {                                                             \
      __typeof(old_val) value = (old_val);                                     \
      if (!(fn)(&value, ##__VA_ARGS__))                                        \
        break;                                                                 \
      if (atom_cmp_exchange_weak((atom_ptr), &(old_val), value)) {             \
        _ret = true;                                                           \
        break;                                                                 \
      }                                                                        \
    }                                                                          \
    _ret;                                                                      \
    /* NOLINTEND */                                                            \
  })
