/* Audit Task 35's small exact forms against the code they replace (or, for
 * the scramblers, against the bit-serial scrambler). Each ref_* function is
 * the code of 347caac (v0.5.0's, as Phases 1 and 2 left it), kept here
 * verbatim but for its name; the library must agree with it exactly:
 *   - the octet's linear value (v90_octet_linear), all 256 octets of both
 *     laws, against Table 1's closed form (v90/v90_pcm.c);
 *   - the modulus encoder (v90_modulus_encode), for every Mi in 1..128: the
 *     dividend of a step (cur < Mi 2^16) at 0, 1 and Mi 2^16 - 1 and at
 *     random values, and random maps of mixed Mi; `full`: every cur;
 *   - the scrambler n bits at once (v34_scramble_n, v34_descramble_n, and
 *     v90's *_bits over arrays) against n calls of the one-bit forms, GPC
 *     and GPA, from random states;
 *   - the x2 mapper (x2map_frame) against its 64-bit form, random frames in
 *     every bank, and a run whose bank changes every few frames;
 *   - the echo canceller (ec_tick), every return value and its whole state;
 *   - the V.92 precoder and prefilter (v92_enc_symbol) on a filter whose
 *     sections are padded by zero coefficients, up to more than 255 of
 *     them, against the same filter less the padding (v92_filt_trim);
 *   - the same encoder when its filter changes under it without
 *     v92_enc_init, against 347caac's v92_enc_symbol.
 * The program prints one line per part; any difference fails. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "test.h"
#include "v90_pcm.h"
#include "v90_enc.h"
#include "v90_scr.h"
#include "v34.h"
#include "../modem/x2map.h"
#include "../modem/x2tab.h"
#include "../modem/ec.h"
#include "v92_enc.h"
#include "v34_priv.h"

#ifdef TEST_SHORT
#define N_RCP     5000      /* random cur per Mi */
#define N_RMAP    20000     /* random maps of mixed Mi */
#define N_SCR     100000    /* random bytes per polynomial and direction */
#define N_X2      20000     /* random frames per bank */
#define N_X2MIX   20000
#define N_EC      100000
#define N_FILT    40
#else
#define N_RCP     100000
#define N_RMAP    1000000
#define N_SCR     1000000
#define N_X2      1000000
#define N_X2MIX   1000000
#define N_EC      1000000
#define N_FILT    200
#endif
#define N_STATES  100       /* random scrambler states */

static uint64_t rs = 0x2545f4914f6cdd1du;
static uint32_t rnd32(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 32);
}
/* 0 .. n - 1 */
static uint32_t rnd_below(uint32_t n) { return rnd32() % n; }

/* ---------------- reference copy of v90/v90_pcm.c (347caac) ---------------- */
static int32_t ref_ucode_linear(int alaw, unsigned ucode)
{
    unsigned c, s;
    ucode &= 0x7F;
    c = ucode >> 4;
    s = ucode & 15;
    if (alaw)
        return c == 0 ? (int32_t)((2 * s + 1) << 3)
                      : (int32_t)((2 * s + 33) << (c + 2));
    return (int32_t)((2 * s + 33) << (c + 2)) - 132;
}
static int32_t ref_pcm_linear(int alaw, unsigned ucode, int positive)
{
    int32_t v = ref_ucode_linear(alaw, ucode);
    return positive ? v : -v;
}
static unsigned ref_octet_ucode(int alaw, uint8_t octet)
{
    if (alaw) return (unsigned)((octet ^ 0x55u) & 0x7Fu);
    return (unsigned)(0x7Fu - (octet & 0x7Fu));
}
static int ref_octet_sign(uint8_t octet)
{
    return (octet >> 7) & 1;
}
static int32_t ref_octet_linear(int alaw, uint8_t octet)
{
    return ref_pcm_linear(alaw, ref_octet_ucode(alaw, octet), ref_octet_sign(octet));
}

