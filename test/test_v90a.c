/* The V.90 analogue modem's sequencer (modem/v90a) against the library's
 * digital modem without V.8 (p2d, then p34d as the answerer starts it), over
 * test/v90link.h's line: the order and timing of Phases 3 and 4 as 9.3.2 and
 * 9.4.2 ask, the echo canceller trained on MD, the DIL measured, data both
 * ways; the time-outs when the digital modem goes away at each stage; a
 * rate renegotiation begun by the digital modem, cleardown from either end.
 * (Retrains need the whole answerer: test_orig_retrain.) */
#include <stdio.h>
#include <inttypes.h>
#include "test.h"
#include "v90link.h"
#include "../modem/clock.h"

#define MS(x) ((int64_t)(x) * 8)

static vl_t L;

static vl_cfg base(void)
{
    vl_cfg c;
    memset(&c, 0, sizeof c);
    c.loop_km = 2; c.noise_dbm0 = -75; c.ppm = 30; c.phase = 0.3; c.echo_db = -10;
    c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.seed = 1;
    return c;
}

/* run until the analogue modem is in data mode (or ended, or t_max) */
static int run_to(vl_t *l, int64_t t_max, v90a_state want_state)
{
    while (l->k < t_max && l->va.state != want_state && l->va.state != V90A_ENDED) vl_tick(l);
    return l->va.state;
}

static void run_for(vl_t *l, int64_t n) { int64_t e = l->k + n; while (l->k < e && l->va.state != V90A_ENDED) vl_tick(l); }

/* milliseconds from samples, and tenths of a dB, printed as integers: the C
 * libraries round decimals of doubles differently */
static int32_t ms(int32_t t) { return t / 8; }
#define DB10(x) (x) / 10, ((x) < 0 ? -(x) : (x)) % 10

