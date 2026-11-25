/* modem/orig: the calling modem up to V.32bis data mode. Against the
 * library's answering modem (modem/answer, V.8 selecting V.32bis) on the far
 * port of sim/v32line, the caller at the network side: a clean and an
 * impaired line, rate ceilings, retrains from either end, the answerer
 * going away, the answerer calling for a cleardown. And V.32bis Annex A: a
 * pre-V.8 answerer that sends ANS (V.25) and then V.32bis (the library's
 * v32 answering modem driven directly), and pre-V.8 V.22bis and V.21
 * answerers while V.32bis is on offer. Pseudo-random data both ways, every
 * bit checked. There is no independent V.32bis answerer to call (spandsp has
 * none): the answerer here is the library's, itself checked against the
 * independent caller sim/v32call (test_answer_v32, v32/test). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/orig.h"
#include "../modem/answer.h"
#include "../modem/v14.h"
#include "v32line.h"

#define FS 8000
#define TWO_PI 6.283185307179586
#define PI 3.141592653589793
#define MS(ms) ((long)(ms) * FS / 1000)

#define SECS 2                       /* (the same on ARM: the output must be the host's) */

/* 64 zeros, a one, then a pseudo-random stream; the checker finds the one */
typedef struct { uint32_t l; int n; } src_t;
typedef struct { uint32_t l; int zeros, synced; long bits, errs; } chk_t;
static int src_bit(void *u)
{
    src_t *s = u;
    if (s->n < 65) return s->n++ == 64;
    s->l = s->l * 1103515245u + 12345u;
    return (s->l >> 16) & 1;
}
static void chk_bit(void *u, int b)
{
    chk_t *c = u;
    if (!c->synced) {
        if (!b) c->zeros++;
        else { if (c->zeros >= 48) c->synced = 1; c->zeros = 0; }
        return;
    }
    c->l = c->l * 1103515245u + 12345u;
    c->bits++;
    c->errs += b != (int)((c->l >> 16) & 1);
}
static src_t c_src, a_src; static chk_t c_chk, a_chk;
static int  c_get(void *u) { (void)u; return src_bit(&c_src); }
static void c_put(void *u, int b) { (void)u; chk_bit(&c_chk, b); }
static int  a_get(void *u) { (void)u; return src_bit(&a_src); }
static void a_put(void *u, int b) { (void)u; chk_bit(&a_chk, b); }
static void data_reset(void)
{
    memset(&c_src, 0, sizeof c_src); memset(&a_src, 0, sizeof a_src);
    memset(&c_chk, 0, sizeof c_chk); memset(&a_chk, 0, sizeof a_chk);
    c_src.l = a_chk.l = 1; a_src.l = c_chk.l = 7;
}

/* V.21 / V.22bis legacy peers carry text: V.14 */
static v14_t c_dte, a_dte;

enum { P_ANSWER, P_LEGACY_V32, P_LEGACY_V22, P_LEGACY_V21, P_LEGACY_AUTO, P_SILENT };
static int peer_kind, peer_mute;
static long peer_n, ans_ms;
static answer_t A;                 /* the library's answerer */
static v32_t PV32;                 /* a pre-V.8 V.32bis answerer */
static v22bis_t PV;                /* a pre-V.8 V.22bis answerer */
static v21mod_t PFM; static v21demod_t PFD; static arx_t PU; static int pfbit = 1;
static uint8_t a_got[256]; static int a_ngot;   /* what the legacy V.21 peer received */
static long t_aa;                  /* when the caller's AA reached the peer (legacy V.32bis) */

static int16_t clip(double x) { long q = lrint(x); return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q); }

/* V.25 ANS: 2100 Hz, reversals every 450 ms, for ans_ms; then silence */
static double ans(long n)
{
    double t = (double)n / FS, ph = TWO_PI * 2100 * t;
    if (n >= MS(ans_ms)) return 0;
    if (((long)(t / 0.45)) % 2) ph += PI;
    return 4243 * cos(ph);
}

