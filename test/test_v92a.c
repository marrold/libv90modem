/* The V.92 analogue modem's sequencer (modem/v90a with V.92 on) against the
 * library's digital modem without V.8 (p2d offering V.92, then v92/v92_p34d
 * as the answerer starts it), over test/v90link.h's line with the upstream
 * through the loop and codec filters: the order and timing of Phases 3 and
 * 4 as 9.5.2 and 9.6.2 ask, our PCM upstream through the precoder and
 * prefilter the digital modem designed (CPd), the DIL measured, data both
 * ways; a rate renegotiation begun by the digital modem; cleardown from
 * either end; the time-outs when the digital modem goes away at each stage;
 * and the V.90 fallback when the digital modem does not offer V.92. */
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
    c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.up_line = 1; c.seed = 1;
    c.v92 = 1; c.caller_v92 = 1;
    c.d_retrain = 1;                    /* a retrain restarts the digital modem, as answer.c does */
    return c;
}

static int run_to(vl_t *l, int64_t t_max, v90a_state want_state)
{
    while (l->k < t_max && l->va.state != want_state && l->va.state != V90A_ENDED) vl_tick(l);
    return l->va.state;
}

static void run_for(vl_t *l, int64_t n) { int64_t e = l->k + n; while (l->k < e && l->va.state != V90A_ENDED) vl_tick(l); }

static int32_t ms(int32_t t) { return t / 8; }
#define DB10(x) (x) / 10, ((x) < 0 ? -(x) : (x)) % 10