int main(void)
{
    vl_cfg c;
    vl_t *l = &L;

    printf("a call, 2 km, +30 ppm, echo -10 dB, noise -75 dBm0 (downstream) / -70 dBm0 (upstream)\n");
    c = base();
    vl_init(l, &c);
    run_to(l, MS(30000), V90A_DATA);
    run_for(l, MS(300));                /* the digital modem's side finishes */
    printf("  state %d, error %d; data at %d ms: down %d, up %d bit/s, SNR %d.%d dB\n", l->va.state, l->va.error,
           (int)ms(l->va.t_data), v90a_rx_rate(&l->va), v90a_tx_rate(&l->va), DB10(v90a_snr_db10(&l->va)));
    printf("  INFO1a %d ms, Sd-bar %d, Jd %d, Jd' %d, DIL ended %d, Ri %d, Ri-bar %d, MP %d, Ed %d\n",
           (int)ms(l->va.t_info1a), (int)ms(l->va.t_sd), (int)ms(l->va.t_jd), (int)ms(l->va.t_jdp), (int)ms(l->va.t_dil_end),
           (int)ms(l->va.t_ri), (int)ms(l->va.t_ribar), (int)ms(l->va.t_mp), (int)ms(l->va.t_ed));
    printf("  canceller after MD: %d.%d dB; DIL: %d per interval and Ucode, robbed %d%d%d%d%d%d, deviation %d.%d %%\n",
           DB10(l->va.erle_db10), l->va.dil_passes, l->va.res.robbed[0], l->va.res.robbed[1], l->va.res.robbed[2],
           l->va.res.robbed[3], l->va.res.robbed[4], l->va.res.robbed[5], DB10(l->va.res.dev_q8 * 1000 / 256));
    printf("  CPt: K %d (%d bit/s), z %d.%d; CP: K %d, z %d.%d; TRN2d frames %d (%d wrong); B1d wrong bits %d\n",
           l->va.cpt_ch.K, l->va.cpt_ch.bps, DB10(l->va.cpt_ch.z_q4 * 10 / 16), l->va.cp_ch.K, DB10(l->va.cp_ch.z_q4 * 10 / 16),
           l->va.trn2d_frames, l->va.trn2d_bad, l->va.b1d_bad);
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(l->p34.state, P34_DATA);
    CHECK(v90a_rx_rate(&l->va) >= 44000);
    CHECK_EQ_I(v90a_rx_rate(&l->va), l->p34.down_rate);
    CHECK_EQ_I(v90a_tx_rate(&l->va), l->p34.up_rate);
    CHECK(v90a_tx_rate(&l->va) >= 24000);
    /* 9.3.2.4: Sd-bar within 1500 ms of Ja (we wait 2000); 9.3.2.7: Jd within 4500 ms of
     * its end; 9.3.2.10: the DIL ended within 5000 ms of S; 9.4.2: B1d
     * within 15 s of INFO1a */
    CHECK(l->va.t_info1a < l->va.t_sd && l->va.t_sd < l->va.t_jd && l->va.t_jd < l->va.t_jdp);
    CHECK(l->va.t_jdp < l->va.t_dil_end && l->va.t_dil_end < l->va.t_ri && l->va.t_ri < l->va.t_ribar);
    CHECK(l->va.t_ribar < l->va.t_mp && l->va.t_mp < l->va.t_ed && l->va.t_ed < l->va.t_data);
    CHECK(l->va.t_dil_end - l->va.t_jdp < MS(5000));
    CHECK(l->va.t_data - l->va.t_info1a < MS(15000));
    CHECK(l->va.erle_db10 > 500);
    CHECK(l->va.dil_passes >= 8);
    CHECK(l->va.trn2d_frames > 1000);
    CHECK_EQ_I(l->va.trn2d_bad, 0);
    CHECK_EQ_I(l->va.b1d_bad, 0);
    /* 8.5.2 allows a CP to name fewer constellations than intervals and
     * share them; a real server froze in data mode on such a CP (reported
     * by an independent softmodem's live calls). Ours name one per interval
     * (six), CPt's too. */
    {
        int s, six = v90_cp_nconst(&l->va.cpt) == 6 && v90_cp_nconst(&l->va.cp) == 6;
        for (s = 0; s < 6; s++) six = six && l->va.cpt.idx[s] == s && l->va.cp.idx[s] == s;
        printf("  constellations in CPt / CP as sent: %d / %d\n", v90_cp_nconst(&l->va.cpt), v90_cp_nconst(&l->va.cp));
        CHECK(six);
    }
    {
        int64_t d0 = l->down.got, u0 = l->up.got;
        run_for(l, MS(4000));
        printf("  4 s of data: down %" PRId64 " bits, %" PRId64 " check failures; up %" PRId64 " bits, %" PRId64 " failures\n",
               l->down.got - d0, l->down.fails, l->up.got - u0, l->up.fails);
        CHECK(l->down.got - d0 > 4 * 40000);
        CHECK(l->up.got - u0 > 4 * 20000);
        CHECK_EQ_I(l->down.fails, 0);
        CHECK_EQ_I(l->up.fails, 0);
    }
    CHECK_EQ_I(l->va.retrains, 0);              /* (retrains: test_orig_retrain, with the whole answerer) */

    printf("a rate renegotiation begun by the digital modem\n");
    {
        int64_t f0 = l->down.fails + l->up.fails, d0;
        CHECK_EQ_I(p34d_renegotiate(&l->p34), 0);
        run_for(l, MS(100));
        CHECK(l->va.state != V90A_DATA);
        run_to(l, l->k + MS(6000), V90A_DATA);
        run_for(l, MS(500));
        printf("  the stream's break: %" PRId64 " check failures (the 23-bit check across the gap)\n", l->down.fails + l->up.fails - f0);
        f0 = l->down.fails + l->up.fails;
        d0 = l->down.got;
        run_for(l, MS(2000));
        printf("  state %d, renegotiations %d, digital %d; then 2 s: %" PRId64 " bits down, %" PRId64 " new failures\n",
               l->va.state, l->va.renegs, l->p34.state, l->down.got - d0, l->down.fails + l->up.fails - f0);
        CHECK_EQ_I(l->va.state, V90A_DATA);
        CHECK_EQ_I(l->va.renegs, 1);
        CHECK_EQ_I(l->p34.state, P34_DATA);
        CHECK(l->down.got - d0 > 2 * 40000);
        CHECK_EQ_I(l->down.fails + l->up.fails - f0, 0);
    }
    printf("cleardown by us\n");
    {
        int64_t t0 = l->k;
        CHECK_EQ_I(v90a_cleardown(&l->va), 0);
        run_for(l, MS(2000));
        printf("  ours %d (error %d) after %d ms; digital %d (error %d)\n", l->va.state, l->va.error, (int)ms((int32_t)(l->k - t0)),
               l->p34.state, l->p34.error);
        CHECK_EQ_I(l->va.error, V90A_ERR_HANGUP);
        CHECK_EQ_I(l->p34.state, P34_FAILED);
        CHECK_EQ_I(l->p34.error, P34_ERR_CLEARDOWN);
    }
    printf("cleardown by the digital modem\n");
    c = base(); c.seed = 2;
    vl_init(l, &c);
    run_to(l, MS(30000), V90A_DATA);
    run_for(l, MS(300));
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(p34d_cleardown(&l->p34), 0);
    {
        int64_t t0 = l->k;
        run_for(l, MS(3000));
        printf("  ours %d (error %d) after %d ms\n", l->va.state, l->va.error, (int)ms((int32_t)(l->k - t0)));
        CHECK_EQ_I(l->va.error, V90A_ERR_CLEARDOWN);
        /* every bit the digital modem sent before its Rd arrived (a frame
         * held back at Rd was dropped, 0.5.0) */
        printf("  downstream: %" PRId64 " bits sent, %" PRId64 " not received\n", l->down.sent, l->down.sent - l->down.got);
        CHECK_EQ_I(l->down.sent - l->down.got, 0);
    }

    printf("our cleardown, answered by a retrain of the digital modem's (R3)\n");
    c = base(); c.seed = 3;
    vl_init(l, &c);
    run_to(l, MS(30000), V90A_DATA);
    run_for(l, MS(300));
    CHECK_EQ_I(l->va.state, V90A_DATA);
    {
        int64_t t0 = l->k, n0;
        int entered_p2 = 0, d_p2 = 0;
        CHECK_EQ_I(v90a_cleardown(&l->va), 0);
        CHECK_EQ_I(p34d_retrain(&l->p34), 0);       /* on the next sample: silence, then Tone B */
        n0 = l->n;
        while (l->k < t0 + MS(3000) && l->va.state != V90A_ENDED) {
            vl_tick(l);
            /* the owner's part of the digital modem's retrain: 70 ms of silence, Phase 2 (9.5.1) */
            if (!d_p2 && l->n - n0 >= MS(70)) {
                p2d_cfg pc = l->p2.cfg;
                p2d_init_retrain(&l->p2, &pc);
                l->dstate = 0; d_p2 = 1;
            }
            if (l->va.state == V90A_RETRAIN || l->va.state == V90A_P2) entered_p2 = 1;
        }
        printf("  ours %d (error %d) after %d ms; Phase 2 entered %d; digital in Phase 2 %d\n", l->va.state, l->va.error,
               (int)ms((int32_t)(l->k - t0)), entered_p2, d_p2);
        CHECK_EQ_I(l->va.error, V90A_ERR_HANGUP);
        CHECK(!entered_p2);
        CHECK(l->k - t0 <= MS(2000));
    }

    printf("a retrain's Phase 2 bounded: %d ms (spec D5)\n", V90A_RT_P2_MS);
    c = base(); c.seed = 4;
    vl_init(l, &c);
    run_to(l, MS(30000), V90A_DATA);
    run_for(l, MS(300));
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(v90a_retrain(&l->va), 0);
    run_to(l, l->k + MS(1000), V90A_P2);
    CHECK_EQ_I(l->va.state, V90A_P2);
    CHECK(l->va.t < 100);                       /* the clock began with Phase 2 */
    /* its clock 1 sample short of the limit; two ticks of our own (the
     * digital modem is no party to this) */
    l->va.t = (int32_t)MS(V90A_RT_P2_MS) - 1;
    (void)v90a_tick(&l->va, 0);
    CHECK_EQ_I(l->va.state, V90A_P2);
    (void)v90a_tick(&l->va, 0);
    printf("  ours %d (error %d)\n", l->va.state, l->va.error);
    CHECK_EQ_I(l->va.state, V90A_ENDED);
    CHECK_EQ_I(l->va.error, V90A_ERR_RETRAINS);

    /* A live server's habit (reported by an independent softmodem's calls):
     * TRN1d for about 4 s, Jd at the last moment 9.3.1.4 allows, and S
     * wanted within 9.3.1.5's 5100 ms of the start of TRN1d. Jd is within
     * 9.3.2.7's 4500 ms of the end of Ja, S follows it at once. Jd in 4100 ms
     * (past 9.3.1.4) is still taken; in 4600 ms, past 9.3.2.7's limit, it is
     * a retrain's (V90A_ERR_JD). */
    printf("a server's long TRN1d, Jd at its end\n");
    {
        static const int TJ[] = { 4000, 4100, 4600 };
        int i;
        for (i = 0; i < 3; i++) {
            int32_t jd_at = -1;
            c = base(); c.seed = (uint32_t)(30 + i); c.trn1d_ms = TJ[i];
            vl_init(l, &c);
            while (l->k < MS(30000) && l->va.state != V90A_DATA && l->va.state != V90A_ENDED) {
                vl_tick(l);
                if (jd_at < 0 && l->p34.state == P34_JD) jd_at = (int32_t)(l->p34.now - l->p34.t_trn1d);
            }
            printf("  TRN1d %d ms: Jd %d ms into it; ours %d (error %d), Jd read %d ms after the end of Ja, Jd' %d ms after that; digital %d\n",
                   TJ[i], jd_at < 0 ? -1 : (int)ms(jd_at), l->va.state, l->va.error,
                   l->va.t_jd != CLK_NONE ? (int)ms(clk_diff(l->va.t_jd, l->va.t_ja_end)) : -1,
                   l->va.t_jdp != CLK_NONE ? (int)ms(clk_diff(l->va.t_jdp, l->va.t_jd)) : -1, l->p34.state);
            if (TJ[i] <= 4100) {
                CHECK(ms(jd_at) >= TJ[i] && ms(jd_at) < TJ[i] + 50);
                CHECK_EQ_I(l->va.state, V90A_DATA);
                CHECK(l->p34.state != P34_FAILED);
                CHECK(clk_diff(l->va.t_jdp, l->va.t_jd) < MS(200));
            } else {
                CHECK_EQ_I(l->va.state, V90A_ENDED);
                CHECK_EQ_I(l->va.error, V90A_ERR_JD);
            }
        }
    }

    printf("time-outs: the digital modem silent from\n");
    {
        static const struct { const char *name; int at; int err; double lo, hi; } T[] = {
            /* at: 0 the start of its Phase 3, 1 Sd-bar heard, 2 Jd', 3 Ri-bar, 4 data */
            { "the start of Phase 3 (no Sd): 2000 ms from Ja", 0, V90A_ERR_SD, 1950, 2200 },
            { "TRN1d learnt (no Jd): 4500 ms from Ja's end", 1, V90A_ERR_JD, 4300, 5500 },
            { "Jd' (no DIL)", 2, V90A_ERR_DIL, 0, 6000 },
            { "Ri-bar (no MP): 15 s from INFO1a", 3, V90A_ERR_PHASE4, 0, 16000 },
            { "data mode: carrier lost", 4, V90A_ERR_CARRIER, 0, 1200 },
        };
        int i;
        for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++) {
            int32_t from = -1;
            uint32_t ref;
            c = base(); c.seed = (uint32_t)(10 + i);
            vl_init(l, &c);
            while (l->k < MS(40000) && l->va.state != V90A_ENDED) {
                vl_tick(l);
                if (from < 0) {
                    int go = T[i].at == 0 ? l->va.state == V90A_P3
                           : T[i].at == 1 ? l->va.rx.state == V90ARX_DD
                           : T[i].at == 2 ? l->va.t_jdp != CLK_NONE
                           : T[i].at == 3 ? l->va.t_ribar != CLK_NONE
                           : l->va.state == V90A_DATA;
                    if (go) { from = (int32_t)l->k; l->dmute = 1; }
                }
            }
            /* the reference each limit runs from */
            ref = T[i].at == 0 ? l->va.t_ja : T[i].at == 1 ? l->va.t_ja_end : T[i].at == 3 ? l->va.t_info1a : (uint32_t)from;
            printf("  %-48s ended %d, error %d, %d ms after %s\n", T[i].name, l->va.state, l->va.error,
                   (int)ms(clk_diff(l->va.now, ref)), T[i].at == 0 ? "Ja" : T[i].at == 1 ? "the end of Ja" : T[i].at == 3 ? "INFO1a" : "the silence");
            CHECK_EQ_I(l->va.state, V90A_ENDED);
            CHECK_EQ_I(l->va.error, T[i].err);
            CHECK(ms(clk_diff(l->va.now, ref)) >= T[i].lo && ms(clk_diff(l->va.now, ref)) <= T[i].hi);
        }
    }
    T_REPORT("v90a");
}