static void test_octets(void)
{
    static const int laws[4] = { 0, 1, 2, -1 };     /* any nonzero is A-law */
    long n = 0, differ = 0;
    int l, o;
    for (l = 0; l < 4; l++)
        for (o = 0; o < 256; o++, n++)
            if (v90_octet_linear(laws[l], (uint8_t)o) != ref_octet_linear(laws[l], (uint8_t)o)) {
                if (differ++ < 3) printf("  octet %02x, law %d: %ld, want %ld\n", o, laws[l],
                                         (long)v90_octet_linear(laws[l], (uint8_t)o), (long)ref_octet_linear(laws[l], (uint8_t)o));
            }
    CHECK_EQ_I(differ, 0);
    printf("  octet -> linear: %ld octets (256 x mu-law, A-law as 1, 2 and -1): %ld differ\n", n, differ);
}

/* ---------------- reference copy of v90/v90_enc.c's v90_modulus_encode (347caac) ---------------- */
static void ref_modulus_encode(const v90_map_t *m, const uint8_t *b, uint8_t k[6])
{
    uint16_t r[3] = { 0, 0, 0 };
    int i, j;
    for (i = 0; i < m->K; i++)
        if (b[i] & 1) r[i >> 4] |= (uint16_t)(1u << (i & 15));
    for (i = 0; i < 6; i++) {
        uint32_t M = m->M[i], rem = 0;
        for (j = 2; j >= 0; j--) {
            uint32_t cur = (rem << 16) | r[j];
            r[j] = (uint16_t)(cur / M);
            rem = cur % M;
        }
        k[i] = (uint8_t)rem;
    }
}

/* K = 42 bits from the three limbs */
static void limbs_to_bits(uint32_t r2, uint32_t r1, uint32_t r0, uint8_t b[42])
{
    int i;
    for (i = 0; i < 16; i++) b[i] = (uint8_t)((r0 >> i) & 1);
    for (i = 0; i < 16; i++) b[16 + i] = (uint8_t)((r1 >> i) & 1);
    for (i = 0; i < 10; i++) b[32 + i] = (uint8_t)((r2 >> i) & 1);
}

/* one call whose first step's dividend at limb 1 is cur: limb 2 is cur >> 16
 * (below Mi, the remainder carried down), limb 1 its low 16 bits */
static int mod_differs(const v90_map_t *m, uint32_t cur, uint32_t r0)
{
    uint8_t b[42], k1[6], k2[6];
    limbs_to_bits(cur >> 16, cur & 0xFFFF, r0, b);
    v90_modulus_encode(m, b, k1);
    ref_modulus_encode(m, b, k2);
    return memcmp(k1, k2, 6) != 0;
}

static void test_modulus(int full)
{
    v90_map_t m;
    long n = 0, differ = 0, nmap = 0;
    uint32_t M;
    int i;
    memset(&m, 0, sizeof m);
    m.K = 42;
    for (M = 1; M <= 128; M++) {
        uint32_t top = M << 16, cur;        /* cur < Mi 2^16 */
        long c;
        for (i = 0; i < 6; i++) m.M[i] = (uint8_t)M;
        if (full) {
            for (cur = 0; cur < top; cur++, n++) differ += mod_differs(&m, cur, rnd32() & 0xFFFF);
        } else {
            static const int edge_r0[3] = { 0, 1, 0xFFFF };
            int e;
            for (e = 0; e < 3; e++, n += 3) {
                differ += mod_differs(&m, 0, (uint32_t)edge_r0[e]);
                differ += mod_differs(&m, 1, (uint32_t)edge_r0[e]);
                differ += mod_differs(&m, top - 1, (uint32_t)edge_r0[e]);
            }
            for (c = 0; c < N_RCP; c++, n++) differ += mod_differs(&m, rnd_below(top), rnd32() & 0xFFFF);
        }
    }
    /* maps of mixed Mi, random R0 below 2^42 */
    for (nmap = 0; nmap < N_RMAP; nmap++) {
        uint8_t b[42], k1[6], k2[6];
        for (i = 0; i < 6; i++) m.M[i] = (uint8_t)(1 + rnd_below(128));
        m.K = (uint8_t)(1 + rnd_below(42));
        for (i = 0; i < 42; i++) b[i] = (uint8_t)(rnd32() & 1);
        v90_modulus_encode(&m, b, k1);
        ref_modulus_encode(&m, b, k2);
        differ += memcmp(k1, k2, 6) != 0;
    }
    CHECK_EQ_I(differ, 0);
    printf("  modulus encoder: Mi 1..128, %ld dividends (%s), %ld random maps: %ld differ\n",
           n, full ? "every one below Mi 2^16" : "0, 1, Mi 2^16 - 1 and random", nmap, differ);
}

