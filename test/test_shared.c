/* The primitives kept once (audit Task 22) against the copies they replaced:
 * the V.34 clause 7 scrambler (v34_scramble / v34_descramble, and its
 * wrappers v34scr_*, v90_scramble_* and v32_scramble), the PP, S and S-bar
 * generators of both scalings (v34/v34_train.c and modem/v34train.c), the
 * INFO CRC (info_crc, v90_crc, the x2 records) and INFO1c as INFO1d. The
 * old_* functions are the replaced code as it was. */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../modem/v34train.h"
#include "../modem/info.h"
#include "../modem/x2rec.h"
#include "../modem/sintab.h"
#include "../modem/v34_tabs.h"
#include "../v90/v90_scr.h"
#include "../v90/v90_seq.h"
#include "../v34/v34.h"
#include "../v32/v32.h"

static uint32_t rng_s = 0x2545F491u;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5;
    return rng_s;
}

/* ---- the scramblers as they were ---- */

/* v34/v34_core.c: tap 5 (GPA) or 18 (GPC), the register kept to 23 bits */
typedef struct { uint32_t sr; int tap; } old_scr_t;
static int old_v34_scramble(old_scr_t *s, int bit)
{
    unsigned o = ((unsigned)bit ^ (s->sr >> (s->tap - 1)) ^ (s->sr >> 22)) & 1;
    s->sr = ((s->sr << 1) | o) & 0x7FFFFF;
    return (int)o;
}
static int old_v34_descramble(old_scr_t *s, int bit)
{
    unsigned i = (unsigned)bit & 1;
    unsigned o = (i ^ (s->sr >> (s->tap - 1)) ^ (s->sr >> 22)) & 1;
    s->sr = ((s->sr << 1) | i) & 0x7FFFFF;
    return (int)o;
}
/* modem/v34train.c's v34scr_put / v34scr_get */
static int old_v34scr_put(old_scr_t *s, int bit)
{
    int o = (bit ^ (int)(s->sr >> (s->tap - 1)) ^ (int)(s->sr >> 22)) & 1;
    s->sr = ((s->sr << 1) | (uint32_t)o) & 0x7fffff;
    return o;
}
static int old_v34scr_get(old_scr_t *s, int bit)
{
    int o = (bit ^ (int)(s->sr >> (s->tap - 1)) ^ (int)(s->sr >> 22)) & 1;
    s->sr = ((s->sr << 1) | (uint32_t)(bit & 1)) & 0x7fffff;
    return o;
}
/* v90/v90_scr.c and v32/v32_code.c: the register not masked */
static int old_v90_scr(uint32_t *reg, int bit, int tap)
{
    int out = (bit ^ (int)(*reg >> (tap - 1)) ^ (int)(*reg >> 22)) & 1;
    *reg = (*reg << 1) | (uint32_t)out;
    return out;
}
static int old_v90_descr(uint32_t *reg, int bit, int tap)
{
    int out = (bit ^ (int)(*reg >> (tap - 1)) ^ (int)(*reg >> 22)) & 1;
    *reg = (*reg << 1) | (uint32_t)(bit & 1);
    return out;
}

#ifdef TEST_SHORT
#define SCR_BITS 1000L          /* per state */
#else
#define SCR_BITS 10000L         /* 100 states: 10^6 bits a direction */
#endif
#define SCR_STATES 100

/* every scrambler, both polynomials, both directions: the bits that differ
 * from the old code, and the states whose register differs at the end */
static void scramblers(void)
{
    int poly, dir, st;
    for (poly = 0; poly < 2; poly++) {
        int tap = poly == V34_GPA ? 5 : 18;
        for (dir = 0; dir < 2; dir++) {
            long bad = 0, bad_end = 0;
            for (st = 0; st < SCR_STATES; st++) {
                uint32_t r0 = rnd();
                old_scr_t o34, o34t;
                uint32_t o90 = r0, o32 = r0;
                v34_scr_t n34;
                v34scr_t n34t;
                v90_scr_t n90;
                v32_scr_t n32;
                long k;
                o34.tap = o34t.tap = tap;
                o34.sr = o34t.sr = r0 & 0x7FFFFF;
                v34_scr_init(&n34, poly); n34.sr = r0 & 0x7FFFFF;
                v34scr_init(&n34t, poly == V34_GPA ? V34T_GPA : V34T_GPC); n34t.sr = r0 & 0x7FFFFF;
                n90.reg = r0; n32.reg = r0;
                for (k = 0; k < SCR_BITS; k++) {
                    int b = (int)(rnd() & 1), w, g;
                    if (dir == 0) {
                        w = old_v34_scramble(&o34, b);
                        g = v34_scramble(&n34, b);
                        bad += g != w;
                        bad += v34scr_put(&n34t, b) != old_v34scr_put(&o34t, b);
                        w = old_v90_scr(&o90, b, tap);
                        g = poly == V34_GPA ? v90_scramble_gpa(&n90, b) : v90_scramble_gpc(&n90, b);
                        bad += g != w;
                        bad += v32_scramble(&n32, poly == V34_GPA, b) != old_v90_scr(&o32, b, tap);
                    } else {
                        w = old_v34_descramble(&o34, b);
                        g = v34_descramble(&n34, b);
                        bad += g != w;
                        bad += v34scr_get(&n34t, b) != old_v34scr_get(&o34t, b);
                        w = old_v90_descr(&o90, b, tap);
                        g = poly == V34_GPA ? v90_descramble_gpa(&n90, b) : v90_descramble_gpc(&n90, b);
                        bad += g != w;
                        bad += v32_descramble(&n32, poly == V34_GPA, b) != old_v90_descr(&o32, b, tap);
                    }
                }
                bad_end += n34.sr != o34.sr;
                bad_end += n34t.sr != o34t.sr;
                bad_end += (n90.reg & 0x7FFFFF) != (o90 & 0x7FFFFF);
                bad_end += (n32.reg & 0x7FFFFF) != (o32 & 0x7FFFFF);
            }
            printf("  scramblers, %s, %s: %ld bits differ, %ld end states differ\n",
                   poly == V34_GPA ? "GPA" : "GPC", dir ? "descramble" : "scramble", bad, bad_end);
            CHECK_EQ_I(bad, 0);
            CHECK_EQ_I(bad_end, 0);
        }
    }
}