int main(void)
{
    vl_cfg c;
    vl_t *l = &L;

    printf("a call, 2 km, +30 ppm, echo -10 dB, noise -75 dBm0 (downstream) / -70 dBm0 (upstream)\n");
    c = base();
    vl_init(l, &c);
    run_to(l, MS(40000), V90A_DATA);
    run_for(l, MS(300));
    printf("  state %d, error %d, PCM upstream %d; data at %d ms: down %d, up %d bit/s, SNR %d.%d dB; digital %d (error %d)\n",
           l->va.state, l->va.error, l->va.pcm, (int)ms(l->va.t_data), v90a_rx_rate(&l->va), v90a_tx_rate(&l->va),
           DB10(v90a_snr_db10(&l->va)), l->v92d.state, l->v92d.error);
    printf("  INFO1a %d ms, Sd-bar %d, Jd %d, Su %d, Jp %d, Jp' %d, DIL ended %d, CPt %d, Ri %d, Ri-bar %d, SUVd %d, CPd %d, Ed %d\n",
           (int)ms(l->va.t_info1a), (int)ms(l->va.t_sd), (int)ms(l->va.t_jd), (int)ms(l->va.t_su), (int)ms(l->va.t_jp),
           (int)ms(l->va.t_jdp), (int)ms(l->va.t_dil_end), (int)ms(l->va.t_cpt), (int)ms(l->va.t_ri), (int)ms(l->va.t_ribar),
           (int)ms(l->va.t_suvd), (int)ms(l->va.t_mp), (int)ms(l->va.t_ed));
    printf("  canceller after TRN1u: %d.%d dB; DIL: %d per interval and Ucode; Jp: %d/65536 of a symbol\n",
           DB10(l->va.erle_db10), l->va.dil_passes, l->va.jp.frac);
    printf("  our clock against the network's: %d.%02d ppm from Phase 2 (L2), %d.%02d ppm from Sd and after (line: +30)\n",
           l->va.p2.clk_ppm_q8 / 256, (l->va.p2.clk_ppm_q8 < 0 ? -l->va.p2.clk_ppm_q8 : l->va.p2.clk_ppm_q8) % 256 * 100 / 256,
           v90arx_ppm_q8(&l->va.rx) / 256, (v90arx_ppm_q8(&l->va.rx) < 0 ? -v90arx_ppm_q8(&l->va.rx) : v90arx_ppm_q8(&l->va.rx)) % 256 * 100 / 256);
    printf("  CPd: K %d, trellis %d, LZ1 %d LP1 %d LZ2 %d LP2 %d, 4G %d; B1d wrong bits %d; digital B1u wrong %d\n",
           l->va.cpd.map.K, l->va.cpd.map.trellis, l->va.cpd.filt.lz1, l->va.cpd.filt.lp1, l->va.cpd.filt.lz2,
           l->va.cpd.filt.lp2, l->va.cpd.filt.gain4, l->va.b1d_bad, (int)l->v92d.b1_bad);
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(l->va.pcm, 1);
    CHECK_EQ_I(l->dstate, 3);
    CHECK_EQ_I(l->v92d.state, V92D_DATA);
    CHECK(v90a_rx_rate(&l->va) >= 44000);
    CHECK_EQ_I(v90a_rx_rate(&l->va), l->v92d.down_rate);
    CHECK_EQ_I(v90a_tx_rate(&l->va), l->v92d.up_rate);
    CHECK(v90a_tx_rate(&l->va) >= 24000);
    /* 9.5.2.2.1: Sd-bar within 1500 ms of Ja (we wait 2000; our digital modem meets 1500); 9.5.2.2.2: Jd within 4500 ms
     * of its end; 9.5.2.1.11: CPt within 5000 ms of the second Su-bar;
     * 9.6.2.2.1: B1d within 20 s of INFO1a */
    CHECK(l->va.t_info1a < l->va.t_sd && l->va.t_sd < l->va.t_jd && l->va.t_jd <= l->va.t_su && l->va.t_su < l->va.t_jp);
    CHECK(l->va.t_jp < l->va.t_jdp && l->va.t_jdp < l->va.t_dil_end && l->va.t_dil_end <= l->va.t_cpt && l->va.t_cpt < l->va.t_ri);
    CHECK(l->va.t_ri < l->va.t_ribar && l->va.t_ribar < l->va.t_suvd && l->va.t_suvd < l->va.t_mp && l->va.t_mp < l->va.t_ed);
    CHECK(l->va.t_ed < l->va.t_data);
    CHECK(l->va.t_sd - l->va.t_ja < MS(1500));
    CHECK(l->va.t_jd - l->va.t_ja_end < MS(4500));
    CHECK(l->va.t_cpt - l->va.t_jp < MS(5000));
    CHECK(l->va.t_data - l->va.t_info1a < MS(20000));
    CHECK(l->va.erle_db10 > 500);
    CHECK(l->va.dil_passes >= 8);
    /* the clock is known before the first TRN1u: from the digital modem's L2 */
    CHECK_NEAR(l->va.p2.clk_ppm_q8 / 256.0, 30.0, 2.0);
    CHECK_EQ_I(l->va.b1d_bad, 0);
    CHECK_EQ_I(l->v92d.b1_bad, 0);
    {
        int64_t d0 = l->down.got, u0 = l->up.got;
        run_for(l, MS(4000));
        printf("  4 s of data: down %" PRId64 " bits, %" PRId64 " check failures; up %" PRId64 " bits, %" PRId64 " failures\n",
               l->down.got - d0, l->down.fails, l->up.got - u0, l->up.fails);
        CHECK(l->down.got - d0 > 4 * 40000);
        CHECK(l->up.got - u0 > 4 * 24000);
        CHECK_EQ_I(l->down.fails, 0);
        CHECK_EQ_I(l->up.fails, 0);
    }
    CHECK_EQ_I(l->va.retrains, 0);              /* (retrains: test_orig_retrain, with the whole answerer) */

    printf("a rate renegotiation begun by the digital modem\n");
    {
        int64_t f0 = l->down.fails + l->up.fails, d0, u0, c0 = l->clips;
        CHECK_EQ_I(v92d_renegotiate(&l->v92d), 0);
        run_for(l, MS(100));
        CHECK(l->va.state != V90A_DATA);
        run_to(l, l->k + MS(12000), V90A_DATA);
        run_for(l, MS(500));
        printf("  the stream's break: %" PRId64 " check failures (the 23-bit check across the gap)\n", l->down.fails + l->up.fails - f0);
        f0 = l->down.fails + l->up.fails;
        d0 = l->down.got; u0 = l->up.got;
        run_for(l, MS(2000));
        if (l->va.retrains > 0) {
            CHECK(l->clips > c0);                   /* one after the renegotiation began */
            /* one of our A/D samples at full scale (our PCM upstream's peak
             * in this line's -10 dB echo) loses the receiver (T21.9,
             * docs/limits.md): the retrain recovers it, then the 2 s again */
            printf("  %d A/D samples at full scale since the renegotiation began: one cost a retrain (T21.9)\n", (int)(l->clips - c0));
            run_to(l, l->k + MS(15000), V90A_DATA);
            run_for(l, MS(500));
            f0 = l->down.fails + l->up.fails;
            d0 = l->down.got; u0 = l->up.got;
            run_for(l, MS(2000));
        }
        printf("  state %d, renegotiations %d, retrains %d, digital %d, up %d; then 2 s: %" PRId64 " bits down, %" PRId64 " up, %" PRId64 " new failures\n",
               l->va.state, l->va.renegs, l->va.retrains, l->v92d.state, v90a_tx_rate(&l->va), l->down.got - d0, l->up.got - u0,
               l->down.fails + l->up.fails - f0);
        CHECK(l->va.retrains <= 1);
        CHECK_EQ_I(l->va.state, V90A_DATA);
        CHECK_EQ_I(l->va.renegs, 1);
        CHECK_EQ_I(l->v92d.state, V92D_DATA);
        CHECK(l->down.got - d0 > 2 * 40000);
        CHECK(l->up.got - u0 > 2 * 24000);
        CHECK_EQ_I(l->down.fails + l->up.fails - f0, 0);
    }
    printf("cleardown by us\n");
    {
        int64_t t0 = l->k;
        CHECK_EQ_I(v90a_cleardown(&l->va), 0);
        run_for(l, MS(4000));
        printf("  ours %d (error %d) after %d ms; digital %d (error %d)\n", l->va.state, l->va.error, (int)ms((int32_t)(l->k - t0)),
               l->v92d.state, l->v92d.error);
        CHECK_EQ_I(l->va.error, V90A_ERR_HANGUP);
        CHECK_EQ_I(l->v92d.state, V92D_FAILED);
        CHECK_EQ_I(l->v92d.error, V92D_ERR_CLEARDOWN);
    }
    printf("cleardown by the digital modem\n");
    c = base(); c.seed = 2;
    vl_init(l, &c);
    run_to(l, MS(40000), V90A_DATA);
    run_for(l, MS(300));
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(v92d_cleardown(&l->v92d), 0);
    {
        int64_t t0 = l->k, d_end = -1;
        run_for(l, MS(12000));
        printf("  ours %d (error %d) after %d ms\n", l->va.state, l->va.error, (int)ms((int32_t)(l->k - t0)));
        CHECK_EQ_I(l->va.error, V90A_ERR_CLEARDOWN);
        /* 9.6.2.1.2: we sent SUVu / CPu with the acknowledge bit after the
         * CPd with drn = 0, so the digital modem's cleardown is over at
         * once (9.6.1.1.4's point), not at its time limit */
        CHECK_EQ_I(l->va.ack_sent, 1);
        while (l->k < t0 + MS(5000) && d_end < 0) { vl_tick(l); if (l->v92d.state == V92D_FAILED) d_end = l->k - t0; }
        printf("  the digital modem: state %d (error %d) after %d ms\n", l->v92d.state, l->v92d.error, (int)ms((int32_t)d_end));
        CHECK_EQ_I(l->v92d.state, V92D_FAILED);
        CHECK_EQ_I(l->v92d.error, V92D_ERR_HANGUP);
        CHECK(d_end >= 0 && d_end < MS(1000));
        /* every bit the digital modem sent before its Rd arrived (a frame
         * held back at Rd was dropped, 0.5.0) */
        printf("  downstream: %" PRId64 " bits sent, %" PRId64 " not received\n", l->down.sent, l->down.sent - l->down.got);
        CHECK_EQ_I(l->down.sent - l->down.got, 0);
    }

    printf("cleardown by the digital modem, whose acknowledge never reaches us\n");
    c = base(); c.seed = 2;
    vl_init(l, &c);
    run_to(l, MS(40000), V90A_DATA);
    run_for(l, MS(300));
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(v92d_cleardown(&l->v92d), 0);
    {
        /* no acknowledge from the digital modem reaches us (its SUVd' and
         * CPd' are read as without the bit): we acknowledge for 100 ms + a
         * round trip (and a sequence or so), then end as the cleardown it
         * is */
        int64_t t0 = l->k, t_cpd = -1;
        while (l->k < t0 + MS(12000) && l->va.state != V90A_ENDED) {
            vl_tick(l);
            l->va.got_ack = 0;
            if (t_cpd < 0 && l->va.got_cpd && l->va.cpd.drn == 0) t_cpd = l->k;
        }
        printf("  ours %d (error %d) %d ms after the CPd\n", l->va.state, l->va.error, (int)ms((int32_t)(l->k - t_cpd)));
        CHECK_EQ_I(l->va.error, V90A_ERR_CLEARDOWN);
        CHECK(t_cpd >= 0);
        CHECK(l->k - t_cpd > MS(100) && l->k - t_cpd < MS(100) + l->va.p2.rtd + MS(100));
    }

    printf("our cleardown (CPu), answered by a retrain of the digital modem's (R3)\n");
    c = base(); c.seed = 4;
    vl_init(l, &c);
    run_to(l, MS(40000), V90A_DATA);
    run_for(l, MS(300));
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(l->va.pcm, 1);
    {
        int64_t t0 = l->k, n0;
        int entered_p2 = 0, d_p2 = 0;
        CHECK_EQ_I(v90a_cleardown(&l->va), 0);
        CHECK_EQ_I(v92d_retrain(&l->v92d), 0);      /* on the next sample: silence, then Tone B */
        n0 = l->n;
        while (l->k < t0 + MS(3000) && l->va.state != V90A_ENDED) {
            vl_tick(l);
            /* the owner's part of the digital modem's retrain: 70 ms of silence, Phase 2 (9.7.1) */
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

    printf("the digital modem without V.92: V.90\n");
    c = base(); c.v92 = 0; c.seed = 3;
    vl_init(l, &c);
    run_to(l, MS(40000), V90A_DATA);
    run_for(l, MS(300));
    printf("  state %d, PCM upstream %d, down %d, up %d bit/s; digital p34d %d\n", l->va.state, l->va.pcm,
           v90a_rx_rate(&l->va), v90a_tx_rate(&l->va), l->p34.state);
    CHECK_EQ_I(l->va.state, V90A_DATA);
    CHECK_EQ_I(l->va.pcm, 0);
    CHECK_EQ_I(l->dstate, 1);
    CHECK_EQ_I(l->p34.state, P34_DATA);
    CHECK(v90a_tx_rate(&l->va) <= 33600);
    {
        int64_t f0 = l->down.fails + l->up.fails, d0 = l->down.got;
        run_for(l, MS(2000));
        CHECK(l->down.got - d0 > 2 * 40000);
        CHECK_EQ_I(l->down.fails + l->up.fails - f0, 0);
    }

    /* A click: 50 units added to one A/D sample in data mode. The
     * decision-feedback equaliser carries this line's low-frequency tail in
     * its feedback (sum |b| about 4), so the few wrong decisions the click
     * makes keep making more and the receiver is lost (T21.9; docs/limits.md).
     * What recovers it is the retrain its watch begins, with the digital
     * modem restarted as answer.c restarts it: back in data mode within 15
     * s (about 11), the data correct after. */
    printf("a click in data mode (50 units on one sample): recovered by a retrain\n");
    c = base(); c.seed = 5;
    vl_init(l, &c);
    run_to(l, MS(40000), V90A_DATA);
    run_for(l, MS(1000));
    CHECK_EQ_I(l->va.state, V90A_DATA);
    {
        int64_t t0 = l->k + MS(10), f0, d0, u0, back = -1;
        l->cfg.imp_ms = (int)(t0 / 8); l->cfg.imp_amp = 50;
        while (l->k < t0 + MS(15000) && l->va.state != V90A_ENDED) {
            vl_tick(l);
            if (back < 0 && l->k > t0 && l->va.retrains > 0 && l->va.state == V90A_DATA) back = l->k;
            if (back >= 0 && l->k >= back + MS(500)) break;
        }
        f0 = l->down.fails + l->up.fails; d0 = l->down.got; u0 = l->up.got;
        run_for(l, MS(2000));
        printf("  ours %d (error %d), retrains %d, digital's %d; data again %d ms after the click, PCM upstream %d;"
               " then 2 s: %" PRId64 " bits down, %" PRId64 " up, %" PRId64 " failures\n",
               l->va.state, l->va.error, l->va.retrains, l->d_retrains, back < 0 ? -1 : (int)ms((int32_t)(back - t0)), l->va.pcm,
               l->down.got - d0, l->up.got - u0, l->down.fails + l->up.fails - f0);
        CHECK_EQ_I(l->va.state, V90A_DATA);
        CHECK(l->va.retrains >= 1);
        /* (V.92 retrains: 9 to 11 s in test_orig_retrain, and they move by
         * a second with the start-up's timing: 10.06 s at T21.9, 10.97 s
         * after Task 21b. 15 s is about 10 s with room, and still bounds it) */
        CHECK(back >= 0 && back - t0 < MS(15000));
        CHECK(l->down.got - d0 > 2 * 40000);
        CHECK(l->up.got - u0 > 2 * 24000);
        CHECK_EQ_I(l->down.fails + l->up.fails - f0, 0);
    }

    printf("time-outs: the digital modem silent from\n");
    {
        static const struct { const char *name; int at; int err; double lo, hi; } T[] = {
            /* at: 0 the start of its Phase 3, 1 Sd-bar heard, 2 our Su, 3 Jp', 4 Ri-bar, 5 data */
            { "the start of Phase 3 (no Sd): 2000 ms from Ja", 0, V90A_ERR_SD, 1950, 2200 },
            { "TRN1d learnt (no Jd): 4500 ms from Ja's end", 1, V90A_ERR_JD, 4300, 5500 },
            { "our Su (no Jp)", 2, V90A_ERR_JP, 0, 6000 },
            { "Jp' (no DIL)", 3, V90A_ERR_DIL, 0, 6000 },
            { "Ri-bar (no CPd): 20 s from INFO1a", 4, V90A_ERR_PHASE4, 0, 21000 },
            { "data mode: carrier lost", 5, V90A_ERR_CARRIER, 0, 1200 },
        };
        int i;
        for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++) {
            int32_t from = -1;
            uint32_t ref;
            c = base(); c.seed = (uint32_t)(10 + i);
            vl_init(l, &c);
            while (l->k < MS(50000) && l->va.state != V90A_ENDED) {
                vl_tick(l);
                if (from < 0) {
                    int go = T[i].at == 0 ? l->va.state == V90A_P3
                           : T[i].at == 1 ? l->va.rx.state == V90ARX_DD
                           : T[i].at == 2 ? l->va.t_su != CLK_NONE
                           : T[i].at == 3 ? l->va.t_jdp != CLK_NONE
                           : T[i].at == 4 ? l->va.t_ribar != CLK_NONE
                           : l->va.state == V90A_DATA;
                    if (go) { from = (int32_t)l->k; l->dmute = 1; }
                }
            }
            ref = T[i].at == 0 ? l->va.t_ja : T[i].at == 1 ? l->va.t_ja_end : T[i].at == 4 ? l->va.t_info1a : (uint32_t)from;
            printf("  %-48s ended %d, error %d, %d ms after %s\n", T[i].name, l->va.state, l->va.error,
                   (int)ms(clk_diff(l->va.now, ref)), T[i].at == 0 ? "Ja" : T[i].at == 1 ? "the end of Ja" : T[i].at == 4 ? "INFO1a" : "the silence");
            CHECK_EQ_I(l->va.state, V90A_ENDED);
            CHECK_EQ_I(l->va.error, T[i].err);
            CHECK(ms(clk_diff(l->va.now, ref)) >= T[i].lo && ms(clk_diff(l->va.now, ref)) <= T[i].hi);
        }
    }
    T_REPORT("v92a");
}
