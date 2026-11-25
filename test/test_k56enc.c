/* K56flex's data encoder (modem/k56enc; docs/history/k56-facts.md "Data",
 * "Robbed bits", "Tables", "Oracle"). Against D-Modem's encoder as data
 * (test/data/k56enc_oracle.h): every rate, pad and robbed-bit mask; then
 * frame and unframe, the bits a frame takes, the shaping sign, Ruling C4's
 * restart and the shell mapper's z8 saturation at M = 16. */
#include <inttypes.h>
#include <string.h>
#include "test.h"
#include "k56enc.h"
#include "k56tab.h"
#include "v90_scr.h"
#include "v90_pcm.h"
#include "data/k56enc_oracle.h"

#define NFRAMES 600

/* the oracle's data bits: xorshift32 from 1, one step a bit */
static int xs_bit(uint32_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return (int)(*x & 1u);
}

static uint32_t fnv_step(uint32_t h, uint8_t o)
{
    return (h ^ o) * 16777619u;
}

/* the pipeline of "Oracle": 600 frames, FNV-1a over the octets; the octets
 * of frames 0, 1 and 599 to f (when not NULL) */
static uint32_t oracle_run(int r, int pad, int mask, uint8_t f[3][K56ENC_N])
{
    k56enc_t e;
    v90_scr_t s;
    uint32_t x = 1, h = 2166136261u;
    uint8_t bits[64], out[K56ENC_N];
    int n, i, k;

    k56enc_init(&e, r, pad, (uint8_t)mask);
    v90_scr_init(&s);
    for (n = 0; n < NFRAMES; n++) {
        k = k56enc_bits(&e);
        for (i = 0; i < k; i++)
            bits[i] = (uint8_t)v90_scramble_gpa(&s, xs_bit(&x));
        k56enc_frame(&e, bits, out);
        for (i = 0; i < K56ENC_N; i++)
            h = fnv_step(h, out[i]);
        if (f && (n == 0 || n == 1 || n == NFRAMES - 1))
            memcpy(f[n == 0 ? 0 : n == 1 ? 1 : 2], out, K56ENC_N);
    }
    return h;
}

static void test_oracle(void)
{
#ifdef TEST_SHORT
    static const int masks[] = { 0x00, 0x24, 0x3f, 0x15 };
    const int nm = 4;
#else
    int masks[64], nm = 64, i;
    for (i = 0; i < 64; i++) masks[i] = i;
#endif
    int r, pad, k, bad = 0, all = 0, fbad = 0;

    for (r = 1; r <= 13; r++)
        for (pad = 0; pad < 3; pad++) {
            uint8_t f[3][K56ENC_N];
            for (k = 0; k < nm; k++) {
                uint32_t h = oracle_run(r, pad, masks[k], masks[k] == 0 ? f : NULL);
                all++;
                if (h != k56enc_oracle_fnv[r - 1][pad][masks[k]]) {
                    if (bad < 8)
                        printf("  oracle: r %d pad %d mask %#04x: %08" PRIx32 ", want %08" PRIx32 "\n",
                               r, pad, masks[k], h, k56enc_oracle_fnv[r - 1][pad][masks[k]]);
                    bad++;
                }
                if (masks[k] == 0)
                    fbad += memcmp(f, k56enc_oracle_frames[r - 1][pad], sizeof f) != 0;
            }
        }
    printf("  oracle: %d of %d rate x pad x mask equal to D-Modem's; frames 0, 1, 599 at mask 0: %d of 39 differ\n",
           all - bad, all, fbad);
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(fbad, 0);
    CHECK_EQ_I(all, 39 * nm);
}

static uint32_t rng = 0x2545f491u;
static int rbit(void)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) & 1u);
}

/* frame and unframe: the bits come back, at every rate and pad */
static void test_roundtrip(void)
{
#ifdef TEST_SHORT
    const int nf = 300;
#else
    const int nf = 2000;
#endif
    static const int masks[2] = { 0x00, 0x3f };
    int r, pad, mk, n, i, bad = 0, all = 0;

    for (r = 1; r <= 13; r++)
        for (pad = 0; pad < 3; pad++)
            for (mk = 0; mk < 2; mk++) {
                k56enc_t e, d;
                uint8_t bits[64], back[64], out[K56ENC_N];
                int ok = 1;
                k56enc_init(&e, r, pad, (uint8_t)masks[mk]);
                k56enc_init(&d, r, pad, (uint8_t)masks[mk]);
                for (n = 0; n < nf && ok; n++) {
                    int k = k56enc_bits(&e);
                    for (i = 0; i < k; i++) bits[i] = (uint8_t)rbit();
                    k56enc_frame(&e, bits, out);
                    if (k56enc_unframe(&d, out, back) != k || memcmp(bits, back, (size_t)k) != 0)
                        ok = 0;
                }
                all++;
                if (!ok) {
                    printf("  roundtrip: r %d pad %d mask %#04x: frame %d differs\n", r, pad, masks[mk], n - 1);
                    bad++;
                }
            }
    printf("  frame / unframe: %d of %d rate x pad x mask, %d frames each\n", all - bad, all, nf);
    CHECK_EQ_I(bad, 0);

    /* an octet that is no point of the constellation */
    {
        k56enc_t d;
        const uint8_t *bios = k56_bios(1, 0);
        uint8_t in[K56ENC_N], back[64], used[128];
        int m, nb = k56_rates[0].M[0] << (k56_rates[0].u - 1);
        memset(used, 0, sizeof used);
        for (i = 0; i < nb; i++) used[bios[i]] = 1;
        for (m = 0; m < 128 && used[m]; m++)
            ;
        k56enc_init(&d, 1, 0, 0);
        for (i = 0; i < K56ENC_N; i++) in[i] = (uint8_t)(0xff - bios[0]);
        in[3] = (uint8_t)(0xff - m);
        CHECK(m < 128);
        CHECK_EQ_I(k56enc_unframe(&d, in, back), -1);
    }
}