/* ---- PP, S, S-bar as they were ---- */

/* modem/v34train.c: QRX_REF units */
static const int16_t old_c12[12] = { 5793, 5017, 2896, 0, -2896, -5017, -5793, -5017, -2896, 0, 2896, 5017 };
static void old_pp(int i, int32_t *re, int32_t *im)
{
    int k = (i / 4) % 12, I = i % 4;
    int a = k * I + ((k % 3) == 1 ? 4 : 0);
    a %= 12;
    *re = old_c12[a];
    *im = old_c12[(a + 9) % 12];
}
static void old_rot4(int q, int32_t *re, int32_t *im)
{
    static const int8_t xr[4] = { 1, 1, -1, -1 }, xi[4] = { 1, -1, -1, 1 };
    *re = xr[q & 3] * QRX_REF; *im = xi[q & 3] * QRX_REF;
}
static void old_s(int n, int len, int32_t *re, int32_t *im) { old_rot4(((len - 1 - n) & 1) ? 0 : 3, re, im); }
static void old_sbar(int n, int32_t *re, int32_t *im) { old_rot4((n & 1) ? 1 : 2, re, im); }

/* v34/v34_train.c: Q14 (PP), Q7 (S, S-bar) */
static void old_pp_point(unsigned i, int16_t *re, int16_t *im)
{
    static const int16_t c30[12] = {
        16384, 14189, 8192, 0, -8192, -14189, -16384, -14189, -8192, 0, 8192, 14189 };
    unsigned k = (i / 4) % 72, I = i % 4;
    unsigned ph = (k * I + ((k % 3 == 1) ? 4 : 0)) % 12;
    *re = c30[ph];
    *im = c30[(ph + 9) % 12];
}
static v34_pt_t mkpt(int re, int im) { v34_pt_t p; p.re = (int16_t)(re * V34_ONE); p.im = (int16_t)(im * V34_ONE); return p; }
static v34_pt_t old_s_point(unsigned i, unsigned len) { return ((len - 1 - i) & 1) ? mkpt(1, 1) : mkpt(-1, 1); }
static v34_pt_t old_sbar_point(unsigned i) { return (i & 1) ? mkpt(1, -1) : mkpt(-1, -1); }

static void sequences(void)
{
    int i, len, bad = 0, n = 0;
    for (i = 0; i < 8 * V34_PP_LEN; i++) {
        int32_t wr, wi, gr, gi;
        int16_t qr, qi, pr, pi;
        old_pp(i, &wr, &wi); v34_pp(i, &gr, &gi);
        bad += wr != gr || wi != gi;
        old_pp_point((unsigned)i, &qr, &qi); v34_pp_point((unsigned)i, &pr, &pi);
        bad += qr != pr || qi != pi;
        n += 2;
    }
    printf("  PP: %d of %d points differ\n", bad, n);
    CHECK_EQ_I(bad, 0);
    bad = n = 0;
    for (len = 1; len <= 300; len++)
        for (i = 0; i < len; i++) {
            int32_t wr, wi, gr, gi;
            v34_pt_t w = old_s_point((unsigned)i, (unsigned)len), g = v34_s_point((unsigned)i, (unsigned)len);
            old_s(i, len, &wr, &wi); v34_s(i, len, &gr, &gi);
            bad += wr != gr || wi != gi;
            bad += w.re != g.re || w.im != g.im;
            n += 2;
        }
    for (i = 0; i < 300; i++) {
        int32_t wr, wi, gr, gi;
        v34_pt_t w = old_sbar_point((unsigned)i), g = v34_sbar_point((unsigned)i);
        old_sbar(i, &wr, &wi); v34_sbar(i, &gr, &gi);
        bad += wr != gr || wi != gi;
        bad += w.re != g.re || w.im != g.im;
        n += 2;
    }
    printf("  S, S-bar: %d of %d points differ\n", bad, n);
    CHECK_EQ_I(bad, 0);
}

