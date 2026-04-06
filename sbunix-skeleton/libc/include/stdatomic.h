#ifndef _STDATOMIC_H
#define _STDATOMIC_H

/*
 * stdatomic.h -- C11 atomic operations for SBUnix.
 *
 * SBUnix is single-threaded per process, so all atomic operations are
 * plain loads/stores with compiler barriers (no hardware fences needed).
 * Lock-free constants are all 2 (always lock-free) since there is no
 * contention.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * Memory ordering enum
 * =================================================================== */

typedef enum {
  memory_order_relaxed,
  memory_order_consume,
  memory_order_acquire,
  memory_order_release,
  memory_order_acq_rel,
  memory_order_seq_cst
} memory_order;

/* ===================================================================
 * Lock-free constants — all 2 (always lock-free on single-threaded OS)
 * =================================================================== */

#define ATOMIC_BOOL_LOCK_FREE      2
#define ATOMIC_CHAR_LOCK_FREE      2
#define ATOMIC_CHAR16_T_LOCK_FREE  2
#define ATOMIC_CHAR32_T_LOCK_FREE  2
#define ATOMIC_WCHAR_T_LOCK_FREE   2
#define ATOMIC_SHORT_LOCK_FREE     2
#define ATOMIC_INT_LOCK_FREE       2
#define ATOMIC_LONG_LOCK_FREE      2
#define ATOMIC_LLONG_LOCK_FREE     2
#define ATOMIC_POINTER_LOCK_FREE   2

/* ===================================================================
 * Atomic type aliases — C11 7.17.6
 *
 * On a single-threaded system the _Atomic qualifier is a no-op;
 * we typedef each to the underlying scalar type directly.
 * =================================================================== */

typedef _Bool              atomic_bool;
typedef char               atomic_char;
typedef signed char        atomic_schar;
typedef unsigned char      atomic_uchar;
typedef short              atomic_short;
typedef unsigned short     atomic_ushort;
typedef int                atomic_int;
typedef unsigned int       atomic_uint;
typedef long               atomic_long;
typedef unsigned long      atomic_ulong;
typedef long long          atomic_llong;
typedef unsigned long long atomic_ullong;

/* <stdint.h> exact-width aliases */
typedef int8_t             atomic_int_least8_t;
typedef uint8_t            atomic_uint_least8_t;
typedef int16_t            atomic_int_least16_t;
typedef uint16_t           atomic_uint_least16_t;
typedef int32_t            atomic_int_least32_t;
typedef uint32_t           atomic_uint_least32_t;
typedef int64_t            atomic_int_least64_t;
typedef uint64_t           atomic_uint_least64_t;

typedef int8_t             atomic_int_fast8_t;
typedef uint8_t            atomic_uint_fast8_t;
typedef int16_t            atomic_int_fast16_t;
typedef uint16_t           atomic_uint_fast16_t;
typedef int32_t            atomic_int_fast32_t;
typedef uint32_t           atomic_uint_fast32_t;
typedef int64_t            atomic_int_fast64_t;
typedef uint64_t           atomic_uint_fast64_t;

typedef intptr_t           atomic_intptr_t;
typedef uintptr_t          atomic_uintptr_t;
typedef size_t             atomic_size_t;
typedef ptrdiff_t          atomic_ptrdiff_t;
typedef intmax_t           atomic_intmax_t;
typedef uintmax_t          atomic_uintmax_t;

/* wchar_t — defined as int on RISC-V */
typedef int                atomic_wchar_t;

/* char16_t / char32_t (C11 <uchar.h> types) */
typedef uint16_t           atomic_char16_t;
typedef uint32_t           atomic_char32_t;

/* ===================================================================
 * atomic_flag — test-and-set type (C11 7.17.8)
 * =================================================================== */

typedef struct {
  _Bool value;
} atomic_flag;

#define ATOMIC_FLAG_INIT { 0 }

/* ===================================================================
 * Initialization
 * =================================================================== */

/* ATOMIC_VAR_INIT: identity on single-threaded system */
#define ATOMIC_VAR_INIT(value) (value)

/* atomic_init: plain store */
#define atomic_init(obj, desired) (void)(*(obj) = (desired))

/* ===================================================================
 * Compiler barrier — prevents the compiler from reordering across it.
 * No hardware fence needed on a single-threaded system.
 * =================================================================== */

#define __atomic_compiler_barrier() __asm__ volatile("" ::: "memory")

/* ===================================================================
 * Fence operations (C11 7.17.4)
 * =================================================================== */

#define atomic_thread_fence(order) __atomic_compiler_barrier()
#define atomic_signal_fence(order) __atomic_compiler_barrier()

/* ===================================================================
 * kill_dependency (C11 7.17.3) — a no-op that breaks dependency chains
 * =================================================================== */

#define kill_dependency(y) (y)

/* ===================================================================
 * Core atomic operations — implemented as plain loads/stores
 * with compiler barriers (single-threaded).
 * =================================================================== */

/* --- Load ---------------------------------------------------------- */

#define atomic_load_explicit(obj, order) \
  __extension__ ({ \
    __atomic_compiler_barrier(); \
    *(obj); \
  })