/* ---------------- the scramblers, n bits at once ---------------- */
static void test_scramblers(void)
{
    int poly, desc;
    for (poly = V34_GPC; poly <= V34_GPA; poly++)
        for (desc = 0; desc <= 1; desc++) {
            long bytes = 0, differ = 0, st;
            /* v34_*_n, n of 1..8 at a time */
            for (st = 0; st < N_STATES; st++) {
                v34_scr_t a, b;
                long j;
                v34_scr_init(&a, poly);
                a.sr = rnd32() & 0x7FFFFF;
                b = a;
                for (j = 0; j < N_SCR / N_STATES; j++, bytes++) {
                    int nb = j % 4 == 3 ? (int)(1 + rnd_below(8)) : 8, k;
                    unsigned in = rnd32() & ((1u << nb) - 1), got, want = 0;
                    got = desc ? v34_descramble_n(&a, in, nb) : v34_scramble_n(&a, in, nb);
                    for (k = nb - 1; k >= 0; k--) {
                        int bit = (int)((in >> k) & 1);
                        want = (want << 1) | (unsigned)(desc ? v34_descramble(&b, bit) : v34_scramble(&b, bit));
                    }
                    if (got != want || a.sr != b.sr) {
                        if (differ++ < 3) printf("  %s %s, state %ld, byte %ld: %02x, want %02x\n",
                                                 poly == V34_GPC ? "GPC" : "GPA", desc ? "descrambler" : "scrambler",
                                                 st, j, got, want);
                    }
                }
            }
            /* v90's arrays, random lengths 1..64 */
            for (st = 0; st < N_STATES; st++) {
                v90_scr_t a, b;
                long j;
                a.reg = rnd32() & 0x7FFFFF;
                b = a;
                for (j = 0; j < N_SCR / N_STATES / 32; j++) {
                    uint8_t x[64], y[64];
                    int nb = (int)(1 + rnd_below(64)), k;
                    for (k = 0; k < nb; k++) x[k] = (uint8_t)(rnd32() & 1);
                    for (k = 0; k < nb; k++) {
                        int bit = x[k];
                        if (poly == V34_GPC) y[k] = (uint8_t)(desc ? v90_descramble_gpc(&b, bit) : v90_scramble_gpc(&b, bit));
                        else                 y[k] = (uint8_t)(desc ? v90_descramble_gpa(&b, bit) : v90_scramble_gpa(&b, bit));
                    }
                    if (poly == V34_GPC) { if (desc) v90_descramble_gpc_bits(&a, x, nb); else v90_scramble_gpc_bits(&a, x, nb); }
                    else                 { if (desc) v90_descramble_gpa_bits(&a, x, nb); else v90_scramble_gpa_bits(&a, x, nb); }
                    if (memcmp(x, y, (size_t)nb) != 0 || a.reg != b.reg) differ++;
                }
            }
            CHECK_EQ_I(differ, 0);
            printf("  %s %-11s: %d states, %ld bytes (v34, 1..8 bits a call) and arrays of 1..64 bits (v90): %ld differ\n",
                   poly == V34_GPC ? "GPC" : "GPA", desc ? "descrambler" : "scrambler", N_STATES, bytes, differ);
        }
}

/* ---------------- reference copy of modem/x2map.c's x2map_frame (347caac) ---------------- */
#define REF_MD 5
#define REF_FRAME_DC_LIN (805 * 4)

static int ref_free_pos(const uint8_t lev[X2MAP_N])
{
    int i, f = 0;
    for (i = 1; i < X2MAP_N; i++) if (lev[i] <= lev[f]) f = i;
    return f;
}

