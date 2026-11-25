/* modem/v90dil: the DIL the analogue modem asks for, what it makes of it, and
 * the constellations it asks for in CPt and CP.
 *
 *   - the descriptor survives v90_dil_build / v90_dil_parse, and walking it
 *     gives exactly what the digital transmitter (v90tx) sends as the DIL;
 *   - the analysis of noisy levels as a pad, a robbed bit and the law make
 *     them: the robbed interval found, the levels within the noise;
 *   - the constellations: valid CP / CPt (v90_map_from_cp), every point at
 *     least z sigma from its neighbour in the levels that really arrive,
 *     the average power of 8.5.2 (computed here in double) within Table 15,
 *     the rate falling with the noise, Jd's mask and our ceiling obeyed. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "v90dil.h"
#include "v90arx.h"
#include "v90_tx.h"
#include "v90_pcm.h"
#include "v90_enc.h"
#include "pcmenc.h"
#include "noise.h"

#define UINFO 90

static v90tx_t tx;

static void test_design(void)
{
    v90_dil_t d, p;
    v90dil_cur_t c;
    uint8_t bits[V90_DIL_MAX_BITS];
    int n, k, bad = 0, firsts = 0, firsts_ok = 1;
    printf("the DIL asked for\n");
    v90dil_design(&d, UINFO);
    n = v90_dil_build(&d, bits);
    printf("  N %d, LSP %d, LTP %d, H1 %d, descriptor %d bits\n", d.n, d.lsp, d.ltp, d.h[0], n);
    CHECK(n > 0);
    CHECK_EQ_I(v90_dil_parse(bits, n, &p), n);
    CHECK(memcmp(&d, &p, sizeof d) == 0);
    CHECK_EQ_I(d.n, V90DIL_NU);
    /* as the digital modem sends it, two passes */
    v90tx_init(&tx, 0, UINFO);
    CHECK_EQ_I(v90tx_load_dil(&tx, &d), 0);
    CHECK_EQ_I(v90tx_select(&tx, V90TX_DIL), 0);
    v90dil_cur_init(&c);
    for (k = 0; k < 2 * V90DIL_PASS; k++) {
        uint8_t o = v90tx_tick(&tx);
        int u, s, f = v90dil_next(&d, &c, &u, &s);
        if (v90tx_signal(&tx) != V90TX_DIL || (unsigned)u != v90_octet_ucode(0, o) || s != v90_octet_sign(o)) bad++;
        firsts += f;
        if (f != (k % 18 == 0)) firsts_ok = 0;
    }
    printf("  two passes against v90tx: %d symbols differ, %d segment starts\n", bad, firsts);
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(firsts, 2 * V90DIL_NU);
    CHECK(firsts_ok);
    /* no training symbol above a third of full scale (either law): a
     * softphone's gain control held down louder codewords and read what
     * followed low (modem/v90dil.h); every Ucode up to there is asked for */
    {
        int law, i, top[2] = { -1, -1 }, all = 1;
        uint8_t seen[128];
        for (law = 0; law < 2; law++) {
            int32_t fs = v90_ucode_linear(law, 127);
            memset(seen, 0, sizeof seen);
            for (i = 0; i < d.n; i++) {
                int u = d.train[i] & 127;
                seen[u] = 1;
                if (u > top[law]) top[law] = u;
            }
            CHECK(3 * v90_ucode_linear(law, (unsigned)top[law]) <= fs);
            for (i = 0; i < 128 && 3 * v90_ucode_linear(law, (unsigned)i) <= fs; i++) if (!seen[i]) all = 0;
        }
        printf("  loudest Ucode asked for %d (mu-law %d, A-law %d of full scale %d, %d)\n", top[0],
               (int)v90_ucode_linear(0, (unsigned)top[0]), (int)v90_ucode_linear(1, (unsigned)top[1]),
               (int)v90_ucode_linear(0, 127), (int)v90_ucode_linear(1, 127));
        CHECK(all);
    }
}

/* the level Ucode u arrives at through pad and robbed bit (Table 1 units) */
static int32_t arrives(int alaw, double pad, unsigned u, int robbed)
{
    uint8_t o = v90_pcm_octet(alaw, u, 1);
    if (pad != 0.0) o = pcm_encode(alaw, (int16_t)floor(v90_octet_linear(alaw, o) * pow(10, -pad / 20) + 0.5));
    if (robbed) o |= 1;
    return v90_octet_linear(alaw, o);
}

