/* Exact integer divisions cheaper than C's 64-bit one on a 32-bit target
 * (ARMv5 divides in software: some 250 instructions for a 64-bit divide)
 * (audit Task 31).
 *
 *   divlu(u1, u0, v)     (u1 2^32 + u0) / v, for u1 < v (the quotient then
 *                        fits 32 bits): Hacker's Delight's divlu, two 32 by
 *                        16 bit quotient digits, each corrected
 *   div_s64_u32(n, d)    n / d truncated as C truncates, d > 0; divlu when
 *                        |n| / d fits 32 bits, C's division otherwise
 *
 * Both give C's quotient for every input in their domain. */
#ifndef V90M_FIXDIV_H
#define V90M_FIXDIV_H

#include <stdint.h>
#include "compiler.h"

static inline uint32_t divlu(uint32_t u1, uint32_t u0, uint32_t v)
{
    const uint32_t b = 65536;
    uint32_t un1, un0, vn1, vn0, q1, q0, un32, un21, un10, rhat;
    int s = V90M_CLZ32(v);           /* v != 0, as u1 < v */
    v <<= s;                         /* normalised: the top bit set */
    vn1 = v >> 16; vn0 = v & 0xFFFFu;
    un32 = s ? (u1 << s) | (u0 >> (32 - s)) : u1;
    un10 = u0 << s;
    un1 = un10 >> 16; un0 = un10 & 0xFFFFu;
    /* the high digit: at most two corrections; q1 < b before q1 vn0 is
     * formed, and rhat < b before b rhat is, so neither overflows */
    q1 = un32 / vn1; rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > b * rhat + un1) {
        q1--; rhat += vn1;
        if (rhat >= b) break;
    }
    un21 = un32 * b + un1 - q1 * v;  /* modulo 2^32, the true remainder < v */
    q0 = un21 / vn1; rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) {
        q0--; rhat += vn1;
        if (rhat >= b) break;
    }
    return q1 * b + q0;
}

static inline int64_t div_s64_u32(int64_t n, uint32_t d)
{
    uint64_t u = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n, q;
    q = (u >> 32) < d ? divlu((uint32_t)(u >> 32), (uint32_t)u, d) : u / d;
    if (n >= 0) return (int64_t)q;
    /* -q without converting a q of 2^63 (n = INT64_MIN, d = 1) to int64_t */
    return q ? -(int64_t)(q - 1) - 1 : 0;
}

#endif