static void ref_x2map_frame(x2map_t *m, const uint8_t *bits, uint8_t out[X2MAP_N])
{
    const uint8_t *rv[X2MAP_N], *sn[X2MAP_N];
    uint8_t lev[X2MAP_N], o[X2MAP_N];
    uint64_t x = 0;
    int32_t sum = 0;
    int nb = x2map_bits(m), i, k, f, neg;
    if (!nb) {
        for (i = 0; i < X2MAP_N; i++) out[i] = 0xff;
        return;
    }
    /* B up to 37 bits: the number in 64 bits */
    for (k = 0; k < nb - REF_MD; k++) x |= (uint64_t)(bits[REF_MD + k] & 1) << k;
    for (i = 0; i < X2MAP_N; i++) {
        uint32_t n = (uint32_t)x2map_list(m, i, &rv[i], &sn[i]), d = (uint32_t)(x % n);
        x /= n;
        lev[i] = rv[i][d]; o[i] = sn[i][d];
    }
    f = ref_free_pos(lev);
    for (i = 0, k = 0; i < X2MAP_N; i++) {
        if (i == f) continue;
        m->last ^= bits[k++] & 1;
        out[i] = m->last ? o[i] : (uint8_t)(o[i] & 0x7f);
        sum += ref_octet_linear(0, out[i]);
    }
    if (m->levels_a)
        neg = sum > REF_FRAME_DC_LIN;
    else
        neg = m->rds + sum > 0;
    out[f] = neg ? (uint8_t)(o[f] & 0x7f) : o[f];
    m->rds += sum + ref_octet_linear(0, out[f]);
}

/* one random frame through both; 1 when they differ */
static int x2_differs(x2map_t *a, x2map_t *b)
{
    uint8_t bits[64], o1[X2MAP_N], o2[X2MAP_N];
    int nb = x2map_bits(b), k;
    for (k = 0; k < nb; k++) bits[k] = (uint8_t)(rnd32() & 1);
    x2map_frame(a, bits, o1);
    ref_x2map_frame(b, bits, o2);
    return memcmp(o1, o2, X2MAP_N) != 0 || a->last != b->last || a->rds != b->rds
        || x2map_bits(a) != nb;
}

static void test_x2map(void)
{
    long frames = 0, differ = 0, j;
    int pad, idx, banks = 0;
    x2map_t a, b;
    /* levels A, then the 45 banks of the data levels */
    for (pad = -1; pad < X2_MODES; pad++)
        for (idx = 1; idx <= X2_NIDX; idx++) {
            if (pad < 0 && idx > 1) break;
            x2map_init(&a, idx, pad < 0 ? 0 : pad, pad < 0);
            x2map_init(&b, idx, pad < 0 ? 0 : pad, pad < 0);
            CHECK(x2map_bits(&a) > 0);
            for (j = 0; j < N_X2; j++, frames++) differ += x2_differs(&a, &b);
            banks++;
        }
    printf("  x2map_frame: %d banks, %ld random frames: %ld differ\n", banks, frames, differ);
    CHECK_EQ_I(differ, 0);
    /* the owner's writes between frames (x2tx: levels A, the data levels,
     * a rate change), banks that do not exist among them */
    differ = 0;
    x2map_init(&a, 1, 0, 1);
    x2map_init(&b, 1, 0, 1);
    for (j = 0; j < N_X2MIX; j++) {
        if (rnd_below(4) == 0) {
            int la = rnd_below(4) == 0, ix = (int)rnd_below(X2_NIDX + 2), pm = (int)rnd_below(X2_MODES + 1);
            a.levels_a = b.levels_a = la; a.index = b.index = ix; a.pad_mode = b.pad_mode = pm;
        }
        differ += x2_differs(&a, &b);
    }
    CHECK_EQ_I(differ, 0);
    printf("  x2map_frame, the bank changed between frames: %ld frames: %ld differ\n", (long)N_X2MIX, differ);
}

/* ---------------- reference copy of modem/ec.c's ec_tick (347caac) ---------------- */
static int16_t ref_ec_tick(ec_t *e, int16_t tx, int16_t rx, int mu_shift)
{
    const int16_t *x;
    int64_t acc = 0;
    int32_t err;
    int i, old;

    if (--e->pos < 0) e->pos = EC_NTAP - 1;
    old = e->x[e->pos];
    e->energy += (int64_t)tx * tx - (int64_t)old * old;
    e->x[e->pos] = e->x[e->pos + EC_NTAP] = tx;
    x = &e->x[e->pos];

    for (i = 0; i < EC_NTAP; i += 4)
    {
        acc += (int64_t)e->h[i] * x[i]; acc += (int64_t)e->h[i + 1] * x[i + 1];
        acc += (int64_t)e->h[i + 2] * x[i + 2]; acc += (int64_t)e->h[i + 3] * x[i + 3];
    }
    err = rx - (int32_t)((acc + (1 << 23)) >> 24);

    if (mu_shift >= 0 && e->energy > 65536) {
        int64_t g = (int64_t)err * ((int64_t)1 << 40) / e->energy;
        for (i = 0; i < EC_NTAP; i++) e->h[i] += (int32_t)((g * x[i] + ((int64_t)1 << (15 + mu_shift))) >> (16 + mu_shift));
    }
    if (err > 32767) err = 32767;
    if (err < -32768) err = -32768;
    return (int16_t)err;
}