/* the 1800 Hz of AA at the peer (a crude correlator, to time it) */
static double aa_i, aa_q;
static void watch_aa(double rx)
{
    double ph = TWO_PI * 1800 * peer_n / FS;
    aa_i += (rx * cos(ph) - aa_i) / 64; aa_q += (rx * sin(ph) - aa_q) / 64;
    if (!t_aa && aa_i * aa_i + aa_q * aa_q > 200.0 * 200.0) t_aa = peer_n;
}

static double peer(void *u, double rx)
{
    int16_t r = clip(rx);
    (void)u;
    peer_n++;
    if (peer_mute) return 0;
    switch (peer_kind) {
    case P_ANSWER:
        return answer_tick(&A, r);
    case P_LEGACY_V32:
        /* V.32bis 6.2: the answer tone, 75 ms of silence, then AC ... */
        watch_aa(rx);
        if (peer_n < MS(ans_ms)) return ans(peer_n);
        if (peer_n < MS(ans_ms + 75)) return 0;
        return v32_tick(&PV32, r);
    case P_LEGACY_V22:
        watch_aa(rx);
        if (peer_n < MS(ans_ms)) return ans(peer_n);
        if (peer_n < MS(ans_ms + 75)) return 0;
        return v22m_tick(&PV, r);
    case P_LEGACY_AUTO:
        /* Annex A.2.2, AA not heard during the answer tone: the
         * unscrambled binary 1 (USB1) for Ta = 3 s, no S1 / SB1 in
         * reply, then V.32bis from AC */
        watch_aa(rx);
        if (peer_n < MS(ans_ms)) return ans(peer_n);
        if (peer_n < MS(ans_ms + 75)) return 0;
        if (peer_n < MS(ans_ms + 75 + 3000)) return v22m_tick(&PV, r);
        return v32_tick(&PV32, r);
    case P_LEGACY_V21: {
        /* answer tone, then a steady mark on channel 2, our text after 1 s */
        int done, c;
        static long cd;
        int level;
        if (peer_n < MS(ans_ms)) return ans(peer_n);
        level = v21demod_tick(&PFD, r);
        if (PFD.energy < v21demod_energy_of(&PFD, 110)) { level = 1; cd = 0; } else cd++;
        c = arx_tick(&PU, level);
        if (cd < MS(100)) c = -1;
        if (c >= 0 && a_ngot < (int)sizeof a_got) a_got[a_ngot++] = (uint8_t)c;
        {
            double out = v21mod_tick(&PFM, pfbit, &done);
            if (done) pfbit = peer_n > MS(ans_ms + 1000) ? v14_getbit(&a_dte) : 1;
            return out;
        }
    }
    default:
        return 0;
    }
}

static v32line_t L;
static orig_t O;
static long now;
static int16_t o_rx;

/* 0: clean (mu-law, 5 ms); 1: 10 dB loss, echoes 12-18 dB down at both
 * hybrids, -45 dBm0 noise, 20 ms each way, 1 Hz, 100 ppm; 2: the same with
 * -60 dBm0 of noise and no carrier shift */
static void line(v32line_cfg *lc, int kind)
{
    memset(lc, 0, sizeof *lc);
    lc->mulaw = 1; lc->seed = 3; lc->delay = 40;
    lc->noise_a_dbm0 = lc->noise_c_dbm0 = -300; lc->near_db = lc->far_db = lc->c_near_db = lc->c_far_db = 300;
    if (kind) {
        lc->loss_db = 10; lc->delay = 160;
        lc->near_db = 12; lc->far_db = 15; lc->c_near_db = 12; lc->c_far_db = 18;
        lc->noise_a_dbm0 = lc->noise_c_dbm0 = kind == 1 ? -45 : -60;
        lc->freq_offset = kind == 1 ? 1 : 0; lc->ppm = 100;
    }
}