/* the bits of each frame of a 3-frame cycle */
static void cycle_bits(int r, int mask, int got[3])
{
    k56enc_t e;
    uint8_t bits[64], out[K56ENC_N];
    int f;
    memset(bits, 0, sizeof bits);
    k56enc_init(&e, r, 0, (uint8_t)mask);
    for (f = 0; f < 3; f++) {
        got[f] = k56enc_bits(&e);
        k56enc_frame(&e, bits, out);
    }
}

static void test_sizes(void)
{
    int g[3], r, f;

    cycle_bits(1, 0, g);
    CHECK_EQ_I(g[0], 32); CHECK_EQ_I(g[1], 32); CHECK_EQ_I(g[2], 32);
    cycle_bits(13, 0, g);
    CHECK_EQ_I(g[0], 56); CHECK_EQ_I(g[1], 56); CHECK_EQ_I(g[2], 56);
    /* 6 robbed: L = 8, n = 8 in every frame */
    cycle_bits(13, 0x3f, g);
    CHECK_EQ_I(g[0], 48); CHECK_EQ_I(g[1], 48); CHECK_EQ_I(g[2], 48);
    /* 1 robbed: L = 2; 2 robbed: L = 4; 4 robbed: L = 6 */
    cycle_bits(13, 0x01, g);
    CHECK_EQ_I(g[0], 54); CHECK_EQ_I(g[1], 54); CHECK_EQ_I(g[2], 54);
    cycle_bits(13, 0x09, g);
    CHECK_EQ_I(g[0], 52); CHECK_EQ_I(g[1], 52); CHECK_EQ_I(g[2], 52);
    cycle_bits(13, 0x0f, g);
    CHECK_EQ_I(g[0], 50); CHECK_EQ_I(g[1], 50); CHECK_EQ_I(g[2], 50);
    /* 3 robbed: R / 1000 - n, n = 5, 4, 3 for cycle symbols 0, 1, 2 */
    cycle_bits(13, 0x07, g);
    CHECK_EQ_I(g[0], 51); CHECK_EQ_I(g[1], 52); CHECK_EQ_I(g[2], 53);
    /* 5 robbed behave as 6 */
    cycle_bits(13, 0x1f, g);
    CHECK_EQ_I(g[0], 48); CHECK_EQ_I(g[1], 48); CHECK_EQ_I(g[2], 48);
    /* at 36000, q = 1: a robbed symbol keeps only its sign */
    cycle_bits(3, 0x3f, g);
    CHECK_EQ_I(g[0], 28);
    /* none at 32000 and 34000 */
    cycle_bits(1, 0x3f, g);
    CHECK_EQ_I(g[0], 32);
    cycle_bits(2, 0x3f, g);
    CHECK_EQ_I(g[0], 34);
    /* every rate, clean: R / 1000 */
    for (r = 1; r <= 13; r++) {
        cycle_bits(r, 0, g);
        for (f = 0; f < 3; f++) CHECK_EQ_I(g[f], 30 + 2 * r);
    }

    /* 5 robbed give the octets of 6 */
    {
        k56enc_t a, b;
        uint8_t bits[64], oa[K56ENC_N], ob[K56ENC_N];
        int n, i, same = 1;
        k56enc_init(&a, 11, 1, 0x1f);
        k56enc_init(&b, 11, 1, 0x3f);
        for (n = 0; n < 30; n++) {
            int k = k56enc_bits(&a);
            if (k != k56enc_bits(&b)) same = 0;
            for (i = 0; i < k; i++) bits[i] = (uint8_t)rbit();
            k56enc_frame(&a, bits, oa);
            k56enc_frame(&b, bits, ob);
            if (memcmp(oa, ob, sizeof oa)) same = 0;
        }
        CHECK(same);
    }
    /* an invalid rate or pad takes no bits */
    {
        k56enc_t e;
        k56enc_init(&e, 0, 0, 0); CHECK_EQ_I(k56enc_bits(&e), 0);
        k56enc_init(&e, 14, 0, 0); CHECK_EQ_I(k56enc_bits(&e), 0);
        k56enc_init(&e, 5, 3, 0); CHECK_EQ_I(k56enc_bits(&e), 0);
    }
}