static void test_ec(void)
{
    ec_t a, b;
    long j, differ = 0;
    int i, amp = 4000;
    for (j = 0; j < N_EC; j++) {
        int16_t tx, rx, r1, r2;
        int mu;
        if (j % 10000 == 0) {
            /* a fresh canceller with a random echo path, Q24, |h| < 0.25 */
            ec_init(&a);
            for (i = 0; i < EC_NTAP; i++) a.h[i] = (int32_t)(rnd32() % (1u << 23)) - (1 << 22);
            b = a;
            amp = (int)(1 + rnd_below(8000));
        }
        if (j % 1000 == 500) amp = rnd_below(4) == 0 ? 20 : (int)(1 + rnd_below(8000));   /* quiet stretches too */
        tx = (int16_t)((int32_t)rnd_below(2u * (uint32_t)amp + 1) - amp);
        rx = (int16_t)((int32_t)rnd_below(2u * 8000 + 1) - 8000);
        mu = (int)rnd_below(8) - 1;                     /* -1 (frozen), 0..6 */
        if (mu >= 0 && mu < 2) mu += 2;
        r1 = ec_tick(&a, tx, rx, mu);
        r2 = ref_ec_tick(&b, tx, rx, mu);
        if (r1 != r2 || memcmp(a.h, b.h, sizeof a.h) != 0 || memcmp(a.x, b.x, sizeof a.x) != 0
            || a.pos != b.pos || a.energy != b.energy) {
            if (differ++ < 3) printf("  ec_tick %ld: %d, want %d\n", j, r1, r2);
            b = a;
        }
    }
    CHECK_EQ_I(differ, 0);
    printf("  ec_tick: %ld samples, return value and state: %ld differ\n", (long)N_EC, differ);
}

/* ---------------- V.92: zero coefficients at the end of each section ---------------- */
static uint32_t bit_seed[2];
static int get_a(void *u) { (void)u; return (int)((bit_seed[0] = bit_seed[0] * 1664525u + 1013904223u) >> 31); }
static int get_b(void *u) { (void)u; return (int)((bit_seed[1] = bit_seed[1] * 1664525u + 1013904223u) >> 31); }

