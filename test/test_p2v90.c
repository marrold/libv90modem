/* V.90 Phase 2, analogue modem (modem/p2call in its V.90 mode, 9.2.2)
 * against the library's digital modem (modem/p2d, 9.2.1) over a delayed,
 * attenuated, noisy, echoing path: the INFO exchange, INFO1a of Table 10 as
 * the digital modem reads it, the round trip as both ends measure it (each
 * end's figure depends on the other's 40 ms turn-round), the upstream
 * symbol rate from INFO1d, the recovery procedures, and the time-outs. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/p2call.h"
#include "../modem/p2d.h"

#define MAXD 4096

typedef struct {
    int d_ad, d_da;             /* one-way delays: analogue -> digital, digital -> analogue */
    double g_ad, g_da;          /* gains */
    double echo;                /* each end's own signal into its receiver */
    double noise;               /* RMS at each receiver */
    int io;                     /* the analogue modem's own sample path, out and back (samples) */
    int mute_ad_to;             /* analogue -> digital dead until this sample */
    int digital_dead_from;      /* the digital modem stops (silent) from this sample, 0 = never */
    int up_sr_mask;             /* the digital modem's INFO1d offer */
    const info0_t *retrain;     /* the analogue modem in a retrain's Phase 2, this the first INFO0d */
    int d_reinit;               /* the digital modem starts over each time it leaves P2D_INFO0 */
    int drop_info1d;            /* this many of the digital modem's INFO1d lost; after each the
                                   analogue modem starts Phase 2 again, INFO0a repeated (ack 0)
                                   until INFO0d is in: the caller of an ISP call (cc260f5a) */
    int restart_in;             /* != 0: the analogue modem starts over (as above) once, 20 ms
                                   into this state of the digital modem's */
    int mute_ad_in;             /* != 0: analogue -> digital dead while the digital modem is in
                                   this state, until it has gone back to INFO0 once */
    int v92;                    /* both ends V.92, PCM upstream allowed (Table 17 / 18/V.92) */
    const info1a_t *info1a;     /* the analogue modem sends this INFO1a instead of its own */
    int seconds;
} scen_t;

typedef struct { p2d_t d; p2call_t a; int a_done, d_done; int l12_samples; int d_inits; int tone_at; int a_inits; } run_t;

/* noise without the C library's maths (its log and sqrt differ in the last
 * bit between libraries, and so would this test's output): the sum of four
 * uniform integers, about Gaussian, unit RMS */
static uint32_t nz;
static double noise1(void)
{
    int32_t a = 0;
    int i;
    for (i = 0; i < 4; i++) { nz = nz * 1664525u + 1013904223u; a += (int32_t)(nz >> 16) - 32768; }
    return a / 37837.2;         /* 32768 * sqrt(4 / 3) */
}

static int16_t clip16(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : floor(v + 0.5)); }