/* every r-th symbol over the call: no data, the sign set against the
 * running sum of the values sent */
static void test_shaping(void)
{
    static const int rates[4] = { 1, 5, 9, 13 };
    static const int masks[2] = { 0x00, 0x12 };
    int ri, mk, n, i, bad_sign = 0, bad_data = 0, shaped = 0;

    for (ri = 0; ri < 4; ri++)
        for (mk = 0; mk < 2; mk++) {
            int r = rates[ri], per = k56_rates[r - 1].r;
            k56enc_t e, d;
            int64_t sum = 0;
            long sym = 0;
            k56enc_init(&e, r, 1, (uint8_t)masks[mk]);
            k56enc_init(&d, r, 1, (uint8_t)masks[mk]);
            for (n = 0; n < 151; n++) {    /* 151 frames: not a multiple of 3 */
                uint8_t bits[64], back[64], out[K56ENC_N], flip[K56ENC_N];
                int k = k56enc_bits(&e);
                for (i = 0; i < k; i++) bits[i] = (uint8_t)rbit();
                k56enc_frame(&e, bits, out);
                memcpy(flip, out, sizeof flip);
                for (i = 0; i < K56ENC_N; i++, sym++) {
                    if (sym % per == per - 1) {
                        int neg = sum > 0;
                        shaped++;
                        if (neg != !(out[i] & 0x80)) bad_sign++;
                        flip[i] ^= 0x80;
                    }
                    sum += v90_octet_linear(0, out[i]);
                }
                /* the shaping signs inverted: the same bits */
                if (k56enc_unframe(&d, flip, back) != k || memcmp(bits, back, (size_t)k))
                    bad_data++;
            }
        }
    printf("  shaping: %d symbols, %d with the wrong sign, %d frames whose shaping signs carried data\n",
           shaped, bad_sign, bad_data);
    CHECK(shaped > 0);
    CHECK_EQ_I(bad_sign, 0);
    CHECK_EQ_I(bad_data, 0);
}

/* Ruling C4: a renegotiation's new rate is a new encoder; k56enc_init on a
 * used one restarts everything (nothing carries across) */
static void test_restart(void)
{
    k56enc_t a, b;
    uint8_t bits[64], oa[K56ENC_N], ob[K56ENC_N];
    int n, i, same = 1;

    k56enc_init(&a, 7, 0, 0x12);
    for (n = 0; n < 17; n++) {           /* mid-cycle, sum and counter moved */
        int k = k56enc_bits(&a);
        for (i = 0; i < k; i++) bits[i] = (uint8_t)rbit();
        k56enc_frame(&a, bits, oa);
    }
    k56enc_init(&a, 12, 2, 0x07);
    k56enc_init(&b, 12, 2, 0x07);
    for (n = 0; n < 40; n++) {
        int k = k56enc_bits(&a);
        if (k != k56enc_bits(&b)) same = 0;
        for (i = 0; i < k; i++) bits[i] = (uint8_t)rbit();
        k56enc_frame(&a, bits, oa);
        k56enc_frame(&b, bits, ob);
        if (memcmp(oa, ob, sizeof oa)) same = 0;
    }
    CHECK(same);
}

/* 54000 (M = 16): z8's total, 16^8 = 2^32, saturates; the largest 31-bit
 * index still maps and unmaps */
static void test_m16(void)
{
    int pad, i;
    for (pad = 0; pad < 3; pad++) {
        k56enc_t e, d;
        uint8_t bits[64], back[64], out[K56ENC_N];
        int k, K = k56_rates[11].K, j;
        CHECK_EQ_I(k56_rates[11].M[pad], 16);
        CHECK_EQ_I(K, 31);
        for (j = 0; j < 2; j++) {        /* 2^31 - 1, then 0 */
            k56enc_init(&e, 12, pad, 0);
            k56enc_init(&d, 12, pad, 0);
            k = k56enc_bits(&e);
            for (i = 0; i < k; i++) bits[i] = (uint8_t)(i < K ? j == 0 : rbit());
            k56enc_frame(&e, bits, out);
            CHECK_EQ_I(k56enc_unframe(&d, out, back), k);
            CHECK(memcmp(bits, back, (size_t)k) == 0);
        }
    }
}

int main(void)
{
    test_sizes();
    test_roundtrip();
    test_shaping();
    test_restart();
    test_m16();
    test_oracle();
    T_REPORT("k56enc");
}
