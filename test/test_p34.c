/* V.90 Phases 3 and 4 and data mode end to end: the digital modem against the
 * analogue test peer. Downstream the peer reads our PCM octets as sent;
 * upstream is a real line: delay, loss, noise, and the echo of our own PCM
 * signal through the hybrid into our receiver. */
#include <string.h>
#include <stdlib.h>
#include "test.h"
#include "../modem/p34d.h"
#include "../sim/p34a.h"
#include "../sim/noise.h"
#include "../v90/v90_pcm.h"

#define MAXD 2048
#define START_MAX_S 30    /* a start-up that never gets to data mode ends a run with mid-call events */

typedef struct { uint32_t s; long n, bad; } prbs_t;
static int prbs_next(prbs_t *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
typedef struct { prbs_t src, sink; } io_t;
static int  src_bit(void *u) { prbs_t *p = &((io_t *)u)->src; p->n++; return prbs_next(p); }
static void sink_bit(void *u, int bit) { prbs_t *p = &((io_t *)u)->sink; p->n++; if (prbs_next(p) != (bit & 1)) p->bad++; }

typedef struct {
    int rate, cn, cd;
    double ppm, gain, snr_db, echo, rot;
    int d_down, d_up;
    int dil_n, scr_in_dil, scr_syms, sr_d, ld_d, alaw;
    int seconds;            /* length of the run; with one of the mid-call events below: from the
                               moment the digital modem first reaches data mode */
    int echo_auto;          /* let the modem measure the echo delay */
    int echo_bulk;          /* extra delay of the echo path, samples (a line driver's buffering) */
    int echo_spread;        /* the echo path is a weak, smeared one with a long tail instead of one tap and a
                               small second one */
    int nseed;              /* another noise sequence */
    /* rate renegotiation and cleardown, ms after data mode was first reached (0 = never):
     * how long the start-up takes is then no business of these scenarios */
    int snr2_ms; double snr2_db;                /* the line noise changes in mid-call */
    int a_reneg_sil;                            /* ... with a request for silence (CPs) */
    int a_reneg_ms, a_reneg_kd, a_reneg_scr;    /* the caller asks, optionally for a new K, with SCR before CP */
    int a_clear_ms;                             /* the caller clears down */
    int d_reneg_ms, d_clear_ms;                 /* we do */
    int a_tone_ms;                              /* the caller gives the start-up up this long after we began MP:
                                                   70 ms of silence, then Tone A with its guard tone (0 = never) */
    int a_trn2d_ms;                             /* the caller trains on TRN2d as a known sequence and needs this
                                                   much of it: an MP that comes sooner makes it give up the same way */
} scen_t;

typedef struct {
    int state, error, d_data_at, a_tx, a_rx, up_rate, down_rate;
    long up_bits, up_bad, down_bits, down_bad;
    int snr_db10;
    int echo_delay;         /* what the modem used */
    int32_t worst_mse;      /* largest smoothed error of the upstream receiver from 1 s into data mode */
    /* around a renegotiation */
    long up_bad_before;     /* upstream errors until 100 ms before it */
    long up_bits2, up_bad2; /* upstream after it */
    int d_data2_at;         /* sample at which the digital modem was back in data mode */
    int ev_at;              /* sample at which the mid-call event came */
    int d_end_at;           /* sample at which it gave up */
    int silent_after_end;   /* it sent only silence from then on */
    int req_rc;
    int tone_at;            /* sample at which the caller's Tone A began (a_tone_ms) */
    p34a_t *a; p34d_t *d;
} out_t;

static void run(const scen_t *s, out_t *o)
{
    static p34d_t d; static p34a_t a;
    static uint8_t dl_o[MAXD]; static double dl_u[MAXD]; static int16_t dl_e[MAXD];
    static io_t dio, aio;
    p34d_cfg dc; p34a_cfg ac; rng_t rng;
    double nrms = s->snr_db > 0 ? s->gain / pow(10.0, s->snr_db / 20.0) : 0;
    uint8_t oct = v90_pcm_octet(s->alaw, 0, 1);
    int i, k, a_starts = 0, d_rx = 0, d_was = 0;
    const int first = s->a_reneg_ms ? s->a_reneg_ms : s->a_clear_ms ? s->a_clear_ms : s->d_reneg_ms ? s->d_reneg_ms : s->d_clear_ms ? s->d_clear_ms : s->snr2_ms;
    long sink0_n = 0, sink0_bad = 0;

    memset(&dc, 0, sizeof dc); memset(&ac, 0, sizeof ac);
    memset(dl_u, 0, sizeof dl_u); memset(dl_e, 0, sizeof dl_e);
    for (k = 0; k < MAXD; k++) dl_o[k] = oct;
    memset(&dio, 0, sizeof dio); memset(&aio, 0, sizeof aio);
    dio.src.s = 11; aio.sink.s = 11; aio.src.s = 23; dio.sink.s = 23;

    dc.alaw = s->alaw; dc.sym_rate = s->rate; dc.car_num = s->cn; dc.car_den = s->cd;
    dc.uinfo = 91; dc.md_len = 0; dc.rtd = s->d_down + s->d_up; dc.echo_delay = s->echo_auto ? -1 : s->echo_bulk;
    dc.getbit = src_bit; dc.putbit = sink_bit; dc.user = &dio;
    CHECK_EQ_I(p34d_init(&d, &dc), 0);

    ac.alaw = s->alaw; ac.uinfo = 91; ac.sym_rate = s->rate; ac.fc = (double)s->rate * s->cn / s->cd;
    ac.ppm = s->ppm; ac.gain = s->gain; ac.sil0 = 224; ac.ntrn = 600; ac.jd_wait = 100;
    ac.dil_n = s->dil_n; ac.dil_syms = 1500; ac.scr_in_dil = s->scr_in_dil; ac.scr_syms = s->scr_syms;
    ac.rot2 = s->rot;
    ac.Kt = 12; ac.Mt = 4; ac.Kd = 32; ac.Md = 41; ac.sr_d = s->sr_d; ac.ld_d = s->ld_d;
    ac.up_rates = 0x1FFF;
    ac.getbit = src_bit; ac.putbit = sink_bit; ac.user = &aio;
    p34a_init(&a, &ac);

    rng_seed(&rng, 99 + (uint32_t)s->nseed);
    memset(o, 0, sizeof *o); o->d_data_at = -1; o->a = &a; o->d = &d;
    for (i = 0; ; i++) {
        if (i >= (!first ? s->seconds * 8000 : o->d_data_at < 0 ? START_MAX_S * 8000 : o->d_data_at + s->seconds * 8000)) break;
        double rx;
        if (s->echo_spread) {
            /* as measured: a start that changes sign from sample to sample, then a long flat tail
             * (the low frequencies of what we send, through the modem's line transformer) */
            static const double head[7] = { 0.010, 0.011, -0.015, 0.007, 0.012, -0.005, 0.014 }; int k;
            rx = dl_u[(i - s->d_up + MAXD) % MAXD] + nrms * rng_gauss(&rng);
            for (k = 0; k < 7; k++) rx += head[k] * dl_e[(i - 9 - k - s->echo_bulk + MAXD) % MAXD];
            for (k = 0; k < 600; k++) rx += 0.010 * exp(-k / 300.0) * dl_e[(i - 16 - k - s->echo_bulk + MAXD) % MAXD];
        } else
        rx = dl_u[(i - s->d_up + MAXD) % MAXD] + s->echo * dl_e[(i - 9 - s->echo_bulk + MAXD) % MAXD]
           + 0.3 * s->echo * dl_e[(i - 14 - s->echo_bulk + MAXD) % MAXD] + nrms * rng_gauss(&rng);
        if (rx > 32767) rx = 32767;
        if (rx < -32768) rx = -32768;
        if (s->snr2_ms && o->d_data_at >= 0 && i == o->d_data_at + s->snr2_ms * 8) nrms = s->gain / pow(10.0, s->snr2_db / 20.0);
        oct = p34d_tick(&d, (int16_t)floor(rx + 0.5));
        dl_o[i % MAXD] = oct; dl_e[i % MAXD] = (int16_t)v90_octet_linear(s->alaw, oct);
        dl_u[i % MAXD] = p34a_tick(&a, dl_o[(i - s->d_down + MAXD) % MAXD]);
        if (s->a_tone_ms || s->a_trn2d_ms) {
            static int mp_at, trn_at; int since;
            if (i == 0) mp_at = trn_at = -1;
            if (trn_at < 0 && d.state == P34_TRN2D) trn_at = i;
            if (mp_at < 0 && d.state == P34_MP) mp_at = i;
            since = mp_at < 0 ? -1 : i - mp_at - s->a_tone_ms * 8;
            if (s->a_trn2d_ms && (mp_at < 0 || mp_at - trn_at >= s->a_trn2d_ms * 8)) since = -1;
            if (since >= 0 && o->tone_at == 0) o->tone_at = i + 560;
            if (since >= 0)
                dl_u[i % MAXD] = since < 560 ? 0.0
                               : s->gain * 2.0 * (sin(6.283185307179586 * 2400.0 * i / 8000.0) + 0.5 * sin(6.283185307179586 * 1800.0 * i / 8000.0));
        }
        if (o->d_data_at < 0 && d.state == P34_DATA) o->d_data_at = i;
        if (o->d_data_at >= 0 && i > o->d_data_at + 8000 && d.up.rx.mse > o->worst_mse) o->worst_mse = d.up.rx.mse;
        {
            /* milliseconds of data mode, and the first sample of each */
            int ms = o->d_data_at < 0 ? -1 : (i - o->d_data_at) / 8, ev = ms >= 0 && ((i - o->d_data_at) % 8) == 0;
            if (ev && first && ms == first) o->ev_at = i;
            if (ev && s->a_reneg_ms && ms == s->a_reneg_ms) { a.reneg_scr = s->a_reneg_scr; a.reneg_silence = s->a_reneg_sil; o->req_rc = p34a_renegotiate(&a, s->a_reneg_kd, 0); }
            if (ev && s->a_clear_ms && ms == s->a_clear_ms) o->req_rc = p34a_renegotiate(&a, 0, 1);
            if (ev && s->d_reneg_ms && ms == s->d_reneg_ms) o->req_rc = p34d_renegotiate(&d);
            if (ev && s->d_clear_ms && ms == s->d_clear_ms) o->req_rc = p34d_cleardown(&d);
            if (ev && first && ms == first - 100) o->up_bad_before = dio.sink.bad;
            /* the two data streams restart with the modems' data pumps */
            if (a.data_starts != a_starts) { a_starts = a.data_starts; aio.src.s = 23; }
            if (d.up.rx_ready && !d_rx) { dio.sink.s = 23; sink0_n = dio.sink.n; sink0_bad = dio.sink.bad; }
            d_rx = d.up.rx_ready;
            if (first && ms > first && o->d_data2_at == 0 && d_was != P34_DATA && d.state == P34_DATA) o->d_data2_at = i;
            d_was = d.state;
        }
        if (d.state == P34_RETRAIN) { o->d_end_at = i; break; }
        if (d.state == P34_FAILED) {
            int j;
            o->d_end_at = i; o->silent_after_end = 1;
            /* a signal of fixed length in progress (Rd: 48 ms) is completed first */
            for (j = 0; j < 480 + 800; j++)
                if (v90_octet_ucode(s->alaw, p34d_tick(&d, 0)) != 0 && j >= 480) o->silent_after_end = 0;
            break;
        }
    }
    o->up_bits2 = dio.sink.n - sink0_n; o->up_bad2 = dio.sink.bad - sink0_bad;
    o->echo_delay = d.ecd.delay;
    o->state = d.state; o->error = d.error; o->a_tx = a.tx_ready; o->a_rx = a.rx_ready;
    o->up_rate = d.up_rate; o->down_rate = d.down_rate; o->snr_db10 = d.snr_db10;
    o->up_bits = dio.sink.n; o->up_bad = dio.sink.bad; o->down_bits = aio.sink.n; o->down_bad = aio.sink.bad;
}

static void report(const char *name, const scen_t *s, const out_t *o)
{
    printf("  %s: state %d err %d, data at %.2f s, down %d bit/s (%ld bits, %ld bad), up %d bit/s (%ld bits, %ld bad), up SNR %.1f dB\n",
           name, o->state, o->error, o->d_data_at / 8000.0, o->down_rate, o->down_bits, o->down_bad,
           o->up_rate, o->up_bits, o->up_bad, o->snr_db10 / 10.0);
    if (o->state != P34_DATA) {
        const p34a_t *a = o->a; const p34d_t *d = o->d;
        printf("    peer: up state %d, down state %d; sdbar %d jd %d jd' %d ri %d ribar %d mp %d mp' %d ed %d\n",
               a->us, a->ds, a->sdbar_seen, a->jd_ok, a->jdp_seen, a->ri_seen, a->ribar_seen, a->mp_seen, a->mpack_seen, a->ed_seen);
        printf("    digital: rxmode %d, s_events %d, cpt %d cp %d cp' %d e %d, tx signal %d, mse %d\n",
               d->rxmode, d->s_events, d->got_cpt, d->got_cp, d->got_cp_ack, d->got_e, (int)v90tx_signal(&d->tx), (int)d->up.rx.mse);
    }
    (void)s;
}

static void check_good(const scen_t *s, const out_t *o)
{
    const p34a_t *a = o->a;
    CHECK_EQ_I(o->state, P34_DATA); CHECK_EQ_I(o->error, 0);
    CHECK(o->a_tx); CHECK(o->a_rx);
    /* what the digital modem sent, as the peer saw it */
    CHECK(a->trn1d_n >= 2040); CHECK_EQ_I(a->trn1d_bad, 0); CHECK_EQ_I(a->sign_bad, 0);
    CHECK(a->jd_ok >= 1); CHECK(a->jdp_seen);
    if (s->dil_n) { CHECK(a->dil_n_ok >= s->dil_n); }
    CHECK_EQ_I(a->dil_bad, 0);
    CHECK(a->ri_n >= 192); CHECK_EQ_I(a->ri_bad, 0);
    CHECK(a->trn2d_frames >= 340); CHECK_EQ_I(a->trn2d_bad, 0);
    CHECK(a->mp_n >= 2); CHECK_EQ_I(a->dec_bad, 0);
    CHECK_EQ_I(a->b1d_frames, 48); CHECK_EQ_I(a->b1d_bad, 0);
    /* rates */
    CHECK_EQ_I(o->down_rate, (32 + 6 - s->sr_d) * 8000 / 6);
    CHECK(o->up_rate >= 4800); CHECK_EQ_I(o->up_rate, a->up_rate);
    /* data both ways */
    CHECK(o->down_bits > 20000); CHECK_EQ_I(o->down_bad, 0);
    CHECK(o->up_bits > 5000); CHECK_EQ_I(o->up_bad, 0);
}

/* each run and its checks is a case (test/test.h, t_case_arg); the scenario
 * is set up as the whole program sets it up, whichever case runs */
#define NCASES 32

int main(int argc, char **argv)
{
    out_t o;
    scen_t s;
    const int only = t_case_arg(argc, argv, NCASES);

    /* 1. no DIL, clean line */
    memset(&s, 0, sizeof s);
    s.rate = 3200; s.cn = 3; s.cd = 5; s.gain = 2000; s.d_down = 20; s.d_up = 24; s.seconds = 8;
    if (T_CASE(only, 1)) {
        run(&s, &o); report("no DIL, clean", &s, &o); check_good(&s, &o);
    }

    /* 2. DIL, echo of our own signal, noise, clock offset, carrier phase jump */
    s.dil_n = 12; s.echo = 0.1; s.snr_db = 38; s.ppm = 60; s.rot = 2.2; s.seconds = 10;
    if (T_CASE(only, 2)) {
        run(&s, &o); report("DIL, echo -20 dB, 38 dB, +60 ppm", &s, &o); check_good(&s, &o);
        CHECK(o.up_rate >= 24000);
    }

    /* 3. the same with SCR during the DIL and before CP, shaping in data mode, A-law, 3000 symbols/s */
    s.rate = 3000; s.cn = 2; s.cd = 3; s.scr_in_dil = 1; s.scr_syms = 400; s.sr_d = 1; s.ld_d = 2; s.alaw = 1;
    s.ppm = -60; s.rot = -1.1;
    if (T_CASE(only, 3)) {
        run(&s, &o); report("3000, SCR, shaping, A-law", &s, &o); check_good(&s, &o);
    }

    /* 4. a poor line: the upstream rate must come down, without errors */
    s.snr_db = 24; s.echo = 0.25;
    if (T_CASE(only, 4)) {
        run(&s, &o); report("24 dB line", &s, &o); check_good(&s, &o);
        CHECK(o.up_rate <= 16800);
    }

    /* 5. a long call on a good line: the receiver's loops must stay quiet on
     * the dense data constellation (they once wandered and cost a burst of
     * errors every 20 s or so) */
    memset(&s, 0, sizeof s);
    s.rate = 3200; s.cn = 3; s.cd = 5; s.gain = 2000; s.d_down = 20; s.d_up = 24; s.seconds = 90;
    s.snr_db = 50; s.echo = 0.1; s.ppm = 30;
    if (T_CASE(only, 5)) {
        run(&s, &o); report("90 s, 50 dB", &s, &o); check_good(&s, &o);
        printf("    worst receiver error in data mode %.1f dB\n", 10 * log10((o.worst_mse + 1e-9) / 65536.0));
        CHECK(o.worst_mse < 65536 / 5000);      /* -37 dB */
        CHECK(o.up_rate >= 28800);              /* the rate is not taken from the moments after re-acquisition */
    }

    /* 6. the echo comes back late: a line driver that buffers 25 ms */
    memset(&s, 0, sizeof s);
    s.rate = 3200; s.cn = 3; s.cd = 5; s.gain = 2000; s.d_down = 20; s.d_up = 24; s.seconds = 10;
    s.snr_db = 45; s.echo = 0.25; s.echo_bulk = 200; s.dil_n = 6;
    if (T_CASE(only, 6)) {
        run(&s, &o); report("echo -12 dB, 25 ms late", &s, &o); check_good(&s, &o);
        CHECK(o.up_rate >= 28800);
    }

    /* 7. nobody tells us how late: the modem measures it during TRN1d */
    s.echo_bulk = 700; s.echo_auto = 1;
    if (T_CASE(only, 7)) {
        run(&s, &o); report("echo 88 ms late, measured", &s, &o); check_good(&s, &o);
        printf("    echo delay found: %d samples (the echo path starts at %d)\n", o.echo_delay, 700 + 9);
        CHECK(o.echo_delay >= 700 + 9 - 12 && o.echo_delay <= 700 + 9);
        CHECK(o.up_rate >= 28800);
    }
    /* the same search on a line without any echo must do no harm */
    s.echo = 0; s.echo_bulk = 0;
    if (T_CASE(only, 8)) {
        run(&s, &o); report("no echo, measured", &s, &o); check_good(&s, &o);
        CHECK(o.up_rate >= 28800);
    }
    /* and a short one */
    s.echo = 0.25; s.echo_bulk = 0;
    if (T_CASE(only, 9)) {
        run(&s, &o); report("echo at once, measured", &s, &o); check_good(&s, &o);
        CHECK(o.echo_delay <= 9);
        CHECK(o.up_rate >= 28800);
    }

    /* a weak echo smeared over many samples, under noise (a U.S. Robotics 5686
     * on the bench: 25 dB below what we send, 34 ms late, no tap standing out
     * and a long tail): the measurement must still find where it starts,
     * on every one of these lines: each line a case (10 .. 21); the last
     * one says on how many of those that ran the start was missed */
    {
        int k, missed = 0;
        for (k = 0; k < 12; k++) {
            if (!T_CASE(only, 10 + k)) continue;
            memset(&s, 0, sizeof s);
            s.rate = 3200; s.cn = 3; s.cd = 5; s.gain = 2000; s.d_down = 20; s.d_up = 24; s.seconds = 10;
            s.snr_db = 22; s.echo_spread = 1; s.echo_bulk = 259; s.echo_auto = 1; s.dil_n = 6; s.nseed = k;
            run(&s, &o);
            if (!(o.echo_delay >= 259 + 9 - 12 && o.echo_delay <= 259 + 11)) {
                missed++;
                printf("    weak smeared echo, noise %d: delay found %d (the echo path starts at %d)\n", k, o.echo_delay, 259 + 9);
            }
            if (only) CHECK(missed == 0);
            if (k == 11) {
                printf("  weak smeared echo 34 ms late: found on %d of 12 lines\n", 12 - missed);
                if (!only) CHECK(12 - missed == 12);
            }
        }
    }

    /* ---- rate renegotiation (9.6) and cleardown (9.7) ---- */
    memset(&s, 0, sizeof s);
    s.rate = 3200; s.cn = 3; s.cd = 5; s.gain = 2000; s.d_down = 20; s.d_up = 24; s.seconds = 6;
    s.snr_db = 45; s.echo = 0.1; s.ppm = 30; s.dil_n = 6;

    /* 8. the caller renegotiates and asks for a lower downstream rate */
    s.a_reneg_ms = 1000; s.a_reneg_kd = 30;
    if (T_CASE(only, 22)) {
        run(&s, &o); report("caller renegotiates, K 32 -> 30", &s, &o);
        printf("    back in data mode %.0f ms after the request; peer saw Rd %d + %d units (%d bad); up after: %ld bits, %ld bad\n",
               (o.d_data2_at - o.ev_at) / 8.0, o.a->rd_units, o.a->rdbar_units, o.a->rd_bad, o.up_bits2, o.up_bad2);
        CHECK_EQ_I(o.req_rc, 0);
        CHECK_EQ_I(o.state, P34_DATA); CHECK_EQ_I(o.error, 0);
        CHECK_EQ_I(o.a->renegs, 1);
        CHECK_EQ_I(o.a->rd_units, 64); CHECK_EQ_I(o.a->rdbar_units, 4); CHECK_EQ_I(o.a->rd_bad, 0);   /* 384 T and 24 T */
        CHECK_EQ_I(o.down_rate, (30 + 6) * 8000 / 6);
        CHECK_EQ_I(o.a->dec_bad, 0); CHECK_EQ_I(o.a->b1d_bad, 0);
        CHECK(o.ev_at > 0 && o.d_data2_at > o.ev_at && o.d_data2_at < o.ev_at + 8000);
        /* downstream: data frame alignment kept, no bit lost or invented across it */
        CHECK(o.down_bits > 265000); CHECK_EQ_I(o.down_bad, 0);
        /* upstream: clean before, clean after */
        CHECK_EQ_I(o.up_bad_before, 0);
        CHECK(o.up_bits2 > 80000); CHECK_EQ_I(o.up_bad2, 0);
        CHECK(o.a_tx); CHECK(o.a_rx);
    }

    /* 9. the same with SCR before the CP, as 9.6.2.1.2 allows */
    s.a_reneg_kd = 0; s.a_reneg_scr = 300;
    if (T_CASE(only, 23)) {
        run(&s, &o); report("caller renegotiates, SCR first", &s, &o);
        CHECK_EQ_I(o.state, P34_DATA); CHECK_EQ_I(o.a->renegs, 1);
        CHECK_EQ_I(o.down_bad, 0); CHECK(o.up_bits2 > 80000); CHECK_EQ_I(o.up_bad2, 0);
    }
    s.a_reneg_ms = 0; s.a_reneg_scr = 0;

    /* 10. the caller clears down: CP with drn = 0 */
    s.a_clear_ms = 1000;
    if (T_CASE(only, 24)) {
        run(&s, &o); report("caller clears down", &s, &o);
        printf("    call ended %.0f ms after the request\n", (o.d_end_at - o.ev_at) / 8.0);
        CHECK_EQ_I(o.state, P34_FAILED); CHECK_EQ_I(o.error, P34_ERR_CLEARDOWN);
        CHECK(o.ev_at > 0 && o.d_end_at > o.ev_at && o.d_end_at < o.ev_at + 4000);
        CHECK_EQ_I(o.up_bad_before, 0); CHECK_EQ_I(o.down_bad, 0);
        CHECK(o.silent_after_end);
    }
    s.a_clear_ms = 0;

    /* 11. we renegotiate (9.6.1.1) */
    s.d_reneg_ms = 1000;
    if (T_CASE(only, 25)) {
        run(&s, &o); report("we renegotiate", &s, &o);
        printf("    back in data mode %.0f ms after the request\n", (o.d_data2_at - o.ev_at) / 8.0);
        CHECK_EQ_I(o.req_rc, 0);
        CHECK_EQ_I(o.state, P34_DATA); CHECK_EQ_I(o.error, 0);
        CHECK_EQ_I(o.a->renegs, 1);
        CHECK_EQ_I(o.a->rd_units, 64); CHECK_EQ_I(o.a->rdbar_units, 4); CHECK_EQ_I(o.a->rd_bad, 0);
        CHECK(o.ev_at > 0 && o.d_data2_at > o.ev_at && o.d_data2_at < o.ev_at + 8000);
        CHECK(o.down_bits > 295000); CHECK_EQ_I(o.down_bad, 0);
        CHECK_EQ_I(o.up_bad_before, 0);
        CHECK(o.up_bits2 > 80000); CHECK_EQ_I(o.up_bad2, 0);
    }
    s.d_reneg_ms = 0;

    /* 12. we clear down: MP with drn = 0, then silence */
    s.d_clear_ms = 1000;
    if (T_CASE(only, 26)) {
        run(&s, &o); report("we clear down", &s, &o);
        printf("    call ended %.0f ms after the request\n", (o.d_end_at - o.ev_at) / 8.0);
        CHECK_EQ_I(o.req_rc, 0);
        CHECK_EQ_I(o.state, P34_FAILED); CHECK_EQ_I(o.error, P34_ERR_HANGUP);
        CHECK(o.a->clear_seen);
        CHECK_EQ_I(o.a->rd_units, 64); CHECK_EQ_I(o.a->rdbar_units, 4);
        CHECK(o.ev_at > 0 && o.d_end_at > o.ev_at && o.d_end_at < o.ev_at + 8000);
        CHECK_EQ_I(o.down_bad, 0); CHECK_EQ_I(o.up_bad_before, 0);
        CHECK(o.silent_after_end);
    }
    s.d_clear_ms = 0;

    /* 14. the caller asks for silence in the renegotiation, to work on its
     * echo canceller (CPs, 9.6.1.2.4 to 9.6.1.2.6, Figure 10) */
    s.a_reneg_ms = 1000; s.a_reneg_sil = 1; s.a_reneg_kd = 31;
    if (T_CASE(only, 27)) {
        run(&s, &o); report("caller renegotiates with silence", &s, &o);
        printf("    back in data mode %.0f ms after the request; silence %d octets (%d not silent), Rt %d + %d units\n",
               (o.d_data2_at - o.ev_at) / 8.0, o.a->sil_octets, o.a->sil_bad, o.a->rt_units, o.a->rtbar_units);
        CHECK_EQ_I(o.state, P34_DATA); CHECK_EQ_I(o.error, 0);
        CHECK_EQ_I(o.a->renegs, 1);
        CHECK(o.a->sil_octets >= 6 * 20); CHECK_EQ_I(o.a->sil_bad, 0);
        CHECK_EQ_I(o.a->sil_octets % 6, 0);                     /* data frame alignment kept */
        CHECK_EQ_I(o.a->rt_units, 64); CHECK_EQ_I(o.a->rtbar_units, 4); CHECK_EQ_I(o.a->rd_bad, 0);
        CHECK_EQ_I(o.down_rate, (31 + 6) * 8000 / 6);
        CHECK_EQ_I(o.a->dec_bad, 0); CHECK_EQ_I(o.a->b1d_bad, 0);
        CHECK(o.down_bits > 250000); CHECK_EQ_I(o.down_bad, 0);
        CHECK_EQ_I(o.up_bad_before, 0);
        CHECK(o.up_bits2 > 60000); CHECK_EQ_I(o.up_bad2, 0);
    }
    s.a_reneg_ms = 0; s.a_reneg_sil = 0; s.a_reneg_kd = 0;

    /* 15. the line gets worse in mid-call: the upstream rate we allowed no
     * longer fits. We renegotiate it down (MP with a lower drn) until the
     * receiver is clean again. */
    s.snr2_ms = 1000; s.snr2_db = 28; s.seconds = 11;
    if (T_CASE(only, 28)) {
        run(&s, &o); report("line drops to 28 dB in mid-call", &s, &o);
        printf("    rate lowered %d time(s); after the last: %ld bits, %ld bad\n", o.d->rate_drops, o.up_bits2, o.up_bad2);
        CHECK_EQ_I(o.state, P34_DATA); CHECK_EQ_I(o.error, 0);
        CHECK(o.d->rate_drops >= 1 && o.d->rate_drops <= 3);
        CHECK(o.up_rate <= 19200 && o.up_rate >= 9600);
        CHECK_EQ_I(o.up_rate, o.a->up_rate);
        CHECK(o.up_bits2 > 40000); CHECK_EQ_I(o.up_bad2, 0);
        CHECK_EQ_I(o.down_bad, 0);
        CHECK_EQ_I(o.up_bad_before, 0);
    }
    s.snr2_ms = 0; s.seconds = 6;

    /* 16. and when it becomes unusable altogether: the renegotiation cannot
     * complete either, and that is a retrain */
    s.snr2_ms = 1000; s.snr2_db = 3; s.seconds = 27;
    if (T_CASE(only, 29)) {
        run(&s, &o); report("line drops to 3 dB in mid-call", &s, &o);
        printf("    retrain asked for %.1f s after the line went bad\n", (o.d_end_at - o.ev_at) / 8000.0);
        CHECK_EQ_I(o.state, P34_RETRAIN); CHECK(o.d->retrain_ours);
        CHECK(o.ev_at > 0 && o.d_end_at > o.ev_at && o.d_end_at < o.ev_at + 15000 * 8);
    }
    s.snr2_ms = 0; s.seconds = 6;

    /* 17. the caller gives the start-up up in Phase 4 and asks for a retrain
     * (as a U.S. Robotics 56K did, 0.7 s into our MP): we hear its Tone A
     * there too, not only in data mode */
    s.a_tone_ms = 1; s.seconds = 30;
    if (T_CASE(only, 30)) {
        run(&s, &o); report("Tone A during our MP", &s, &o);
        printf("    retrain taken up %.0f ms after the tone began\n", (o.d_end_at - o.tone_at) / 8.0);
        CHECK_EQ_I(o.state, P34_RETRAIN); CHECK_EQ_I(o.error, 0); CHECK(!o.d->retrain_ours);
        CHECK(o.tone_at > 0 && o.d_end_at > o.tone_at + 50 * 8 && o.d_end_at < o.tone_at + 200 * 8);
    }
    s.a_tone_ms = 0; s.seconds = 9;        /* no mid-call event here: the whole run */

    /* 18. a caller that needs 0.8 s of TRN2d before the MP (a U.S. Robotics
     * 56K of 1998 gave up with 0.3 s and connected with 0.9 s) */
    s.a_trn2d_ms = 800;
    if (T_CASE(only, 31)) {
        run(&s, &o); report("a caller that needs 0.8 s of TRN2d", &s, &o);
        CHECK_EQ_I(o.state, P34_DATA); CHECK_EQ_I(o.error, 0);
        CHECK(o.up_bits > 10000); CHECK_EQ_I(o.up_bad, 0); CHECK_EQ_I(o.down_bad, 0);
    }
    s.a_trn2d_ms = 0;

    /* 13. a renegotiation that never gets anywhere (the caller sends S,
     * S-bar and then SCR for ever): 9.6.1 asks for a retrain */
    s.a_reneg_ms = 500; s.a_reneg_scr = 1000000; s.seconds = 7;
    if (T_CASE(only, 32)) {
        run(&s, &o); report("renegotiation stalls", &s, &o);
        CHECK_EQ_I(o.state, P34_RETRAIN); CHECK_EQ_I(o.error, 0);
        CHECK(o.d->retrain_ours);
    }
    s.a_reneg_ms = 0; s.a_reneg_scr = 0; s.seconds = 6;

    /* not in data mode: both requests are refused (with the last case) */
    if (T_CASE(only, 32)) {
        static p34d_t d0; p34d_cfg c0;
        memset(&c0, 0, sizeof c0);
        c0.sym_rate = 3200; c0.car_num = 3; c0.car_den = 5; c0.uinfo = 91;
        CHECK_EQ_I(p34d_init(&d0, &c0), 0);
        CHECK_EQ_I(p34d_renegotiate(&d0), -1);
        CHECK_EQ_I(p34d_cleardown(&d0), -1);
        CHECK_EQ_I(p34d_retrain(&d0), -1);
    }

    T_REPORT("p34");
}