/* ---- the INFO CRC as it was ---- */

static uint16_t old_info_crc(const uint8_t *bits, int n)
{
    uint16_t c = 0xFFFF;
    int k;
    for (k = 0; k < n; k++) {
        int fb = (bits[k] ^ c) & 1;
        c >>= 1;
        if (fb) c ^= 0x8408;
    }
    return c;
}
static uint16_t old_crc_bit(uint16_t c, int bit)
{
    int fb = (bit ^ c) & 1;
    c >>= 1;
    return fb ? (uint16_t)(c ^ 0x8408) : c;
}
static uint16_t old_x2_crc16(const uint8_t *bits, int n)
{
    uint16_t c = 0xFFFF;
    int k;
    for (k = 0; k < n; k++) c = old_crc_bit(c, bits[k] & 1);
    return c;
}
static int old_x2rec_build(const uint16_t *w, int nw, int ack, uint8_t *bits)
{
    uint16_t c = 0xFFFF;
    int i, k, n = 0;
    for (k = 0; k < 17; k++) bits[n++] = 1;
    bits[n++] = 0;
    for (i = 0; i < nw; i++) {
        uint16_t v = w[i];
        if (i == 0) v = ack ? (uint16_t)(v | 0x8000) : (uint16_t)(v & 0x7FFF);
        for (k = 0; k < 16; k++) {
            int b = (v >> k) & 1;
            bits[n++] = (uint8_t)b;
            c = old_crc_bit(c, b);
        }
        bits[n++] = 0;
    }
    for (k = 0; k < 16; k++) bits[n++] = (uint8_t)((c >> k) & 1);
    return n;
}

static void crcs(void)
{
    static uint8_t bits[512], wb[X2REC_BITS(8)], gb[X2REC_BITS(8)];
    int t, k, bad = 0, badx = 0;
    for (t = 0; t < 2000; t++) {
        int n = (int)(rnd() % 512);
        uint16_t w[8], back[8];
        int nw = 1 + (int)(rnd() % 8), ack = (int)(rnd() & 1), nb, ng;
        /* whole bytes too: the registers read bit 0 of each */
        for (k = 0; k < n; k++) bits[k] = (uint8_t)((t & 1) ? rnd() : rnd() & 1);
        bad += info_crc(bits, n) != old_info_crc(bits, n);
        bad += v90_crc(bits, n) != old_info_crc(bits, n);
        for (k = 0; k < n; k++) bits[k] &= 1;
        bad += info_crc(bits, n) != old_x2_crc16(bits, n);
        for (k = 0; k < nw; k++) w[k] = (uint16_t)rnd();
        nb = old_x2rec_build(w, nw, ack, wb);
        ng = x2rec_build(w, nw, ack, gb);
        badx += nb != ng || memcmp(wb, gb, (size_t)nb) != 0;
        badx += x2rec_parse(gb, ng, back, nw) != nw;
    }
    printf("  INFO CRC: %d of 6000 registers differ; x2 records: %d of 4000 differ\n", bad, badx);
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(badx, 0);
}

/* INFO1c is INFO1d, built and parsed */
static void info1c(void)
{
    int t, k, bad = 0;
    for (t = 0; t < 200; t++) {
        info1d_t d, pd, pc;
        uint8_t bd[INFO_MAX_BITS], bc[INFO_MAX_BITS];
        int nd, nc;
        memset(&pd, 0, sizeof pd); memset(&pc, 0, sizeof pc);
        d.min_pwr_red = (uint8_t)(rnd() & 7); d.add_pwr_red = (uint8_t)(rnd() & 7);
        d.md_len = (uint8_t)(rnd() & 127);
        for (k = 0; k < 6; k++) {
            d.sr[k].hi_carrier = (uint8_t)(rnd() & 1);
            d.sr[k].preemph = (uint8_t)(rnd() & 15);
            d.sr[k].max_rate = (uint8_t)(rnd() & 15);
        }
        d.freq_off = (int16_t)((int)(rnd() % 1024) - 512);
        nd = info1d_build(&d, bd); nc = info1c_build(&d, bc);
        bad += nd != nc || memcmp(bd, bc, (size_t)nd) != 0;
        info1d_parse(bd + INFO_HDR_BITS, &pd); info1c_parse(bd + INFO_HDR_BITS, &pc);
        bad += memcmp(&pd, &pc, sizeof pd) != 0;
    }
    printf("  INFO1c as INFO1d: %d of 200 differ\n", bad);
    CHECK_EQ_I(bad, 0);
}

int main(void)
{
    printf("test_shared: one copy of each table and primitive\n");
    /* one table each, whoever reads it */
    CHECK_EQ_I(sintab[SIN_N / 4], 32767);
    CHECK_EQ_I(sintab[3 * SIN_N / 4], -32767);
    CHECK_EQ_I(v34_interp[0][V34_ID], 16384);
    CHECK_EQ_I(v34_interp[V34_IP][V34_ID + 1], 16384);
    scramblers();
    sequences();
    crcs();
    info1c();
    T_REPORT("test_shared");
}
