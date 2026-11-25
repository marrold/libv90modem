/* modem/v34up (audit Task 24): p34d's data path was its own, without the
 * non-linear decoder and encoder that v34dx's had; both now run v34up_data,
 * which has them. With cfg.nonlinear == 0 (all p34d asks for) they must
 * return their input for every point, and the shared scaling must be p34d's
 * old one (kept here verbatim) for every gain p34d's rates give. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "test.h"
#include "../modem/v34up.h"

static uint64_t rs = 0x9e3779b97f4a7c15u;
static uint64_t rnd64(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}

/* p34d.c's RX_DATA scaling before v34up, verbatim */
static v34_pt_t old_in(int32_t zr, int32_t zi, int64_t g)
{
    v34_pt_t in;
    int32_t vr = (int32_t)(((int64_t)zr * 512) / g), vi = (int32_t)(((int64_t)zi * 512) / g);
    if (vr > 32767) vr = 32767;
    if (vr < -32768) vr = -32768;
    if (vi > 32767) vi = 32767;
    if (vi < -32768) vi = -32768;
    in.re = (int16_t)vr; in.im = (int16_t)vi;
    return in;
}

static v34up_t up;

/* one received point through both: 1 if they differ */
static int scale_differs(int32_t zr, int32_t zi, const v34_pt_t *tent)
{
    v34_pt_t a = old_in(zr, zi, up.gain), b;
    int32_t dr0, di0, dr1, di1;
    up.rx.zr = zr; up.rx.zi = zi;
    b = v34up_scale_in(&up);
    if (a.re != b.re || a.im != b.im) return 1;
    /* back: p34d took the decision as it was; v34up_data encodes it first */
    dr0 = (int32_t)(((int64_t)tent->re * up.gain) / 512); di0 = (int32_t)(((int64_t)tent->im * up.gain) / 512);
    v34up_scale_out(&up, v34_nl_encode(&up.par, *tent), &dr1, &di1);
    return dr0 != dr1 || di0 != di1;
}

int main(void)
{
    static const int syms[2] = { 3000, 3200 };
    long bad = 0, cfgs = 0;
    int s, k;
    int32_t a, b;
    printf("test_v34up: the shared V.34 upstream receiver\n");

    /* the non-linear decoder and encoder off: every point of the int16 grid
     * at step 16 comes back as it went in */
    {
        v34_cfg_t c;
        memset(&c, 0, sizeof c);
        c.symbol_rate = 3200; c.data_rate = 28800; c.trellis = V34_TRELLIS_16; c.scrambler = V34_GPA;
        CHECK_EQ_I(v34_params_init(&up.par, &c), 0);
        CHECK_EQ_I(up.par.cfg.nonlinear, 0);
        for (a = -32768; a <= 32767; a += 16)
            for (b = -32768; b <= 32767; b += 16) {
                v34_pt_t x, y, z;
                x.re = (int16_t)a; x.im = (int16_t)b;
                y = v34_nl_decode(&up.par, x); z = v34_nl_encode(&up.par, x);
                bad += y.re != x.re || y.im != x.im || z.re != x.re || z.im != x.im;
            }
        printf("  v34_nl_decode / v34_nl_encode, nonlinear 0, 4096 x 4096 points: %ld differ\n", bad);
        CHECK_EQ_I(bad, 0);
    }

    /* p34d's every rate (as upstream_start: 16 states, linear): the scaling
     * in and back out against p34d's old code */
    bad = 0;
    for (s = 0; s < 2; s++)
        for (k = 0; k < 13; k++) {
            v34_cfg_t c;
            int i;
            memset(&c, 0, sizeof c);
            c.symbol_rate = (uint16_t)syms[s]; c.data_rate = (uint16_t)(4800 + 2400 * k);
            c.trellis = V34_TRELLIS_16; c.expanded = 0; c.nonlinear = 0; c.scrambler = V34_GPA;
            if (v34up_data_begin(&up, &c, NULL, NULL) != 0) continue;
            cfgs++;
            for (i = 0; i < 200000; i++) {
                v34_pt_t t;
                int64_t vr = (int64_t)(rnd64() >> 32) - 2147483648, vi = (int64_t)(rnd64() >> 32) - 2147483648;
                int sh = (int)(rnd64() & 31);
                /* every magnitude */
                int32_t zr = (int32_t)(vr / ((int64_t)1 << sh)), zi = (int32_t)(vi / ((int64_t)1 << ((sh + (int)(rnd64() & 7)) & 31)));
                t.re = (int16_t)((int32_t)(rnd64() >> 48) - 32768); t.im = (int16_t)((int32_t)(rnd64() >> 48) - 32768);
                bad += scale_differs(zr, zi, &t);
            }
            {
                v34_pt_t t;
                t.re = 32767; t.im = -32768;
                bad += scale_differs(INT32_MAX, INT32_MIN, &t) + scale_differs(0, -1, &t);
            }
        }
    printf("  scaling in and out, %ld rate configurations x 200000 points: %ld differ\n", cfgs, bad);
    CHECK(cfgs >= 20);
    CHECK_EQ_I(bad, 0);
    T_REPORT("test_v34up");
}