#define atomic_load(obj) \
  atomic_load_explicit((obj), memory_order_seq_cst)

/* --- Store --------------------------------------------------------- */

#define atomic_store_explicit(obj, desired, order) \
  do { *(obj) = (desired); __atomic_compiler_barrier(); } while (0)

#define atomic_store(obj, desired) \
  atomic_store_explicit((obj), (desired), memory_order_seq_cst)

/* --- Exchange ------------------------------------------------------ */

#define atomic_exchange_explicit(obj, desired, order) \
  __extension__ ({ \
    __typeof__(*(obj)) __ae_tmp = *(obj); \
    *(obj) = (desired); \
    __atomic_compiler_barrier(); \
    __ae_tmp; \
  })

#define atomic_exchange(obj, desired) \
  atomic_exchange_explicit((obj), (desired), memory_order_seq_cst)

/* --- Compare-and-exchange ------------------------------------------ */

#define atomic_compare_exchange_strong_explicit(obj, expected, desired, succ, fail) \
  __extension__ ({ \
    _Bool __cx_ret; \
    if (*(obj) == *(expected)) { \
      *(obj) = (desired); \
      __cx_ret = 1; \
    } else { \
      *(expected) = *(obj); \
      __cx_ret = 0; \
    } \
    __atomic_compiler_barrier(); \
    __cx_ret; \
  })

#define atomic_compare_exchange_strong(obj, expected, desired) \
  atomic_compare_exchange_strong_explicit((obj), (expected), (desired), \
    memory_order_seq_cst, memory_order_seq_cst)

/* Weak variant — same as strong on single-threaded system */
#define atomic_compare_exchange_weak_explicit(obj, expected, desired, succ, fail) \
  atomic_compare_exchange_strong_explicit((obj), (expected), (desired), (succ), (fail))

#define atomic_compare_exchange_weak(obj, expected, desired) \
  atomic_compare_exchange_strong((obj), (expected), (desired))

/* --- Fetch-and-modify ---------------------------------------------- */

#define atomic_fetch_add_explicit(obj, arg, order) \
  __extension__ ({ \
    __typeof__(*(obj)) __fa_tmp = *(obj); \
    *(obj) += (arg); \
    __atomic_compiler_barrier(); \
    __fa_tmp; \
  })

#define atomic_fetch_add(obj, arg) \
  atomic_fetch_add_explicit((obj), (arg), memory_order_seq_cst)

#define atomic_fetch_sub_explicit(obj, arg, order) \
  __extension__ ({ \
    __typeof__(*(obj)) __fs_tmp = *(obj); \
    *(obj) -= (arg); \
    __atomic_compiler_barrier(); \
    __fs_tmp; \
  })

#define atomic_fetch_sub(obj, arg) \
  atomic_fetch_sub_explicit((obj), (arg), memory_order_seq_cst)

#define atomic_fetch_and_explicit(obj, arg, order) \
  __extension__ ({ \
    __typeof__(*(obj)) __fn_tmp = *(obj); \
    *(obj) &= (arg); \
    __atomic_compiler_barrier(); \
    __fn_tmp; \
  })

#define atomic_fetch_and(obj, arg) \
  atomic_fetch_and_explicit((obj), (arg), memory_order_seq_cst)

#define atomic_fetch_or_explicit(obj, arg, order) \
  __extension__ ({ \
    __typeof__(*(obj)) __fo_tmp = *(obj); \
    *(obj) |= (arg); \
    __atomic_compiler_barrier(); \
    __fo_tmp; \
  })

#define atomic_fetch_or(obj, arg) \
  atomic_fetch_or_explicit((obj), (arg), memory_order_seq_cst)

#define atomic_fetch_xor_explicit(obj, arg, order) \
  __extension__ ({ \
    __typeof__(*(obj)) __fx_tmp = *(obj); \
    *(obj) ^= (arg); \
    __atomic_compiler_barrier(); \
    __fx_tmp; \
  })

#define atomic_fetch_xor(obj, arg) \
  atomic_fetch_xor_explicit((obj), (arg), memory_order_seq_cst)

/* ===================================================================
 * atomic_flag operations (C11 7.17.8)
 * =================================================================== */

#define atomic_flag_test_and_set_explicit(obj, order) \
  __extension__ ({ \
    _Bool __fl_old = (obj)->value; \
    (obj)->value = 1; \
    __atomic_compiler_barrier(); \
    __fl_old; \
  })

#define atomic_flag_test_and_set(obj) \
  atomic_flag_test_and_set_explicit((obj), memory_order_seq_cst)

#define atomic_flag_clear_explicit(obj, order) \
  do { (obj)->value = 0; __atomic_compiler_barrier(); } while (0)

#define atomic_flag_clear(obj) \
  atomic_flag_clear_explicit((obj), memory_order_seq_cst)

/* ===================================================================
 * atomic_is_lock_free (C11 7.17.5)
 *
 * Always returns true — everything is lock-free on single-threaded OS.
 * =================================================================== */

#define atomic_is_lock_free(obj) ((_Bool)1)

#ifdef __cplusplus
}
#endif

#endif /* _STDATOMIC_H */