static void run(const scen_t *s, run_t *r)
{
    static double ba[MAXD], bd[MAXD];
    static int16_t aio[MAXD];
    p2d_cfg dc;
    p2call_cfg ac;
    int n, dropping = 0, drops = s->drop_info1d;
    memset(ba, 0, sizeof ba); memset(bd, 0, sizeof bd); memset(aio, 0, sizeof aio);
    memset(r, 0, sizeof *r);
    nz = 3;
    memset(&dc, 0, sizeof dc);
    dc.tx_dbm0 = -12; dc.max_tx_code = 23; dc.up_sr_mask = s->up_sr_mask ? s->up_sr_mask : (1 << 3) | (1 << 4);
    p2d_init(&r->d, &dc);
    if (s->v92) p2d_offer_v92(&r->d, 1);
    memset(&ac, 0, sizeof ac);
    ac.level = 4000; ac.io_delay = s->io; ac.uinfo = 90; ac.md_len = 15; ac.v92 = s->v92;
    if (s->retrain) p2call_init_v90_retrain(&r->a, &ac, s->retrain);
    else p2call_init_v90(&r->a, &ac);
    r->a_done = r->d_done = -1;
    for (n = 0; n < s->seconds * 8000; n++) {
        double xa, xd;
        int16_t ta, td, rxa;
        /* what reaches each receiver now */
        xd = s->g_ad * ba[(n - s->d_ad + MAXD) % MAXD] + s->echo * bd[(n - 1 + MAXD) % MAXD] + s->noise * noise1();
        xa = s->g_da * bd[(n - s->d_da + MAXD) % MAXD] + s->echo * ba[(n - 1 + MAXD) % MAXD] + s->noise * noise1();
        if (n < s->mute_ad_to || (s->mute_ad_in && !r->d.n_restart && r->d.state == (p2d_state)s->mute_ad_in))
            xd = s->echo * bd[(n - 1 + MAXD) % MAXD];
        /* the analogue modem's own path: io / 2 out, io / 2 back */
        aio[n % MAXD] = clip16(xa);
        rxa = aio[(n - s->io / 2 + MAXD) % MAXD];
        td = (s->digital_dead_from && n >= s->digital_dead_from) ? 0 : p2d_tick(&r->d, clip16(xd));
        if (s->d_reinit && r->d.state != P2D_INFO0) { p2d_init(&r->d, &dc); r->d_inits++; }
        if (drops && r->d.state == P2D_INFO1D) dropping = 1;
        if (dropping) td = 0;
        if (dropping && r->d.state != P2D_INFO1D && r->d.t >= 8 * 100) {
            /* 100 ms after the lost INFO1d: the caller starts over */
            dropping = 0; drops--;
            p2call_init_v90(&r->a, &ac);
            r->a_inits++;
        }
        if (s->restart_in && !r->a_inits && r->d.state == (p2d_state)s->restart_in && r->d.t == 8 * 20) {
            p2call_init_v90(&r->a, &ac);
            r->a_inits++;
        }
        /* the ISP's caller repeated INFO0a (ack 0) every 82 ms until INFO0d came;
         * p2call by itself sends one, then Tone A, as it hears only silence */
        if (r->a_inits && r->a.state == P2L_A_INFO0 && !r->a.got0d) r->a.resend = 1;
        {
            int was = r->a.state;
            ta = p2call_tick(&r->a, rxa);
            if (s->info1a && was != P2L_A_INFO1A && r->a.state == P2L_A_INFO1A) {
                /* in the tick that queued p2call's own INFO1a: ours instead, the same length */
                uint8_t bits[INFO_MAX_BITS];
                int nb = info1a_build(s->info1a, bits);
                dpsktx_send(&r->a.tx, bits, nb);
            }
        }
        bd[n % MAXD] = td;
        ba[(n + s->io - s->io / 2) % MAXD] = ta;         /* on the line io - io/2 samples later */
        ba[(n + 1 + s->io - s->io / 2) % MAXD] = 0;
        if (r->a.probing) r->l12_samples++;
        if (r->a.state == P2L_A_WAIT_INFO1D && !r->tone_at) r->tone_at = n;     /* our Tone A (9.2.2.1.8) */
        if (r->a_done < 0 && (r->a.state == P2L_DONE || r->a.state == P2L_FAILED)) r->a_done = n;
        if (r->d_done < 0 && (r->d.state == P2D_DONE || r->d.state == P2D_FAILED)) r->d_done = n;
        if (r->a_done >= 0 && (r->d_done >= 0 || s->d_reinit)) break;
    }
}

static scen_t base(void)
{
    scen_t s;
    memset(&s, 0, sizeof s);
    s.d_ad = 80; s.d_da = 80; s.g_ad = 0.5; s.g_da = 0.5; s.echo = 0.1; s.noise = 2; s.seconds = 12;
    return s;
}

static void report(const char *name, const scen_t *s, const run_t *r)
{
    printf("  %-44s analogue %d (err %d) at %d ms, digital %d at %d ms; rtd %d / %d (line %d); up %d, INFO1a: v90 %d, UINFO %d, MD %d, up_sr %d\n",
           name, r->a.state, r->a.error, r->a_done / 8, r->d.state, r->d_done / 8, r->a.rtd, r->d.rtd,
           s->d_ad + s->d_da, r->a.sym_rate, r->d.info1a.v90, r->d.info1a.uinfo, r->d.info1a.md_len, r->d.info1a.up_sr);
}

static void check_ok(const scen_t *s, const run_t *r)
{
    CHECK_EQ_I(r->a.state, P2L_DONE);
    CHECK_EQ_I(r->d.state, P2D_DONE);
    CHECK_EQ_I(r->d.info1a.v90, 1);
    CHECK_EQ_I(r->d.info1a.uinfo, 90);
    CHECK_EQ_I(r->d.info1a.md_len, 15);
    CHECK_EQ_I(r->d.info1a.up_sr, r->a.sym_idx);
    CHECK_NEAR(r->a.rtd, s->d_ad + s->d_da, 3);
    CHECK_NEAR(r->d.rtd, s->d_ad + s->d_da, 3);
    /* what INFO1d asked of our transmitter at that rate */
    CHECK_EQ_I(r->a.tx_hi_carrier, r->d.info1d.sr[r->a.sym_idx].hi_carrier);
    CHECK_EQ_I(r->a.tx_preemph, r->d.info1d.sr[r->a.sym_idx].preemph);
    CHECK_EQ_I(r->a.tx_pwr_red, r->d.info1d.min_pwr_red);
    CHECK(r->d.info1d.sr[r->a.sym_idx].max_rate > 0);
}

