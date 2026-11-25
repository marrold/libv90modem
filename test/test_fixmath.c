/* modem/fixmath.h against the integer square roots it replaced (audit Task
 * 23b) and modem/seqhunt.h against the sync hunters it replaced (23c).  Each old_* function is the code as it was in its file, kept here
 * verbatim; the shared one must agree with it on every input the old one
 * accepted (below 2^20, 10^6 random 64-bit values, and the edges). */
#include <stdio.h>
#include <stdint.h>
#include "test.h"
#include "../modem/fixmath.h"
#include "../modem/seqhunt.h"

/* anstone.c, v22rx.c, v32_code.c (v32_isqrt64), v92_design.c, v92_p34d.c */
static uint32_t old_u64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* probe.c (returned int64_t) */
static int64_t old_probe(int64_t v)
{
    int64_t r = 0, b = (int64_t)1 << 62;
    if (v <= 0) return 0;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* v90arx.c, qamrx.c (returned int32_t) */
static int32_t old_s32(int64_t v)
{
    int64_t r = 0, b = (int64_t)1 << 62;
    if (v <= 0) return 0;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int32_t)r;
}

/* v90dil.c */
static int32_t old_u32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int32_t)r;
}

/* dpsk.c's old copy (a start at 2^40): it differs from the root at 2^42, which
 * its pw / 2 (<= 2^32, see dpskrx_level) never reaches */
static int old_dpsk(int64_t v)
{
    int64_t r = 0, b = (int64_t)1 << 40;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int)r;
}

static uint64_t s = 0x9E3779B97F4A7C15u;
static uint64_t rnd64(void)
{
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;      /* xorshift64 */
    return s;
}

static long compare(uint64_t v)
{
    long bad = 0;
    uint32_t r = isqrt_u64(v);
    bad += r != old_u64(v);
    bad += !((uint64_t)r * r <= v && (r == 0xffffffffu || (uint64_t)(r + 1) * (r + 1) > v));
    if (v <= (uint64_t)INT64_MAX) {
        int64_t sv = (int64_t)v;
        int32_t rs = isqrt_s64(sv);
        bad += rs != (int32_t)(old_probe(sv) > INT32_MAX ? INT32_MAX : old_probe(sv));
        if (old_probe(sv) <= INT32_MAX) bad += rs != old_s32(sv) || rs != (int32_t)old_probe(sv);
    }
    if (v <= 0xffffffffu) bad += (int32_t)isqrt_u64(v) != old_u32((uint32_t)v);
    return bad;
}

/* The sync hunter as it was in each file, kept here.  All were the same few
 * lines: ones counted, and on a zero a run of 17 or more starts a frame.
 *   v90a.c (jones, mones x2), v92aseq.c, v92_p34d.c: hunting on a zero:
 *     if (bit) { ones++; return; }  if (ones >= 17) { start } ones = 0;
 *   p34d.c, x2d.c, k56d.c, v34dx.c: ones counted first and tested for E,
 *     on a zero:  if (ones >= 17) { start } ... ones = 0;
 * (the later copies run `ones++` / `++ones` before the same test, so one
 * model per form covers them). */
typedef struct { int ones; } old_t;
static int old_a(old_t *o, int bit)      /* v90a, v92aseq, v92_p34d */
{
    if (bit) { o->ones++; return 0; }
    if (o->ones >= 17) { o->ones = 0; return 1; }
    o->ones = 0;
    return 0;
}
static int old_b(old_t *o, int bit)      /* p34d, x2d, k56d, v34dx */
{
    int start = 0;
    if (bit) { ++o->ones; return 0; }
    if (o->ones >= 17) start = 1;
    o->ones = 0;
    return start;
}

static long hunt_seq(uint64_t bits, int len)
{
    seqhunt_t h; old_t a, b;
    long bad = 0;
    int i;
    h.ones = a.ones = b.ones = 0;
    for (i = 0; i < len; i++) {
        int bit = (int)((bits >> i) & 1);
        int r = seqhunt_bit(&h, bit), ra = old_a(&a, bit), rb = old_b(&b, bit);
        bad += r != ra || r != rb || h.ones != a.ones || h.ones != b.ones;
    }
    return bad;
}

int main(void)
{
    long bad = 0;
    uint64_t v;
    int i;
    printf("test_fixmath: one integer square root\n");
    for (v = 0; v < ((uint64_t)1 << 20); v++) bad += compare(v);
    printf("  every input below 2^20: %ld differ\n", bad);
    CHECK_EQ_I(bad, 0);
    bad = 0;
    for (i = 0; i < 1000000; i++) {
        v = rnd64();
        bad += compare(v);
        bad += compare(v >> (rnd64() & 63));          /* every magnitude */
    }
    printf("  10^6 random 64-bit inputs (and shifted ones): %ld differ\n", bad);
    CHECK_EQ_I(bad, 0);
    /* the edges, and perfect squares with their neighbours */
    bad = 0;
    bad += compare(UINT64_MAX) + compare((uint64_t)INT64_MAX) + compare(0) + compare(1);
    for (i = 0; i < 64; i++) bad += compare((uint64_t)1 << i) + compare(((uint64_t)1 << i) - 1);
    for (v = 1; v < 0xffffffffu; v = v * 3 + 1) {
        bad += compare(v * v) + compare(v * v - 1) + compare(v * v + 1);
    }
    printf("  edges, 2^k, squares and their neighbours: %ld differ\n", bad);
    CHECK_EQ_I(bad, 0);
    /* negative: the signed forms give 0, as the old signed copies did */
    CHECK_EQ_I(isqrt_s64(-1), 0);
    CHECK_EQ_I(isqrt_s64(INT64_MIN), 0);
    CHECK_EQ_I(old_probe(-1), 0);
    CHECK_EQ_I(old_s32(INT64_MIN), 0);
    CHECK_EQ_I(isqrt_s64(INT64_MAX), INT32_MAX);        /* clamped, the root is 3037000499 */
    CHECK_EQ_I(isqrt_u64(UINT64_MAX), 0xffffffffu);
    /* dpsk.c's old copy agrees below 2^42, above its pw / 2 bound of 2^32 */
    bad = 0;
    for (i = 0; i < 1000000; i++) {
        v = rnd64() >> (22 + (rnd64() & 31));
        if (v >= ((uint64_t)1 << 42)) v >>= 22;
        bad += old_dpsk((int64_t)v) != (int)isqrt_u64(v);
    }
    printf("  dpsk.c's copy below 2^42: %ld differ\n", bad);
    CHECK_EQ_I(bad, 0);
    CHECK(old_dpsk((int64_t)1 << 62) != (int)isqrt_u64((uint64_t)1 << 62));
    /* the sync hunter: every sequence of up to 24 bits, then 10^6 random
     * long ones (runs of ones are what matter, so half are biased to ones) */
    bad = 0;
    {
        int len;
        for (len = 1; len <= 24; len++)
            for (v = 0; v < ((uint64_t)1 << len); v++) bad += hunt_seq(v, len);
        printf("  every bit sequence up to 24 bits: %ld differ\n", bad);
        CHECK_EQ_I(bad, 0);
        bad = 0;
        for (i = 0; i < 1000000; i++) {
            uint64_t x = rnd64();
            if (i & 1) x |= rnd64() | rnd64();
            bad += hunt_seq(x, 64);
        }
        printf("  10^6 random 64-bit sequences: %ld differ\n", bad);
        CHECK_EQ_I(bad, 0);
    }
    T_REPORT("test_fixmath");
}