static void test_v92_trim(void)
{
    static const uint8_t M[12] = { 8, 8, 8, 4, 8, 8, 8, 4, 8, 8, 8, 4 };
    static v92_map_t map;
    static v92_filt_t fc, fp, ft, fct;
    static v92_enc_t ea, eb;
    long syms = 0, differ = 0;
    int c, e, s, over255 = 0;
    memset(&map, 0, sizeof map);
    map.K = 33;
    memcpy(map.M, M, 12);
    map.lc[0] = 8;
    for (e = 0; e < 8; e++) map.pt[0][e] = (uint16_t)((2 * e + 1) * 50);
    for (c = 0; c < N_FILT; c++) {
        unsigned len[4], pad[4], at = 0, ap = 0, total;
        for (s = 0; s < 4; s++) len[s] = rnd_below(s == 2 ? 20 : 21) + (s == 2);   /* lz2 >= 1 */
        total = len[0] + len[1] + len[2] + len[3];
        for (s = 0; s < 4; s++) pad[s] = rnd_below(c % 2 ? 4 : 40);
        if (c % 5 == 0) {
            /* one section with more than 255 zeros at its end */
            s = (int)rnd_below(4);
            pad[s] = 256 + rnd_below(V92_MAX_LSEC - 256 - len[s] + 1);
        }
        for (s = 0; s < 4; s++) {
            unsigned room = V92_MAX_LTOT - total;
            if (len[s] + pad[s] > V92_MAX_LSEC) pad[s] = V92_MAX_LSEC - len[s];
            if (pad[s] > room) pad[s] = room;
            total += pad[s];
            if (pad[s] > 255) over255++;
        }
        memset(&fc, 0, sizeof fc);
        memset(&fp, 0, sizeof fp);
        fc.lz1 = (uint16_t)len[0]; fc.lp1 = (uint16_t)len[1]; fc.lz2 = (uint16_t)len[2]; fc.lp2 = (uint16_t)len[3];
        fp.lz1 = (uint16_t)(len[0] + pad[0]); fp.lp1 = (uint16_t)(len[1] + pad[1]);
        fp.lz2 = (uint16_t)(len[2] + pad[2]); fp.lp2 = (uint16_t)(len[3] + pad[3]);
        fc.gain4 = fp.gain4 = (uint16_t)(0x4000 + rnd_below(0x8000));
        for (s = 0; s < 4; s++) {
            unsigned k;
            for (k = 0; k < len[s]; k++) {
                /* small taps; some zero inside the section */
                int16_t v = rnd_below(6) == 0 ? 0 : (int16_t)((int32_t)rnd_below(4001) - 2000);
                fc.coef[at++] = v;
                fp.coef[ap++] = v;
            }
            ap += pad[s];               /* fp's zeros */
        }
        /* the padded filter trimmed is the unpadded one trimmed, and
           encodes as the padded one does */
        v92_filt_trim(&ft, &fp);
        v92_filt_trim(&fct, &fc);
        if (memcmp(&ft, &fct, sizeof ft) != 0) differ++;
        CHECK_EQ_I(v92_enc_init(&ea, &map, &ft, get_a, NULL), 0);
        CHECK_EQ_I(v92_enc_init(&eb, &map, &fp, get_b, NULL), 0);
        bit_seed[0] = bit_seed[1] = rnd32();
        for (e = 0; e < 1200; e++, syms++) {
            v92_sym_t sa, sb;
            int32_t oa = v92_enc_symbol(&ea, &sa), ob = v92_enc_symbol(&eb, &sb);
            if (oa != ob || sa.i != sb.i || sa.ki != sb.ki || sa.y0 != sb.y0 || sa.eta != sb.eta
                || sa.u != sb.u || sa.x != sb.x || sa.v != sb.v) {
                if (differ++ < 3) printf("  v92 filter %d, symbol %d: %ld, want %ld\n", c, e, (long)ob, (long)oa);
                break;
            }
        }
    }
    CHECK_EQ_I(differ, 0);
    printf("  v92_enc_symbol, sections padded with zeros: %d filters (%d sections of more than 255 zeros), %ld symbols: %ld differ\n",
           N_FILT, over255, syms, differ);
}

/* ---------------- reference copy of v92/v92_enc.c's v92_enc_symbol (347caac) ---------------- */
static int32_t ref_rnd_shift(int64_t v, int sh)
{
    int64_t h = (int64_t)1 << (sh - 1);
    int64_t r = v >= 0 ? (v + h) >> sh : -((-v + h) >> sh);
    if (r > INT32_MAX) return INT32_MAX;
    if (r < -INT32_MAX) return -INT32_MAX;
    return (int32_t)r;
}
static int64_t ref_hdot16(const int16_t *hist, int hp, const int16_t *c, unsigned n)
{
    int64_t acc = 0;
    unsigned k = 0, n1 = (unsigned)hp < n ? (unsigned)hp : n;
    const int16_t *h = hist + hp - 1;
    for (; k < n1; k++, h--) acc += (int32_t)*h * c[k];
    for (h = hist + V92_MAX_LSEC - 1; k < n; k++, h--) acc += (int32_t)*h * c[k];
    return acc;
}
static int64_t ref_hdot32(const int32_t *hist, int hp, const int16_t *c, unsigned n)
{
    int64_t acc = 0;
    unsigned k = 0, n1 = (unsigned)hp < n ? (unsigned)hp : n;
    const int32_t *h = hist + hp - 1;
    for (; k + 4 <= n1; k += 4, h -= 4) {
        acc += (int64_t)h[0] * c[k]; acc += (int64_t)h[-1] * c[k + 1];
        acc += (int64_t)h[-2] * c[k + 2]; acc += (int64_t)h[-3] * c[k + 3];
    }
    for (; k < n1; k++, h--) acc += (int64_t)*h * c[k];
    for (h = hist + V92_MAX_LSEC - 1; k + 4 <= n; k += 4, h -= 4) {
        acc += (int64_t)h[0] * c[k]; acc += (int64_t)h[-1] * c[k + 1];
        acc += (int64_t)h[-2] * c[k + 2]; acc += (int64_t)h[-3] * c[k + 3];
    }
    for (; k < n; k++, h--) acc += (int64_t)*h * c[k];
    return acc;
}