static void start(int kind, unsigned our_modes, unsigned their_modes, int line_kind, int max_bps, int pre_v8_rates)
{
    v32line_cfg lc; orig_cfg oc; answer_cfg ac;
    line(&lc, line_kind);
    v32line_init(&L, &lc);
    data_reset();
    v14_init(&c_dte); v14_init(&a_dte);
    memset(&oc, 0, sizeof oc);
    oc.level = 3000; oc.modes = our_modes; oc.max_bps = max_bps; oc.user = NULL;
    if (kind == P_LEGACY_V22 || kind == P_LEGACY_V21) { oc.getbit = v14_getbit; oc.putbit = v14_putbit; oc.user = &c_dte; }
    else { oc.getbit = c_get; oc.putbit = c_put; }
    orig_init(&O, &oc);
    peer_kind = kind; peer_n = 0; peer_mute = 0; pfbit = 1; a_ngot = 0; t_aa = 0; aa_i = aa_q = 0;
    if (kind == P_ANSWER) {
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = their_modes; ac.getbit = a_get; ac.putbit = a_put;
        answer_init(&A, &ac);
    }
    if (kind == P_LEGACY_V32 || kind == P_LEGACY_AUTO) {
        v32_cfg vc;
        memset(&vc, 0, sizeof vc);
        vc.level = 3000; vc.rates = pre_v8_rates; vc.getbit = a_get; vc.putbit = a_put;
        v32_init(&PV32, &vc);
    }
    if (kind == P_LEGACY_V22 || kind == P_LEGACY_AUTO) v22m_init(&PV, V22_ANSWERER, 1, 3000, v14_getbit, v14_putbit, &a_dte);
    if (kind == P_LEGACY_V21) {
        v21mod_init(&PFM, V21H_MARK, V21H_SPACE, FS, 300, 4243);
        v21demod_init(&PFD, V21L_MARK, V21L_SPACE, FS, 300);
        arx_init(&PU, FS, 300);
    }
    now = 0; o_rx = 0;
}

static void run(long samples)
{
    while (samples-- > 0) { o_rx = v32line_tick(&L, orig_tick(&O, o_rx), peer, NULL); now++; }
}

static int peer_connected(void)
{
    switch (peer_kind) {
    case P_ANSWER: return A.connected && !A.retraining;
    case P_LEGACY_V32: case P_LEGACY_AUTO: return PV32.connected && !PV32.retraining;
    case P_LEGACY_V22: return PV.tx_ready && PV.rx_ready;
    default: return 1;
    }
}

/* until both ends are connected and moving data (or the caller failed): the time, or -1 */
static double to_data(long max_ms)
{
    long end = now + MS(max_ms);
    while (now < end) {
        run(8);
        if (O.phase == ORIG_FAILED) return -1;
        if (O.connected && orig_moving(&O) && peer_connected()) return now / (double)FS;
    }
    return -1;
}

/* pseudo-random data both ways for SECS: all of it, no errors */
static void data(const char *what, int bps)
{
    run(MS(500));                    /* (past the sync pattern that went first) */
    data_reset();
    run(MS(SECS * 1000));
    printf("    %s: caller got %ld bits, %ld bad; answerer got %ld bits, %ld bad\n", what, c_chk.bits, c_chk.errs, a_chk.bits, a_chk.errs);
    CHECK(c_chk.bits > (long)bps * SECS * 3 / 4); CHECK(a_chk.bits > (long)bps * SECS * 3 / 4);
    CHECK_EQ_I(c_chk.errs, 0); CHECK_EQ_I(a_chk.errs, 0);
}

static const unsigned ANS_ALL = V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21;

