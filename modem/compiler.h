/* The compiler's extensions the library uses, each behind a plain C99
 * fallback that gives the same result (audit Task 31).
 *
 *   V90M_NOINLINE     keep a function out of line (a hint; empty where the
 *                     compiler has no such attribute)
 *   V90M_CLZ32(x)     the number of leading zero bits of x, x != 0
 *
 * -DV90M_NO_BUILTINS takes the fallback everywhere (the tests build both). */
#ifndef V90M_COMPILER_H
#define V90M_COMPILER_H

#include <stdint.h>

#if defined(__GNUC__)
#define V90M_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define V90M_NOINLINE __declspec(noinline)
#else
#define V90M_NOINLINE
#endif

/* leading zeros of x, 0 to 31; x must not be 0 */
static inline int V90M_CLZ32(uint32_t x)
{
#if defined(__GNUC__) && !defined(V90M_NO_BUILTINS)
    /* one instruction on ARMv5 (CLZ); unsigned int is at least 32 bits here */
    return __builtin_clz((unsigned int)x) - (int)(sizeof(unsigned int) * 8 - 32);
#else
    int n = 0;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n += 8; x <<= 8; }
    if (!(x & 0xF0000000u)) { n += 4; x <<= 4; }
    if (!(x & 0xC0000000u)) { n += 2; x <<= 2; }
    if (!(x & 0x80000000u)) n += 1;
    return n;
#endif
}

#endif
