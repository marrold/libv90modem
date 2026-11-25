/* Integer square roots, one copy (audit Task 23b).
 *
 * isqrt_u64 is floor(sqrt(v)) by the bit-pair method, exact for every v.
 * isqrt_s64 is the same for a signed v, 0 for v <= 0, and its result clamped
 * to INT32_MAX (the callers pass powers whose root fits 31 bits).  The
 * method needs no multiply, no divide and no table, and the result is the
 * same on every target. */
#ifndef V90M_FIXMATH_H
#define V90M_FIXMATH_H

#include <stdint.h>

static inline uint32_t isqrt_u64(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}

static inline int32_t isqrt_s64(int64_t v)
{
    uint32_t r;
    if (v <= 0) return 0;
    r = isqrt_u64((uint64_t)v);
    return r > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)r;
}

#endif