/* the receiver's scale: UINFO at its Table 1 level */
static double true_level(int alaw, double pad, int rslot, int s, unsigned u)
{
    double k = (double)v90_ucode_linear(alaw, UINFO) / arrives(alaw, pad, UINFO, 0);
    return arrives(alaw, pad, u, s == rslot) * k;
}

/* a positive double in tenths, as two integers (no decimal rounding by the C library) */
#define T10(x) (int)((x) * 10 + 0.5) / 10, (int)((x) * 10 + 0.5) % 10

static rng_t rng;
static v90dil_acc_t acc;
static v90dil_res_t res;

/* the DIL's symbols through the channel, `passes` passes, noise sigma */
static double bias = 0;      /* a level bias by interval: the DIL's residual interference */
static void measure(int alaw, double pad, int rslot, double sigma, int passes)
{
    v90_dil_t d;
    v90dil_cur_t c;
    int32_t k;
    v90dil_design(&d, UINFO);
    v90dil_cur_init(&c);
    v90dil_acc_init(&acc);
    for (k = 0; k < (int32_t)passes * V90DIL_PASS; k++) {
        int u, s, slot = (int)(k % 6);
        double y;
        (void)v90dil_next(&d, &c, &u, &s);
        y = true_level(alaw, pad, rslot, slot, (unsigned)u) * (1 + bias * (slot - 2.5) / 2.5) * (s ? 1 : -1) + sigma * rng_gauss(&rng);
        v90dil_acc_add(&acc, slot, u, s, (int32_t)floor(y * (1 << V90ARX_YF) + 0.5));
    }
    v90dil_analyse(&acc, alaw, UINFO, &res);
}

/* 8.5.2's average power in double, exactly as the formula reads */
static double power_dbl(int alaw, int K, const v90_map_t *m)
{
    double R[7], A[7], sum = 0, two = pow(2, K);
    int Ki[6], i, j;
    R[0] = two - 1; A[0] = 1;
    for (i = 0; i < 6; i++) {
        Ki[i] = (int)fmod(R[i], m->M[i]);
        R[i + 1] = (R[i] - Ki[i]) / m->M[i];
        A[i + 1] = A[i] * m->M[i];
    }
    for (i = 0; i < 6; i++)
        for (j = 0; j < m->M[i]; j++) {
            double p = pow(v90_ucode_linear(alaw, m->C[i][j]), 2), n;
            if (j < Ki[i]) n = A[i] * (R[i + 1] + 1);
            else if (j == Ki[i]) n = two - A[i] * (R[i] - R[i + 1]);
            else n = A[i] * R[i + 1];
            sum += p * n;
        }
    return sum / (6 * two);
}

/* checks of a designed set; returns the bit rate */
static int check_cp(const v90_cp_t *cp, const v90dil_choice_t *ch, int alaw, double pad, int rslot, double sigma,
                    int z_q4, int max_code, int is_cp)
{
    v90_map_t m;
    int i, j, worst_ok = 1;
    double lim = v90dil_limit(max_code), pw;
    CHECK_EQ_I(cp->is_cp, is_cp);
    if (ch->K == 0) { CHECK_EQ_I(cp->drn, 0); return 0; }
    CHECK_EQ_I(v90_map_from_cp(&m, cp), 0);
    CHECK_EQ_I(m.Sr, 0);
    CHECK_EQ_I(cp->alaw, alaw);
    pw = power_dbl(alaw, m.K, &m);
    if (!(pw <= lim * lim)) printf("    power over the limit\n");
    CHECK(pw <= lim * lim);
    CHECK(ch->power >= pw);
    /* the levels that really arrive: every point d = z sigma (less 15 % for
     * what the measurement gets wrong) from the next, the lowest d / 2 */
    for (i = 0; i < 6; i++) {
        double d = z_q4 / 16.0 * sigma * 0.85, last = 0;
        for (j = m.M[i] - 1; j >= 0; j--) {
            double l = true_level(alaw, pad, rslot, i, m.C[i][j]);
            if (j == m.M[i] - 1 ? 2 * l < d : l - last < d) worst_ok = 0;
            last = l;
        }
    }
    CHECK(worst_ok);
    return is_cp ? V90_RATE_BPS(cp->drn - 1) : (cp->drn + 8) * 8000 / 6;
}