static void via_v8(void)
{
    double t;
    long t0;

    printf("V.8 then V.32bis, against the library's answerer\n");
    start(P_ANSWER, ANS_ALL, ANS_ALL, 0, 0, 0);
    t = to_data(20000);
    printf("  clean line: data at %.2f s, %d bit/s, modulation %d, V.8 %d, SNR %.1f dB, round trip %d samples; TRN: ours %u T, the answerer's %u T\n",
           t, O.bitrate, O.modulation, O.used_v8, orig_snr_db10(&O) / 10.0, orig_rtd(&O),
           O.e.leg.v32.seg_len[V32S_C_TRN], A.u.v32.seg_len[V32S_TRN1]);
    CHECK(t > 0);
    CHECK_EQ_I(O.phase, ORIG_V32);
    CHECK_EQ_I(O.modulation, ORIG_MOD_V32BIS); CHECK_EQ_I(O.bitrate, 14400); CHECK_EQ_I(O.used_v8, 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V32BIS); CHECK_EQ_I(A.bitrate, 14400);
    CHECK(orig_snr_db10(&O) > 250);
    data("14400 bit/s", 14400);

    /* the answerer retrains (7.2): we follow (7.1), data goes on */
    CHECK_EQ_I(answer_retrain(&A), 0);
    t0 = now;
    run(MS(300));
    CHECK_EQ_I(orig_moving(&O), 0); CHECK_EQ_I(O.connected, 1);
    t = to_data(10000);
    printf("    retrain started by the answerer: data again after %.2f s; ours %d, theirs %d\n", (now - t0) / (double)FS, O.e.leg.v32.retrains, A.retrains);
    CHECK(t > 0); CHECK_EQ_I(O.e.leg.v32.retrains, 1); CHECK_EQ_I(A.retrains, 1);
    data("after it", O.bitrate);

    /* ours */
    CHECK_EQ_I(orig_retrain(&O), 0);
    t0 = now;
    run(MS(300));
    CHECK_EQ_I(orig_moving(&O), 0); CHECK_EQ_I(O.connected, 1);
    t = to_data(10000);
    printf("    retrain started by the caller: data again after %.2f s; ours %d, theirs %d\n", (now - t0) / (double)FS, O.e.leg.v32.retrains, A.retrains);
    CHECK(t > 0); CHECK_EQ_I(O.e.leg.v32.retrains, 2); CHECK_EQ_I(A.retrains, 2);
    data("after it", O.bitrate);

    /* the answerer goes away */
    peer_mute = 1;
    run(MS(1500));
    printf("    the answerer silent: phase %d, end %d\n", O.phase, O.end);
    CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_LOST); CHECK_EQ_I(O.connected, 0);

    start(P_ANSWER, ANS_ALL, ANS_ALL, 1, 0, 0);
    t = to_data(20000);
    printf("  10 dB loss, echoes 12-18 dB, -45 dBm0 noise, 20 ms, 1 Hz, 100 ppm: data at %.2f s, %d bit/s, SNR %.1f dB\n",
           t, O.bitrate, orig_snr_db10(&O) / 10.0);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V32BIS); CHECK(O.bitrate >= 9600);
    data("data", O.bitrate);

    start(P_ANSWER, ANS_ALL, ANS_ALL, 2, 0, 0);
    t = to_data(20000);
    printf("  the same with -60 dBm0 of noise and no carrier shift: data at %.2f s, %d bit/s\n", t, O.bitrate);
    CHECK(t > 0); CHECK_EQ_I(O.bitrate, 14400);
    data("data", O.bitrate);

    /* the realistic line again with our Te moved (V.8 ends a few ms
     * earlier or later): the rate must not depend on that moment. At 20 to
     * 22 dB both receivers' error allows 9600 (v32_rate_mse); before the
     * "robust" branch 5 of 14 calls took 7200 or 4800 (docs/history/PROGRESS.md) */
    {
#ifdef TEST_SHORT
        enum { NTE = 3, TSTEP = 48 };
#else
        enum { NTE = 14, TSTEP = 10 };
#endif
        int k, low = 0, lo = 99999;
        for (k = 0; k < NTE; k++) {
            start(P_ANSWER, ANS_ALL, ANS_ALL, 1, 0, 0);
            O.v8.cfg.te_ms = 1000 + TSTEP * k;
            t = to_data(20000);
            run(MS(500)); data_reset(); run(MS(SECS * 1000));
            if (t < 0 || O.bitrate < 9600 || c_chk.errs || a_chk.errs || c_chk.bits < 9600L * SECS * 3 / 4) {
                low++;
                printf("    Te %d ms: data at %.2f s, %d bit/s, %ld / %ld bad bits\n", 1000 + TSTEP * k, t, O.bitrate, c_chk.errs, a_chk.errs);
            }
            if (O.bitrate < lo) lo = O.bitrate;
        }
        printf("  the same line, Te moved: %d of %d calls under 9600 or with errors (lowest %d bit/s)\n", low, NTE, lo);
        CHECK_EQ_I(low, 0);
    }

    printf("  rate ceilings of ours\n");
    start(P_ANSWER, ANS_ALL, ANS_ALL, 0, 9600, 0);
    t = to_data(20000);
    printf("    9600: data at %.2f s, %d bit/s (answerer %d)\n", t, O.bitrate, A.bitrate);
    CHECK(t > 0); CHECK_EQ_I(O.bitrate, 9600); CHECK_EQ_I(A.bitrate, 9600);
    data("9600 bit/s", 9600);
    start(P_ANSWER, ANS_ALL, ANS_ALL, 0, 4800, 0);
    t = to_data(20000);
    printf("    4800: data at %.2f s, %d bit/s (answerer %d)\n", t, O.bitrate, A.bitrate);
    CHECK(t > 0); CHECK_EQ_I(O.bitrate, 4800); CHECK_EQ_I(A.bitrate, 4800);
    data("4800 bit/s", 4800);

    /* the answerer's R3 calls for a cleardown (made to: as though it found
     * the line too poor for every rate of R2) */
    start(P_ANSWER, ANS_ALL, ANS_ALL, 0, 0, 0);
    while (now < MS(20000) && !(A.phase == ANS_V32 && A.u.v32.state == V32S_S2)) run(1);
    A.u.v32.r2_rates = 0;
    t = to_data(20000);
    printf("  R3 calling for a cleardown: phase %d, end %d\n", O.phase, O.end);
    CHECK(t < 0); CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_CLEARDOWN);

    /* V.32bis not offered by us: V.8 goes to V.22bis as before */
    start(P_ANSWER, V8_MOD_V22 | V8_MOD_V21, ANS_ALL, 0, 0, 0);
    t = to_data(20000);
    printf("  V.32bis not offered: modulation %d\n", O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V22BIS);
}