static int32_t ref_enc_symbol(v92_enc_t *e, v92_sym_t *s)
{
    const v92_map_t *m = e->map;
    const v92_filt_t *f = e->filt;
    int i = e->i, k = i % V92_TFRAME;
    int lc = v92_lc(m, i);
    int r, period, first, eta, n;
    int32_t fb = 0, u, x, v, out;

    if (i == 0) {
        if (e->forced) {
            memcpy(e->ki, e->kf, V92_DFRAME);
            e->forced = 0;
        } else {
            uint8_t bits[V92_MAX_K];
            for (n = 0; n < m->K; n++)
                bits[n] = (uint8_t)v90_scramble_gpa(&e->scr, e->get(e->user) & 1);
            v92_mod_encode(&e->mod, bits, e->ki);
        }
    }
    if (k == 0) e->y0 = (uint8_t)v34_conv_y0(m->trellis, e->conv);

    if (k < 3) {
        r = e->ki[i];
        period = m->M[i];
    } else {
        int par = (e->eta[0] + e->eta[1] + e->eta[2] + e->y0) % 2 != 0;
        r = 2 * e->ki[i] + par;
        period = 2 * m->M[i];
    }
    first = -lc + (r + lc) % period;

    if (f) {
        int64_t acc = 0;
        const int16_t *z1 = f->coef, *p1 = z1 + f->lz1;
        acc += ref_hdot16(e->uh, e->hp, z1, f->lz1) * (1 << (V92_XQ - 1));
        acc += ref_hdot32(e->xh, e->hp, p1, f->lp1);
        fb = ref_rnd_shift(acc, 14);
    }

    if (e->pick) {
        int16_t cand[2 * V92_MAX_LC];
        for (n = 0, eta = first; eta < lc; eta += period) cand[n++] = (int16_t)eta;
        n = e->pick(e->pick_user, i, cand, n);
        eta = cand[n];
    } else {
        int64_t best = -1;
        int e1, best_eta = first;
        for (e1 = first; e1 < lc; e1 += period) {
            int64_t xa = (int64_t)v92_point(m, i, e1) * (1 << V92_XQ) + fb;
            if (xa < 0) xa = -xa;
            if (best < 0 || xa <= best) { best = xa; best_eta = e1; }
        }
        eta = best_eta;
    }
    u = v92_point(m, i, eta);
    e->eta[k] = (int16_t)eta;

    if (f) {
        int64_t acc = 0;
        const int16_t *z2 = f->coef + f->lz1 + f->lp1, *p2 = z2 + f->lz2;
        int64_t xs = (int64_t)u * (1 << V92_XQ) + fb;
        x = xs > INT32_MAX ? INT32_MAX : xs < -INT32_MAX ? -INT32_MAX : (int32_t)xs;
        acc += (int64_t)x * z2[0];
        if (f->lz2 > 1) acc += ref_hdot32(e->xh, e->hp, z2 + 1, f->lz2 - 1u);
        acc += ref_hdot32(e->vh, e->hp, p2, f->lp2) * 2;
        v = ref_rnd_shift(acc, 15);
        out = ref_rnd_shift((int64_t)v * f->gain4, V92_XQ + 18 - 14);
    } else {
        x = u * (1 << V92_XQ);
        v = x;
        out = u;
    }
    e->uh[e->hp] = (int16_t)u;
    e->xh[e->hp] = x;
    e->vh[e->hp] = v;
    e->hp = (uint16_t)(e->hp + 1 == V92_MAX_LSEC ? 0 : e->hp + 1);

    if (s) {
        s->i = (uint8_t)i;
        s->ki = e->ki[i];
        s->y0 = e->y0;
        s->eta = (int16_t)eta;
        s->u = u;
        s->x = x;
        s->v = v;
    }

    if (k == 3)
        e->conv = (uint8_t)v34_conv_next(m->trellis, e->conv, v92_inverse_map(e->eta));
    e->i = (uint8_t)(i + 1 == V92_DFRAME ? 0 : i + 1);
    return out;
}