static int design(int alaw, double sigma, int z_q4, int is_cp, uint32_t rates, int max_bps, v90_cp_t *cp, v90dil_choice_t *ch)
{
    v90dil_req_t q;
    int i;
    memset(&q, 0, sizeof q);
    q.alaw = alaw; q.is_cp = is_cp; q.z_q4 = z_q4; q.max_tx_code = 23; q.rates = rates; q.max_bps = max_bps;
    q.up_rates = 0x7ff;
    for (i = 0; i < 6; i++) q.sigma2_q8[i] = (uint32_t)(sigma * sigma * 256);
    return v90dil_design_cp(&res, &q, cp, ch);
}

int main(void)
{
    static const struct { const char *name; int alaw; double pad; int rslot; } CH[] = {
        { "mu-law, clean", 0, 0, -1 }, { "mu-law, 3 dB pad", 0, 3, -1 }, { "mu-law, 6 dB pad", 0, 6, -1 },
        { "mu-law, robbed bit in interval 2", 0, 0, 2 }, { "mu-law, 6 dB pad and robbed bit in 5", 0, 6, 5 },
        { "A-law, clean", 1, 0, -1 } };
    static const double SIG[] = { 2, 5, 10, 20, 40 };
    int i, j;
    v90_cp_t cp, cp2;
    v90dil_choice_t ch;
    uint8_t bits[V90_CP_MAX_BITS];

    rng_seed(&rng, 11);
    test_design();

    printf("Table 15\n");
    CHECK_EQ_I(v90dil_limit(0), 15124);
    CHECK_EQ_I(v90dil_limit(23), 4024);
    CHECK_EQ_I(v90dil_limit(31), 2540);

    printf("analysis: 4 passes (at least 8 symbols per interval and Ucode), noise 8 units\n");
    for (i = 0; i < (int)(sizeof CH / sizeof CH[0]); i++) {
        int s, u, far = 0;
        double worst = 0;
        measure(CH[i].alaw, CH[i].pad, CH[i].rslot, 8, 4);
        CHECK_EQ_I(v90dil_acc_min(&acc), 8);
        for (s = 0; s < 6; s++) {
            CHECK_EQ_I(res.robbed[s], s == CH[i].rslot);
            for (u = 0; u < V90DIL_NU; u++) {
                double e = fabs(res.lv[s][u] / (double)(1 << V90ARX_YF) - true_level(CH[i].alaw, CH[i].pad, CH[i].rslot, s, (unsigned)u));
                if (e > worst) worst = e;
                if (e > 4 * 8 / sqrt(8)) far++;
            }
        }
        printf("  %-40s robbed %d%d%d%d%d%d, deviation from Table 1 %d.%d %%, worst level error %d.%d units\n", CH[i].name,
               res.robbed[0], res.robbed[1], res.robbed[2], res.robbed[3], res.robbed[4], res.robbed[5],
               res.dev_q8 * 1000 / 256 / 10, res.dev_q8 * 1000 / 256 % 10, T10(worst));
        CHECK_EQ_I(far, 0);
        if (CH[i].pad == 0 && CH[i].rslot < 0) CHECK(res.dev_q8 < 4); else CHECK(res.dev_q8 > 4);
    }

    printf("the same with a bias of up to 0.8 %% by interval (what the DIL's patterns leave): Ucodes 64 and up\n");
    bias = 0.008;
    for (i = 0; i < (int)(sizeof CH / sizeof CH[0]); i++) {
        int s, u;
        double worst = 0;
        measure(CH[i].alaw, CH[i].pad, CH[i].rslot, 8, 4);
        for (s = 0; s < 6; s++) for (u = 64; u < V90DIL_NU; u++) {
            double e = fabs(res.lv[s][u] / (double)(1 << V90ARX_YF) - true_level(CH[i].alaw, CH[i].pad, CH[i].rslot, s, (unsigned)u));
            if (e > worst) worst = e;
        }
        printf("  %-40s UINFO arrives as Ucode %d, worst level error %d.%d units\n", CH[i].name, res.uinfo_as, T10(worst));
        CHECK(worst < 2);
    }
    bias = 0;

    printf("CP: rate by noise (sigma, units) and channel, d >= 12 sigma, -12 dBm0 limit\n");
    printf("  %-40s", "");
    for (j = 0; j < (int)(sizeof SIG / sizeof SIG[0]); j++) printf(" %8d", (int)SIG[j]);
    printf("\n");
    for (i = 0; i < (int)(sizeof CH / sizeof CH[0]); i++) {
        int last = 1 << 30;
        printf("  %-40s", CH[i].name);
        for (j = 0; j < (int)(sizeof SIG / sizeof SIG[0]); j++) {
            int k, bps;
            measure(CH[i].alaw, CH[i].pad, CH[i].rslot, SIG[j] / 2, 8);
            k = design(CH[i].alaw, SIG[j], 12 * 16, 1, 0, 0, &cp, &ch);
            bps = check_cp(&cp, &ch, CH[i].alaw, CH[i].pad, CH[i].rslot, SIG[j], 12 * 16, 23, 1);
            printf(" %8d", bps);
            CHECK(k == ch.K);
            CHECK(bps <= last);
            last = bps;
        }
        printf("\n");
    }
    /* a clean line with little noise carries the top rates */
    measure(0, 0, -1, 1, 8);
    design(0, 2, 12 * 16, 1, 0, 0, &cp, &ch);
    CHECK(ch.bps >= 52000);
    /* sequences: build and parse */
    {
        int n = v90_cp_build(&cp, bits);
        CHECK(n > 0);
        CHECK_EQ_I(v90_cp_parse(bits, n, &cp2), n);
        CHECK(memcmp(&cp, &cp2, sizeof cp) == 0);
    }
    printf("ceilings\n");
    design(0, 2, 12 * 16, 1, (1u << 10) - 1, 0, &cp, &ch);       /* Jd: up to rate index 9 */
    printf("  Jd allows up to %d: %d\n", V90_RATE_BPS(9), ch.bps);
    CHECK_EQ_I(ch.bps, V90_RATE_BPS(9));
    design(0, 2, 12 * 16, 1, 0, 44000, &cp, &ch);
    printf("  our ceiling 44000: %d\n", ch.bps);
    CHECK_EQ_I(ch.bps, 44000);
    design(0, 2000, 12 * 16, 1, 0, 0, &cp, &ch);
    printf("  noise 2000 units: K %d, drn %d\n", ch.K, cp.drn);
    CHECK_EQ_I(ch.K, 0);
    CHECK_EQ_I(cp.drn, 0);

    printf("CPt: d >= 20 sigma\n");
    for (j = 0; j < (int)(sizeof SIG / sizeof SIG[0]); j++) {
        measure(0, 6, 2, SIG[j] / 2, 8);
        design(0, SIG[j], 20 * 16, 0, 0, 0, &cp, &ch);
        printf("  sigma %4d: K %d, %d bit/s, z %d.%d\n", (int)SIG[j], ch.K, check_cp(&cp, &ch, 0, 6, 2, SIG[j], 20 * 16, 23, 0), ch.z_q4 * 10 / 16 / 10, ch.z_q4 * 10 / 16 % 10);
        CHECK(ch.K >= 6 && ch.K <= 24);
    }

    printf("8.5.2's power, against the formula in double\n");
    {
        v90_map_t m;
        int bad = 0, t;
        for (t = 0; t < 200; t++) {
            int K = 15 + t % 22, alaw = t & 1, ii, M;
            memset(&m, 0, sizeof m);
            M = (int)ceil(pow(2, K / 6.0)) + t % 3;
            if (M > 120) M = 120;
            for (ii = 0; ii < 6; ii++) {
                uint8_t mask[16]; int kk;
                memset(mask, 0, sizeof mask);
                for (kk = 0; kk < M; kk++) { int u = 127 - ((kk * 7 + ii) % 128); mask[u >> 3] |= (uint8_t)(1 << (u & 7)); }
                v90_map_set_points(&m, ii, mask);
            }
            {
                double pd = power_dbl(alaw, K, &m);
                uint32_t pi = v90dil_power(alaw, K, m.M, (const uint8_t (*)[128])m.C);
                if (!(pi >= pd && pi <= pd * 1.001 + 2)) bad++;
            }
        }
        printf("  200 sets: %d outside [exact, exact + 0.1 %%]\n", bad);
        CHECK_EQ_I(bad, 0);
    }
    T_REPORT("v90dil");
}