static void annex_a(void)
{
    double t;
    printf("pre-V.8 answerers, V.32bis on offer (Annex A)\n");
    ans_ms = 3300;
    start(P_LEGACY_V32, ANS_ALL, 0, 1, 0, V32_RALL);
    t = to_data(20000);
    printf("  ANS 3.3 s, then V.32bis: AA at the answerer from %.2f s; data at %.2f s, %d bit/s, modulation %d, V.8 %d\n",
           t_aa / (double)FS, t, O.bitrate, O.modulation, O.used_v8);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V32BIS); CHECK_EQ_I(O.used_v8, 0);
    CHECK(t_aa > MS(1000)); CHECK(t_aa < MS(ans_ms));           /* A.2.1.3: AA after 1 s of ANS, during it */
    data("data", O.bitrate);

    /* a short answer tone: it ends before AA has begun; AC is answered with AA (A.2.1.1) */
    ans_ms = 900;
    start(P_LEGACY_V32, ANS_ALL, 0, 0, 0, V32_RALL);
    t = to_data(20000);
    printf("  ANS 0.9 s, then V.32bis: AA from %.2f s; data at %.2f s, %d bit/s\n", t_aa / (double)FS, t, O.bitrate);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V32BIS); CHECK_EQ_I(O.bitrate, 14400);
    CHECK(t_aa > MS(ans_ms));

    /* V.22bis answerer: its unscrambled ones while our AA goes out (A.2.1.3) */
    ans_ms = 3300;
    start(P_LEGACY_V22, ANS_ALL, 0, 1, 0, 0);
    t = to_data(20000);
    printf("  ANS 3.3 s, then V.22bis: data at %.2f s, %d bit/s, modulation %d\n", t, O.bitrate, O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V22BIS); CHECK_EQ_I(O.used_v8, 0);
    {
        static const char up[] = "the calling modem says: 0123456789", down[] = "the answering modem says: ZYXWVU";
        uint8_t got[64], agot[64]; int ng = 0, ag = 0, k;
        v14_write(&c_dte, (const uint8_t *)up, (int)strlen(up));
        v14_write(&a_dte, (const uint8_t *)down, (int)strlen(down));
        for (k = 0; k < 200; k++) {
            run(MS(10));
            ng += v14_read(&c_dte, got + ng, (int)sizeof got - 1 - ng);
            ag += v14_read(&a_dte, agot + ag, (int)sizeof agot - 1 - ag);
        }
        printf("    caller got %d of %d octets, answerer %d of %d\n", ng, (int)strlen(down), ag, (int)strlen(up));
        CHECK(ng == (int)strlen(down) && !memcmp(got, down, strlen(down)));
        CHECK(ag == (int)strlen(up) && !memcmp(agot, up, strlen(up)));
    }

    /* ANS lasting no more than 800 ms from the start of AA (A.2.1.3): the
     * unscrambled binary 1 is answered only after Tc (A.2.1.2) */
    ans_ms = 1500;
    start(P_LEGACY_V22, ANS_ALL, 0, 1, 0, 0);
    t = to_data(20000);
    printf("  ANS 1.5 s, then V.22bis: AA at the answerer from %.2f s; V.22bis after Tc, data at %.2f s, modulation %d\n",
           t_aa / (double)FS, t, O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V22BIS);
    CHECK(t > 1.5 + 3.1);
    /* ... and an automode answerer (A.2.2) that missed our AA: USB1 for
     * 3 s, then AC: still V.32bis */
    start(P_LEGACY_AUTO, ANS_ALL, 0, 1, 0, V32_RALL);
    t = to_data(20000);
    printf("  ANS 1.5 s, USB1 3 s, then V.32bis (an automode answerer): data at %.2f s, %d bit/s, modulation %d\n", t, O.bitrate, O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V32BIS);
    ans_ms = 3300;

    /* V.21 answerer: no AC, no unscrambled ones; AA stops, the mark is heard */
    start(P_LEGACY_V21, ANS_ALL, 0, 1, 0, 0);
    t = to_data(20000);
    printf("  ANS 3.3 s, then V.21: data at %.2f s, modulation %d\n", t, O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V21);
    {
        static const char up[] = "the calling modem says: 0123456789";
        int k;
        v14_write(&c_dte, (const uint8_t *)up, (int)strlen(up));
        for (k = 0; k < 300; k++) run(MS(10));
        printf("    answerer got %d of %d octets\n", a_ngot, (int)strlen(up));
        CHECK(a_ngot == (int)strlen(up) && !memcmp(a_got, up, strlen(up)));
    }

    /* V.32bis only, and nobody after the tone */
    start(P_LEGACY_V21, V8_MOD_V32, 0, 0, 0, 0);
    peer_kind = P_LEGACY_V21;
    run(MS(25000));
    printf("  V.32bis only, a V.21 answerer: phase %d, end %d\n", O.phase, O.end);
    CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_NO_CARRIER);
}

int main(void)
{
    via_v8();
    annex_a();
    T_REPORT("orig_v32");
}