int main(void)
{
    scen_t s;
    run_t r;

    printf("V.90 Phase 2: the analogue modem (p2call) against the digital modem (p2d)\n");
    s = base();
    run(&s, &r);
    report("10 ms each way, echo, noise", &s, &r);
    check_ok(&s, &r);
    CHECK_EQ_I(r.a.sym_rate, 3200);
    /* L1 160 ms and L2 until Tone B: the digital modem listens to 460 ms of L2 */
    printf("    our probing lasted %d ms\n", r.l12_samples / 8);
    CHECK(r.l12_samples > 8 * 500 && r.l12_samples < 8 * 800);
    /* 9.2.2.1.8 lets us read the digital modem's L2 for up to 500 ms; we
     * read it for no more than 300 ms from its start (it began 10 ms before
     * probe_from), so that a far end waiting 600 ms plus a round trip from
     * the start of its L2 hears our Tone A with time to spare */
    printf("    our Tone A %d ms after the start of the digital modem's L2\n", (r.tone_at - (r.a.probe_from - 80)) / 8);
    CHECK(r.tone_at - (r.a.probe_from - 80) <= 8 * 300);

    s = base(); s.d_ad = 300; s.d_da = 250; s.g_ad = 0.2; s.g_da = 0.15; s.echo = 0.3; s.noise = 10;
    run(&s, &r);
    report("69 ms round trip, 14 / 16 dB loss, strong echo", &s, &r);
    check_ok(&s, &r);

    s = base(); s.io = 96;
    run(&s, &r);
    report("our own sample path 12 ms (io_delay)", &s, &r);
    check_ok(&s, &r);

    s = base(); s.up_sr_mask = 1 << 3;
    run(&s, &r);
    report("the digital modem offers 3000 only", &s, &r);
    check_ok(&s, &r);
    CHECK_EQ_I(r.a.sym_rate, 3000);

    printf("recovery\n");
    s = base(); s.mute_ad_to = 8 * 400;
    run(&s, &r);
    report("our first INFO0a lost (9.2.2.2.1)", &s, &r);
    check_ok(&s, &r);
    CHECK(r.a.n_info0c >= 2);

    s = base(); s.drop_info1d = 1; s.seconds = 30;
    run(&s, &r);
    report("INFO1d lost, the caller back at INFO0a (9.2.1.2.1)", &s, &r);
    printf("    caller restarted %d times, digital back to INFO0 %d times, INFO0d sent %d\n",
           r.a_inits, r.d.n_restart, r.d.n_info0d);
    check_ok(&s, &r);
    CHECK_EQ_I(r.a_inits, 1);
    CHECK_EQ_I(r.d.n_restart, 1);
    CHECK_EQ_I(r.a.info0d.ack, 1);      /* the caller heard our INFO0d with bit 28 set */

    s = base(); s.restart_in = P2D_WAIT_REV2;
    run(&s, &r);
    report("the caller back at INFO0a in WAIT_REV2 (9.2.1.2.1)", &s, &r);
    check_ok(&s, &r);
    CHECK_EQ_I(r.a_inits, 1);
    CHECK_EQ_I(r.d.n_restart, 1);

    /* the restart unheard in WAIT_REV2: its 2 s run out, and the INFO0a
     * still repeated reaches us in RECOVER (9.2.1.2.3) */
    s = base(); s.mute_ad_in = P2D_WAIT_REV2; s.restart_in = P2D_WAIT_REV2;
    run(&s, &r);
    report("the caller back at INFO0a in RECOVER (9.2.1.2.1)", &s, &r);
    check_ok(&s, &r);
    CHECK_EQ_I(r.a_inits, 1);
    CHECK_EQ_I(r.d.n_restart, 1);
    CHECK(r.d.n_recover >= 1);

    s = base(); s.drop_info1d = 1000; s.seconds = 40;
    run(&s, &r);
    printf("  every INFO1d lost: digital %d, error %d at %d ms, caller restarted %d times, digital back to INFO0 %d times\n",
           r.d.state, r.d.error, r.d_done / 8, r.a_inits, r.d.n_restart);
    CHECK_EQ_I(r.d.n_restart, P2D_MAX_RESTART);
    CHECK_EQ_I(r.d.state, P2D_FAILED);
    CHECK_EQ_I(r.d.error, P2D_ERR_INFO1A);
    CHECK(r.d_done > 0 && r.d_done < 8 * P2D_EXCHANGE_MS);

    printf("V.92 INFO1a whose first 33 payload bits pass INFO0a's CRC\n");
    {
        /* Table 18/V.92, one of the 16 parameter sets that do: INFO1a must win */
        info1a_t f;
        memset(&f, 0, sizeof f);
        f.v90 = 1; f.pcm = 1; f.up_sr = 6; f.down_sr = 6;
        f.sections = 2; f.ltot_code = 2; f.lmax_code = 2; f.md_len = 22; f.uinfo = 88;
        s = base(); s.v92 = 1; s.info1a = &f;
        run(&s, &r);
        printf("  sections 2, Ltot 2, Lmax 2, MD 22, UINFO 88: digital %d (err %d, mode %d) at %d ms, back to INFO0 %d times;"
               " INFO1a: pcm %d sections %d ltot %d lmax %d md %d uinfo %d\n",
               r.d.state, r.d.error, r.d.mode, r.d_done / 8, r.d.n_restart, r.d.info1a.pcm, r.d.info1a.sections,
               r.d.info1a.ltot_code, r.d.info1a.lmax_code, r.d.info1a.md_len, r.d.info1a.uinfo);
        CHECK_EQ_I(r.d.state, P2D_DONE);
        CHECK_EQ_I(r.d.mode, P2D_MODE_V92);
        CHECK_EQ_I(r.d.n_restart, 0);
        CHECK_EQ_I(r.d.info1a.sections, 2);
        CHECK_EQ_I(r.d.info1a.ltot_code, 2);
        CHECK_EQ_I(r.d.info1a.lmax_code, 2);
        CHECK_EQ_I(r.d.info1a.md_len, 22);
        CHECK_EQ_I(r.d.info1a.uinfo, 88);

        /* at low SNR (noise 700, about 9 dB) a hunter one symbol phase off
         * may see the first 41 bits clean and the rest not: its INFO0a must
         * still wait for the hunter that has the whole INFO1a */
        s = base(); s.v92 = 1; s.info1a = &f; s.noise = 700; s.d_ad = 100;
        run(&s, &r);
        printf("  the same at noise 700, 100 samples a -> d: digital %d (err %d, mode %d) at %d ms, back to INFO0 %d times\n",
               r.d.state, r.d.error, r.d.mode, r.d_done / 8, r.d.n_restart);
        CHECK_EQ_I(r.d.state, P2D_DONE);
        CHECK_EQ_I(r.d.mode, P2D_MODE_V92);
        CHECK_EQ_I(r.d.n_restart, 0);
    }

    printf("time-outs\n");
    s = base(); s.digital_dead_from = 1;
    run(&s, &r);
    printf("  no digital modem: analogue %d, error %d at %d ms\n", r.a.state, r.a.error, r.a_done / 8);
    CHECK_EQ_I(r.a.state, P2L_FAILED);
    CHECK_EQ_I(r.a.error, P2L_ERR_INFO0);
    CHECK(r.a_done > 0 && r.a_done <= 8 * 8100);
    s = base(); s.digital_dead_from = 8 * 900;          /* gone after the INFO0 exchange */
    run(&s, &r);
    printf("  digital modem gone after INFO0: analogue %d, error %d at %d ms\n", r.a.state, r.a.error, r.a_done / 8);
    CHECK_EQ_I(r.a.state, P2L_FAILED);
    CHECK(r.a_done > 0 && r.a_done < 8 * 11000);

    printf("a retrain's Phase 2 bounded (spec D5)\n");
    {
        info0_t i0d;
        s = base();
        run(&s, &r);
        i0d = r.a.info0d;
        /* a digital modem that keeps sending INFO0d: each one sends us back
         * to 9.2.2.1.3, counted as a restart */
        s = base(); s.retrain = &i0d; s.d_reinit = 1; s.seconds = 120;
        run(&s, &r);
        printf("  INFO0d for ever: analogue %d, error %d at %d ms, restarts %d, INFO0a sent %d, digital started %d times\n",
               r.a.state, r.a.error, r.a_done / 8, r.a.restarts, r.a.n_info0c, r.d_inits);
        CHECK_EQ_I(r.a.state, P2L_FAILED);
        CHECK_EQ_I(r.a.error, P2L_ERR_REV);
        CHECK(r.a_done > 0 && r.a_done <= 8 * (4 * 8000 + 1000));
    }
    T_REPORT("p2v90");
}
