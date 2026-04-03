#ifndef _ASSERT_H
#define _ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 : \
     (dprintf(2, "Assertion failed: %s, file %s, line %d\n", \
              #expr, __FILE__, __LINE__), abort()))
#endif

#ifndef static_assert
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define static_assert _Static_assert
#else
#define static_assert(cond, msg) typedef char static_assertion_##__LINE__[(cond) ? 1 : -1]
#endif
#endif

#endif /* _ASSERT_H */