/* a random filter: sections of up to 40 taps (z2 at least 1), small taps,
 * some of them zero, and up to 40 zeros at the end of each section */
static void random_filt(v92_filt_t *f)
{
    unsigned len[4], s, k, at = 0;
    memset(f, 0, sizeof *f);
    for (s = 0; s < 4; s++) len[s] = rnd_below(41);
    if (len[2] == 0) len[2] = 1;
    f->lz1 = (uint16_t)len[0]; f->lp1 = (uint16_t)len[1]; f->lz2 = (uint16_t)len[2]; f->lp2 = (uint16_t)len[3];
    f->gain4 = (uint16_t)(0x4000 + rnd_below(0x8000));
    for (s = 0; s < 4; s++) {
        unsigned z = rnd_below(len[s] + 1);          /* the last z are zero */
        for (k = 0; k < len[s]; k++, at++)
            f->coef[at] = (k >= len[s] - z || rnd_below(6) == 0) ? 0 : (int16_t)((int32_t)rnd_below(4001) - 2000);
    }
}

/* The filter changed under a running encoder, without v92_enc_init (its
 * lengths below the zeros counted at init among the changes): the library
 * reads no more than the filter it is given, as 347caac did, and gives
 * what 347caac gave */
static void test_v92_live(void)
{
    static const uint8_t M[12] = { 8, 8, 8, 4, 8, 8, 8, 4, 8, 8, 8, 4 };
    static v92_map_t map;
    static v92_filt_t fa, fb;
    static v92_enc_t ea, eb;
    long syms = 0, changes = 0, differ = 0;
    int c, e;
    memset(&map, 0, sizeof map);
    map.K = 33;
    memcpy(map.M, M, 12);
    map.lc[0] = 8;
    for (e = 0; e < 8; e++) map.pt[0][e] = (uint16_t)((2 * e + 1) * 50);
    for (c = 0; c < N_FILT; c++) {
        random_filt(&fa);
        fb = fa;
        CHECK_EQ_I(v92_enc_init(&ea, &map, &fa, get_a, NULL), 0);
        CHECK_EQ_I(v92_enc_init(&eb, &map, &fb, get_b, NULL), 0);
        bit_seed[0] = bit_seed[1] = rnd32();
        for (e = 0; e < 1200; e++, syms++) {
            v92_sym_t sa, sb;
            int32_t oa, ob;
            if (e % 97 == 50) { random_filt(&fa); fb = fa; changes++; }
            oa = v92_enc_symbol(&ea, &sa);
            ob = ref_enc_symbol(&eb, &sb);
            if (oa != ob || sa.i != sb.i || sa.ki != sb.ki || sa.y0 != sb.y0 || sa.eta != sb.eta
                || sa.u != sb.u || sa.x != sb.x || sa.v != sb.v) {
                if (differ++ < 3) printf("  v92 live filter %d, symbol %d: %ld, want %ld\n", c, e, (long)oa, (long)ob);
                break;
            }
        }
    }
    CHECK_EQ_I(differ, 0);
    printf("  v92_enc_symbol, the filter changed without v92_enc_init: %d filters, %ld changes, %ld symbols: %ld differ\n",
           N_FILT, changes, syms, differ);
}

int main(int argc, char **argv)
{
    int full = argc > 1 && strcmp(argv[1], "full") == 0;
    if (argc > 1 && !full) {
        printf("usage: %s [full]\n", argv[0]);
        return 2;
    }
    printf("test_small_exact: Task 35's exact forms against the code before them%s\n", full ? " (full)" : "");
    test_octets();
    test_modulus(full);
    test_scramblers();
    test_x2map();
    test_ec();
    test_v92_trim();
    test_v92_live();
    T_REPORT("test_small_exact");
}
