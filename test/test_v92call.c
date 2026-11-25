/* A V.92 call with PCM upstream in simulation: the digital modem
 * (v92/v92_p34d, production code) against the simulated analogue modem
 * (sim/v92a) through sim/v92chan: line shapes, noise, mu-law and A-law
 * codec, digital pads, hybrid echo. Phase 3 to data mode in every scenario;
 * the last ones start with Phase 2 (modem/p2d against sim/p2a), also with a
 * V.90 caller, which must go on through the V.90 Phases 3 and 4 as before. */
#include <string.h>
#include "test.h"
#include "../v92/v92_p34d.h"
#include "../sim/v92a.h"
#include "../sim/v92chan.h"
#include "../modem/p2d.h"
#include "../modem/p34d.h"
#include "../sim/p2a.h"
#include "../sim/p34a.h"
#include "../sim/noise.h"
#include "../v90/v90_pcm.h"

#define MAXD 2048

typedef struct { uint32_t s; long n, bad; } prbs_t;
typedef struct { prbs_t src, sink; } io_t;
static int prbs_next(prbs_t *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
static int  src_bit(void *u) { prbs_t *p = &((io_t *)u)->src; p->n++; return prbs_next(p); }
static void sink_bit(void *u, int bit) { prbs_t *p = &((io_t *)u)->sink; p->n++; if (prbs_next(p) != (bit & 1)) p->bad++; }

typedef struct {
    const char *name;
    int alaw, shape, dil_n;
    double level, noise, echo, up_pad, phase0, gain_err, echo_tail, wander;
    int echo_delay, d_down, d_up;
    int seconds;
    int max_up;
    uint32_t up_rates;
    int a_reneg_ms, d_reneg_ms, a_clear_ms, d_clear_ms, tone_a_ms, d_retrain_ms;
    int uinfo, ltot, lmax;      /* from Phase 2, 0 = defaults */
    int mute_ms;                /* the caller's signal stops for good */
    int deaf_ms;                /* from then on the caller no longer hears us (it gets the idle code) */
    int dead_ms;                /* from then on the line brings digital silence (no echo either) */
    int ru_ms, ru_amp;          /* from then on: the caller silent, a clean Ru of this amplitude in its place */
    int echo_search;            /* the echo delay is not given to the digital modem */
    int pace;                   /* samples per step of the CPd design (0: one) */
    int suvd_wait, cpd_wait;    /* the caller's patience in Phase 4, symbols (0: any) */
    int first_cpd_plain;        /* the caller answers a first CPd with the acknowledge bit by silence */
    int b1u_lead, z2_dead, conv_start, ja_no_ones, ntrn1, jd_wait;      /* the caller's ways, see ../sim/v92a.h */
    int drop_cpd;               /* the caller misses its first so many CPds */
    int no_suvu;                /* in a rate renegotiation the caller sends no SUVu */
    int no_ack;                 /* ... it never sets the acknowledge bit */
    int burst_ms[2]; double burst;          /* the line's noise rises to burst at burst_ms, and is
                                           back to noise when a rate drop of ours begins */
} scen_t;

typedef struct {
    int state, error, d_data_at, a_tx, a_rx, up_rate, down_rate, renegs, d_end_at, req_rc;
    int reneg_at;               /* the first sample in V92D_RENEG (-1: never) */
    int retrain_at;             /* the first sample in V92D_RETRAIN (-1: never) */
    int cpd_copies, cpd_run;    /* CPds we sent; the most of them in a row with no SUVd between */
    int reneg_cpds;             /* ... of them from the first renegotiation on */
    int cpd_on_suvu;            /* the first of those began after the caller's SUVu was in (-1: none) */
    char seq[40];               /* what we sent from the first renegotiation on, a letter a signal
                                   (D data, R Rd, r Rd-bar, T TRN2d, S SUVd, s SUVd', C CPd, c CPd', E Ed) */
    int drops;                  /* rate drops of ours (bad frames in data mode) */
    int drop_rate[4], drop_ceil[4];     /* each: the upstream rate then, the design's ceiling */
    long up_bits, up_bad, down_bits, down_bad;
    long up_bits2, up_bad2;     /* upstream after the last (re)start */
    int peak;                   /* where the channel peak ended up, 1/256 sample */
    double power;
    long ticks;
} out_t;

static v92d_t d;
static v92a_t a;

static void run(const scen_t *s, out_t *o)
{
    static v92chan_t ch;
    static uint8_t dl_o[MAXD];
    static io_t dio, aio;
    v92d_cfg dc; v92a_cfg ac; v92chan_cfg cc;
    uint8_t oct = v90_pcm_octet(s->alaw, 0, 1), idle = oct;
    int16_t rx = 0;
    int i, k, a_starts = 0, d_rx = 0, cpd_run = 0, cpd_more = 0;
    v90tx_sig_t psig = V90TX_SILENCE, qsig = V90TX_NONE;
    int nseq = 0;
    uint32_t punits = 0;
    long sink0_n = 0, sink0_bad = 0;

    memset(&dc, 0, sizeof dc); memset(&ac, 0, sizeof ac); memset(&cc, 0, sizeof cc);
    for (k = 0; k < MAXD; k++) dl_o[k] = oct;
    memset(&dio, 0, sizeof dio); memset(&aio, 0, sizeof aio);
    dio.src.s = 11; aio.sink.s = 11; aio.src.s = 23; dio.sink.s = 23;

    cc.alaw = s->alaw; cc.shape = s->shape; cc.level = s->level; cc.noise = s->noise;
    cc.echo_gain = s->echo; cc.echo_delay = s->echo_delay; cc.echo_tail = s->echo_tail; cc.wander = s->wander; cc.wander_hz = 25.0; cc.wander_from = 40000; cc.up_pad_db = s->up_pad; cc.up_delay = s->d_up; cc.seed = 5;
    v92chan_init(&ch, &cc);

    dc.alaw = s->alaw; dc.uinfo = s->uinfo ? s->uinfo : 91; dc.md_len = 0;
    dc.ltot = s->ltot ? s->ltot : 256; dc.lmax = s->lmax ? s->lmax : 192;
    dc.rtd = s->d_down + s->d_up; dc.echo_delay = s->echo_search ? -1 : s->echo_delay; dc.dsn_pace = s->pace;
    dc.max_up_rate = s->max_up;
    dc.getbit = src_bit; dc.putbit = sink_bit; dc.user = &dio;
    CHECK_EQ_I(v92d_init(&d, &dc), 0);

    ac.alaw = s->alaw; ac.uinfo = dc.uinfo; ac.sil0 = 560; ac.ntrn1 = 2040; ac.ntrn1b = 2040; ac.jd_wait = 100;
    ac.dil_n = s->dil_n; ac.dil_syms = 1500; ac.trn2u_min = s->suvd_wait ? 1000000 : 12000;
    ac.suvd_wait = s->suvd_wait; ac.cpd_wait = s->cpd_wait; ac.first_cpd_plain = s->first_cpd_plain;
    ac.up_rates = s->up_rates ? s->up_rates : 0x7FFFF;
    ac.Kt = 12; ac.Mt = 4; ac.Kd = 32; ac.Md = 41; ac.sr_d = 0; ac.ld_d = 0;
    ac.gain_err = s->gain_err;
    ac.b1u_lead = s->b1u_lead; ac.conv_start = s->conv_start; ac.ja_no_ones = s->ja_no_ones;
    if (s->ntrn1) ac.ntrn1 = s->ntrn1;
    if (s->jd_wait) ac.jd_wait = s->jd_wait;
    ac.z2_dead = s->z2_dead; ac.drop_cpd = s->drop_cpd; ac.reneg_no_suvu = s->no_suvu; ac.reneg_no_ack = s->no_ack;
    ac.getbit = src_bit; ac.putbit = sink_bit; ac.user = &aio;
    v92a_init(&a, &ac);
    a.phase = s->phase0;

    memset(o, 0, sizeof *o); o->d_data_at = -1; o->reneg_at = -1; o->retrain_at = -1; o->cpd_on_suvu = -1;
    for (i = 0; i < s->seconds * 8000; i++) {
        double amp, frac;
        int valid, ms = i / 8, ev = (i % 8) == 0;
        if (s->tone_a_ms && ms >= s->tone_a_ms && ms < s->tone_a_ms + 120)      /* the caller's Tone A */
            rx = (int16_t)(3000.0 * sin(2 * 3.14159265358979 * 2400.0 * i / 8000.0));
        oct = v92d_tick(&d, rx);
        {
            /* what we send: a CPd begins (a new signal, or another copy of
             * the one running: its unit count steps at the last sample of
             * a copy, and the signal is still a CPd on the next); an SUVd
             * between ends a run */
            v90tx_sig_t sg = v90tx_signal(&d.tx);
            uint32_t u = v90tx_units(&d.tx);
            int is_cpd = sg == V90TX_CPD || sg == V90TX_CPD_ACK;
            if (is_cpd && (sg != psig || (cpd_more && sg == psig))) {
                o->cpd_copies++;
                if (o->reneg_at >= 0 && o->reneg_cpds++ == 0) o->cpd_on_suvu = d.got_suvu;
                if (++cpd_run > o->cpd_run) o->cpd_run = cpd_run;
            } else if ((sg == V90TX_SUVD || sg == V90TX_SUVD_ACK) && sg != psig) {
                cpd_run = 0;
            }
            cpd_more = is_cpd && sg == psig && u != punits;
            if (o->reneg_at >= 0 && sg != qsig && nseq + 1 < (int)sizeof o->seq) {
                static const char letter[V90TX_NSIG] = {
                    [V90TX_DATA] = 'D', [V90TX_RD] = 'R', [V90TX_RD_BAR] = 'r', [V90TX_TRN2D] = 'T',
                    [V90TX_SUVD] = 'S', [V90TX_SUVD_ACK] = 's', [V90TX_CPD] = 'C', [V90TX_CPD_ACK] = 'c',
                    [V90TX_ED] = 'E', [V90TX_SILENCE] = '0' };
                o->seq[nseq++] = sg >= 0 && letter[sg] ? letter[sg] : '?';
                qsig = sg;
            }
            psig = sg; punits = u;
        }
        dl_o[i % MAXD] = oct;
        valid = v92a_tick(&a, s->deaf_ms && ms >= s->deaf_ms ? idle : dl_o[(i - s->d_down + MAXD) % MAXD], &amp, &frac);
        if (s->mute_ms && ms >= s->mute_ms) valid = 0;
        if (s->ru_ms && ms >= s->ru_ms) valid = 0;
        rx = v92chan_tick(&ch, oct, valid, amp, frac);
        if (s->ru_ms && ms >= s->ru_ms) rx = (int16_t)(rx + ((i / 3) % 2 ? -s->ru_amp : s->ru_amp));
        if (s->dead_ms && ms >= s->dead_ms) rx = 0;
        for (k = 0; k < 2; k++) if (ev && s->burst_ms[k] && ms == s->burst_ms[k]) ch.cfg.noise = s->burst;
        if (d.rate_drops > o->drops && o->drops < 4) {
            o->drop_rate[o->drops] = d.up_rate; o->drop_ceil[o->drops] = -1; o->drops++;
            ch.cfg.noise = s->noise;
        }
        if (o->drops && d.in_reneg && d.dsn_state == 1 && o->drop_ceil[o->drops - 1] < 0) o->drop_ceil[o->drops - 1] = d.dsn.cfg.max_rate;
        if (o->reneg_at < 0 && d.state == V92D_RENEG) o->reneg_at = i;
        if (o->d_data_at < 0 && d.state == V92D_DATA) { o->d_data_at = i; o->peak = v92_trn_peak(&d.trn); }
        if (ev && s->a_reneg_ms && ms == s->a_reneg_ms) o->req_rc = v92a_renegotiate(&a, 0, 0);
        if (ev && s->a_clear_ms && ms == s->a_clear_ms) o->req_rc = v92a_renegotiate(&a, 0, 1);
        if (ev && s->d_reneg_ms && ms == s->d_reneg_ms) o->req_rc = v92d_renegotiate(&d);
        if (ev && s->d_clear_ms && ms == s->d_clear_ms) o->req_rc = v92d_cleardown(&d);
        if (ev && s->d_retrain_ms && ms == s->d_retrain_ms) o->req_rc = v92d_retrain(&d);
        /* the upstream data stream restarts with the modems' data pumps */
        if (a.data_starts != a_starts) { a_starts = a.data_starts; aio.src.s = 23; }
        if (d.rx_ready && !d_rx) { dio.sink.s = 23; sink0_n = dio.sink.n; sink0_bad = dio.sink.bad; }
        d_rx = d.rx_ready;
        if (d.state == V92D_RETRAIN && o->retrain_at < 0) o->retrain_at = i;
        if (d.state == V92D_RETRAIN || d.state == V92D_FAILED) { o->d_end_at = i; break; }
    }
    o->ticks = i;
    o->up_bits2 = dio.sink.n - sink0_n; o->up_bad2 = dio.sink.bad - sink0_bad;
    o->state = d.state; o->error = d.error; o->a_tx = a.tx_ready; o->a_rx = a.rx_ready;
    o->up_rate = d.up_rate; o->down_rate = d.down_rate; o->renegs = d.renegs;
    o->up_bits = dio.sink.n; o->up_bad = dio.sink.bad; o->down_bits = aio.sink.n; o->down_bad = aio.sink.bad;
    o->power = a.pw_n ? a.pw_acc / (double)a.pw_n : 0;
}

static void report(const scen_t *s, const out_t *o)
{
    printf("  %-34s %s shape %d noise %-3.0f: state %d err %d, data after %d ms, up %d bit/s (LC %d, K %d, 4G %u, %d taps, delay %d), "
           "down %d, up %ld bits %ld bad, down %ld bits %ld bad, tx power %.2f, peak at %+.2f T\n",
           s->name, s->alaw ? "A-law " : "mu-law", s->shape, s->noise, o->state, o->error, o->d_data_at / 8, o->up_rate,
           d.cpd.map.lc[0], d.cpd.map.K, d.cpd.filt.gain4, d.cpd.filt.lz2 + d.cpd.filt.lp1, d.dsn.dly,
           o->down_rate, o->up_bits, o->up_bad, o->down_bits, o->down_bad, o->power, (double)(int8_t)(o->peak & 255) / 256.0);
    if (getenv("V92_VERBOSE"))
        printf("      noise floor %.1f (quantiser %.1f), echo %u, training residual %.1f (quantiser %.1f), rho %.2f, ISI %.1f dB, "
               "x rms max %u, points %d, decoder noise %.1f, bad frames %u, design steps %d\n",
               sqrt((double)d.noise), sqrt((double)d.q_quiet), d.dsn.cfg.echo_rms, sqrt((double)d.dsn.cfg.res), sqrt((double)d.dsn.cfg.q_trn),
               (double)d.dsn.rho_q8 / 256.0, 10 * log10((double)d.dsn.isi_q28 / 268435456.0 + 1e-12), d.dsn.xrms_max, d.dsn.ncand,
               sqrt((double)v92_dec_quality(&d.dec)), d.dec.bad_frames, d.dsn.steps);
}

/* the start-up went by the book and data flows both ways without errors */
static void check_good(const scen_t *s, const out_t *o)
{
    CHECK_EQ_I(o->state, V92D_DATA); CHECK_EQ_I(o->error, 0);
    CHECK(o->a_tx); CHECK(o->a_rx);
    /* what the analogue modem saw of our signals */
    CHECK(a.trn1d_n >= 2040 - 48); CHECK_EQ_I(a.trn1d_bad, 0); CHECK_EQ_I(a.sign_bad, 0);
    CHECK(a.jd_ok >= 1); CHECK(a.jp_ok >= 1); CHECK(a.jpp_seen);
    if (s->dil_n) { CHECK(a.dil_n_ok > 50); CHECK_EQ_I(a.dil_bad, 0); }
    else { CHECK(a.scr_n > 2000); CHECK_EQ_I(a.scr_bad, 0); }
    CHECK(a.ri_n >= 48); CHECK_EQ_I(a.ri_bad, 0); CHECK(a.ribar_seen);
    CHECK(a.trn2d_frames >= 340); CHECK_EQ_I(a.trn2d_bad, 0);
    CHECK(a.suvd_n >= 2); CHECK(a.cpd_n >= 1); CHECK(a.suvd_ack || a.cpd_ack);
    CHECK_EQ_I(a.seq_bad, 0); CHECK_EQ_I(a.dec_bad, 0);
    CHECK_EQ_I(a.b1d_bad, 0); CHECK_EQ_I(d.b1_bad, 0);
    /* the phase asked for in Jp put the peak of the channel on a sampling instant */
    CHECK(abs((int8_t)(o->peak & 255)) < 56);
    /* rates */
    CHECK_EQ_I(o->down_rate, (32 + 6) * 8000 / 6);
    CHECK(o->up_rate >= 24000 && o->up_rate <= 48000); CHECK_EQ_I(o->up_rate, a.up_rate);
    if (s->max_up) CHECK(o->up_rate <= s->max_up);
    CHECK(o->power <= 1.1);
    /* data both ways */
    CHECK(o->down_bits > 3 * 8000 * 38 / 6); CHECK_EQ_I(o->down_bad, 0);
    CHECK(o->up_bits > 3 * 24000); CHECK_EQ_I(o->up_bad, 0);
    CHECK_EQ_I(d.dec.bad_frames, 0);
}

/* our cleardown's signals as 9.8.1.1.2 and 9.6.1.1.2 give them: data, Rd,
 * Rd-bar, TRN2d, SUVd; with cpd, then one CPd (no acknowledge bit) and
 * after it SUVd only, the last of them SUVd'; without, nothing after SUVd */
static int clear_seq_ok(const char *q, int cpd)
{
    const char *c;
    size_t n = strlen(q);
    if (strncmp(q, "DRrTS", 5) != 0) return 0;
    if (!cpd) return n == 5;
    c = strchr(q, 'C');
    if (!c || strchr(q, 'c') || strchr(c + 1, 'C')) return 0;
    if (c[1] == 0 || strspn(c + 1, "Ss") != strlen(c + 1)) return 0;
    return q[n - 1] == 's';
}

static void base(scen_t *s, const char *name, int alaw, int shape, double noise, int dil_n)
{
    memset(s, 0, sizeof *s);
    s->name = name; s->alaw = alaw; s->shape = shape; s->noise = noise; s->dil_n = dil_n;
    s->level = 4000; s->echo = 0.05; s->echo_delay = 3; s->d_down = 20; s->d_up = 24; s->seconds = 9;
    s->phase0 = 0.3;
}

/* ---- Phase 2 in front ---- */

typedef struct { p2d_t d; p2a_t a; } p2_t;

static void phase2(p2_t *r, int caller_v92)
{
    static int16_t lad[MAXD], lda[MAXD];
    p2d_cfg dc; p2a_cfg ac; rng_t rng;
    int16_t dtx = 0, atx = 0;
    int i;
    memset(lad, 0, sizeof lad); memset(lda, 0, sizeof lda);
    memset(&dc, 0, sizeof dc); memset(&ac, 0, sizeof ac);
    dc.tx_dbm0 = -12; dc.max_tx_code = 23; dc.up_sr_mask = (1 << 3) | (1 << 4); dc.up_hi_carrier = 1 << 4;
    p2d_init(&r->d, &dc);
    p2d_offer_v92(&r->d, 1);                /* a V.92 digital modem, PCM upstream offered */
    ac.level = 4000; ac.v90 = 1; ac.up_sr = -1; ac.uinfo = 91;
    ac.info0a.lo3000 = ac.info0a.hi3000 = 1; ac.info0a.lo3200 = ac.info0a.hi3200 = 1;
    ac.info0a.pwr_reduce = 1; ac.info0a.max_sr_diff = 5; ac.info0a.c1664 = 1;
    ac.info0a.v92 = (uint8_t)caller_v92;
    ac.pcm_up = caller_v92; ac.sections = 2; ac.ltot_code = 1; ac.lmax_code = 1; ac.md_len = 0;
    p2a_init(&r->a, &ac);
    rng_seed(&rng, 7);
    for (i = 0; i < 8000 * 8; i++) {
        double xd, xa;
        lad[i % MAXD] = atx; lda[i % MAXD] = dtx;
        xd = 0.5 * lad[(i - 20 + MAXD) % MAXD] + 0.1 * dtx + 2.0 * rng_gauss(&rng);
        xa = 0.5 * lda[(i - 24 + MAXD) % MAXD] + 0.1 * atx + 2.0 * rng_gauss(&rng);
        dtx = p2d_tick(&r->d, (int16_t)xd);
        atx = p2a_tick(&r->a, (int16_t)xa);
        if ((r->d.state == P2D_DONE || r->d.state == P2D_FAILED) &&
            (r->a.state == P2A_DONE || r->a.state == P2A_FAILED)) break;
    }
}

/* a V.90 caller after Phase 2: V.90 Phases 3 and 4 (as test_p34.c) */
static void v90_call(const p2_t *r)
{
    static p34d_t pd; static p34a_t pa;
    static uint8_t dl_o[MAXD]; static double dl_u[MAXD]; static int16_t dl_e[MAXD];
    static io_t dio, aio;
    p34d_cfg dc; p34a_cfg ac;
    int sr = r->d.info1a.up_sr, hi = sr == 4, i, k;
    uint8_t oct = v90_pcm_octet(0, 0, 1);
    memset(&dc, 0, sizeof dc); memset(&ac, 0, sizeof ac);
    memset(dl_u, 0, sizeof dl_u); memset(dl_e, 0, sizeof dl_e);
    for (k = 0; k < MAXD; k++) dl_o[k] = oct;
    memset(&dio, 0, sizeof dio); memset(&aio, 0, sizeof aio);
    dio.src.s = 11; aio.sink.s = 11; aio.src.s = 23; dio.sink.s = 23;
    dc.uinfo = r->d.info1a.uinfo; dc.md_len = r->d.info1a.md_len; dc.rtd = 44; dc.echo_delay = 0;
    if (sr == 3) { dc.sym_rate = 3000; dc.car_num = hi ? 2 : 3; dc.car_den = hi ? 3 : 5; }
    else { dc.sym_rate = 3200; dc.car_num = hi ? 3 : 4; dc.car_den = hi ? 5 : 7; }
    dc.getbit = src_bit; dc.putbit = sink_bit; dc.user = &dio;
    CHECK_EQ_I(p34d_init(&pd, &dc), 0);
    ac.uinfo = dc.uinfo; ac.sym_rate = dc.sym_rate; ac.fc = (double)dc.sym_rate * dc.car_num / dc.car_den;
    ac.gain = 2000; ac.sil0 = 224; ac.ntrn = 600; ac.jd_wait = 100; ac.dil_syms = 1500;
    ac.Kt = 12; ac.Mt = 4; ac.Kd = 32; ac.Md = 41; ac.up_rates = 0x1FFF;
    ac.getbit = src_bit; ac.putbit = sink_bit; ac.user = &aio;
    p34a_init(&pa, &ac);
    for (i = 0; i < 8 * 8000; i++) {
        double rx = dl_u[(i - 24 + MAXD) % MAXD] + 0.05 * dl_e[(i - 9 + MAXD) % MAXD];
        oct = p34d_tick(&pd, (int16_t)floor(rx + 0.5));
        dl_o[i % MAXD] = oct; dl_e[i % MAXD] = (int16_t)v90_octet_linear(0, oct);
        dl_u[i % MAXD] = p34a_tick(&pa, dl_o[(i - 20 + MAXD) % MAXD]);
        if (pd.state == P34_RETRAIN || pd.state == P34_FAILED) break;
    }
    printf("  V.90 caller after a V.92 Phase 2: state %d err %d, down %d up %d bit/s, up %ld bits %ld bad, down %ld bits %ld bad\n",
           pd.state, pd.error, pd.down_rate, pd.up_rate, dio.sink.n, dio.sink.bad, aio.sink.n, aio.sink.bad);
    CHECK_EQ_I(pd.state, P34_DATA); CHECK(pa.tx_ready && pa.rx_ready);
    CHECK_EQ_I(pd.down_rate, 38 * 8000 / 6); CHECK(pd.up_rate >= 4800);
    CHECK(aio.sink.n > 20000); CHECK_EQ_I(aio.sink.bad, 0);
    CHECK(dio.sink.n > 5000); CHECK_EQ_I(dio.sink.bad, 0);
}

int main(int argc, char **argv)
{
    static p2_t p2;
    scen_t s;
    out_t o, clean_mu, clean_a;
    int only = argc > 1 ? atoi(argv[1]) : 0;
#define WANT(n) (!only || only == (n))

    /* ---- law, line shape, noise ---- */
    if (WANT(1)) {
        base(&s, "1 mu-law, short line, no DIL", 0, 0, 3, 0);
        run(&s, &o); report(&s, &o); check_good(&s, &o); clean_mu = o;
        CHECK(o.up_rate > 33600);
        base(&s, "2 A-law, short line, DIL", 1, 0, 3, 40);
        run(&s, &o); report(&s, &o); check_good(&s, &o); clean_a = o;
        CHECK(o.up_rate > 33600);
        base(&s, "3 mu-law, short line, noisy", 0, 0, 40, 40);
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(o.up_rate < clean_mu.up_rate);
        base(&s, "4 A-law, short line, noisy", 1, 0, 40, 0);
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(o.up_rate < clean_a.up_rate);
    }
    if (WANT(2)) {
        base(&s, "5 mu-law, long line", 0, 1, 3, 40);
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(o.up_rate > 33600);
        base(&s, "6 A-law, long line, noisy", 1, 1, 40, 40);
        s.phase0 = 0.85;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "7 A-law, long line with a bump", 1, 2, 3, 0);
        s.phase0 = 0.6;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "8 mu-law, bump, noisy, weak", 0, 2, 20, 40);
        s.level = 1500;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
    }
    /* ---- pads, echo, delay ---- */
    if (WANT(3)) {
        base(&s, "9 mu-law, 3 dB pad upstream", 0, 0, 3, 0);
        s.up_pad = 3;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "10 A-law, 6 dB pad upstream", 1, 1, 3, 40);
        s.up_pad = 6;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "11 mu-law, strong late echo", 0, 0, 3, 40);
        s.echo = 0.25; s.echo_delay = 60; s.d_down = 150; s.d_up = 170;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "12 A-law, no echo, loud", 1, 0, 3, 0);
        s.echo = 0; s.level = 9000;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
    }
    /* ---- our limits ---- */
    if (WANT(4)) {
        base(&s, "13 ceiling 33333 bit/s", 0, 0, 3, 0);
        s.max_up = 33333;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.up_rate, 33333);
        base(&s, "14 caller allows 24000..32000", 1, 0, 3, 40);
        s.up_rates = 0x7F;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.up_rate, 32000);
    }
    /* ---- rate renegotiation, cleardown, retrain ---- */
    if (WANT(5)) {
        base(&s, "15 caller renegotiates", 0, 0, 3, 0);
        s.a_reneg_ms = 5500; s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.renegs, 1); CHECK_EQ_I(a.renegs, 1);
        CHECK(o.up_bits2 > 2 * 24000); CHECK_EQ_I(o.up_bad2, 0);
        /* the same on a noisy line: the data frames the decoder held before
         * Ru are delivered, decided on the inputs it had (the baseline moved
         * under Ru before it was heard) */
        base(&s, "15b caller renegotiates, short noisy line", 0, 0, 40, 40);
        s.echo = 0.05; s.level = 4000; s.phase0 = 0.4;
        s.a_reneg_ms = 5541; s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.renegs, 1);
        CHECK_EQ_I(o.up_bad2, 0);
        base(&s, "16 we renegotiate", 1, 1, 3, 40);
        s.d_reneg_ms = 5500; s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.renegs, 1); CHECK_EQ_I(a.renegs, 1);
        CHECK(a.rd_units >= 60 && a.rdbar_units == 4); CHECK_EQ_I(a.rd_bad, 0);
        CHECK(o.up_bits2 > 2 * 24000); CHECK_EQ_I(o.up_bad2, 0);
        base(&s, "17 caller clears down", 0, 0, 3, 0);
        s.a_clear_ms = 5500; s.seconds = 9;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_CLEARDOWN);
        CHECK_EQ_I(o.up_bad, 0); CHECK_EQ_I(o.down_bad, 0);
        /* 9.11: a rate renegotiation that sends drn = 0. 9.8.1.1.2: Rd,
         * Rd-bar, TRN2d, then SUVd; on the caller's SUVu a single CPd (the
         * one in use, drn = 0) and more SUVd (9.6.1.1.2), with the
         * acknowledge bit once the caller's CPu is in; over where 9.6.1.1.4
         * would send Ed: our SUVd' out, the caller's acknowledge in */
        base(&s, "18 we clear down", 0, 0, 3, 0);
        s.d_clear_ms = 5500; s.seconds = 9;
        run(&s, &o); report(&s, &o);
        printf("    sent %s; CPds %d, the first after SUVu %d; over after %d ms\n", o.seq, o.reneg_cpds, o.cpd_on_suvu,
               (o.d_end_at - 5500 * 8) / 8);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_HANGUP);
        CHECK(a.clear_seen); CHECK_EQ_I(o.up_bad, 0); CHECK_EQ_I(o.down_bad, 0);
        CHECK(o.d_end_at < 5500 * 8 + 4 * 8000);
        CHECK_EQ_I(o.retrain_at, -1);
        CHECK(clear_seq_ok(o.seq, 1));
        CHECK_EQ_I(o.reneg_cpds, 1); CHECK_EQ_I(o.cpd_on_suvu, 1);
        CHECK(a.suvd_ack);                          /* the caller heard our SUVd' */
        /* the caller no longer hears us: Rd, TRN2d and SUVd go out all the
         * same, no SUVu comes, and at our limit that is our cleardown, not
         * a retrain */
        base(&s, "18b we clear down, the caller deaf to us", 0, 0, 3, 0);
        s.d_clear_ms = 5500; s.deaf_ms = 5400; s.seconds = 16;
        run(&s, &o);
        printf("  %-34s state %d err %d after %d ms\n", s.name, o.state, o.error, (o.d_end_at - 5500 * 8) / 8);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_HANGUP);
        CHECK_EQ_I(o.retrain_at, -1);
        CHECK(o.d_end_at > 5500 * 8 && o.d_end_at < 5500 * 8 + 4 * 8000);
        CHECK_EQ_I(o.reneg_cpds, 0);
        /* the caller hears us and answers Rd with Ru, but sends no SUVu:
         * SUVd until our limit, no CPd, our cleardown all the same */
        base(&s, "18c we clear down, the caller sends no SUVu", 0, 0, 3, 0);
        s.d_clear_ms = 5500; s.no_suvu = 1; s.seconds = 16;
        run(&s, &o);
        printf("  %-34s state %d err %d after %d ms; sent %s\n", s.name, o.state, o.error, (o.d_end_at - 5500 * 8) / 8, o.seq);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_HANGUP);
        CHECK_EQ_I(o.retrain_at, -1);
        CHECK(o.d_end_at > 5500 * 8 && o.d_end_at < 5500 * 8 + 4 * 8000);
        CHECK(clear_seq_ok(o.seq, 0)); CHECK_EQ_I(o.reneg_cpds, 0);
        CHECK(a.rd_units >= 60); CHECK(a.suvd_n > 0);
        /* the caller sends SUVu and CPu but never the acknowledge bit: our
         * CPd again and again (9.6.1.1.3), SUVd between, until our limit */
        base(&s, "18d we clear down, the caller never acknowledges", 0, 0, 3, 0);
        s.d_clear_ms = 5500; s.no_ack = 1; s.seconds = 16;
        run(&s, &o);
        printf("  %-34s state %d err %d after %d ms; CPds %d, at most %d in a row\n", s.name, o.state, o.error,
               (o.d_end_at - 5500 * 8) / 8, o.reneg_cpds, o.cpd_run);
        CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_HANGUP);
        CHECK_EQ_I(o.retrain_at, -1);
        CHECK(o.d_end_at > 5500 * 8 + 3000 * 8 && o.d_end_at < 5500 * 8 + 4 * 8000);
        CHECK(o.reneg_cpds >= 2); CHECK_EQ_I(o.cpd_run, 1); CHECK(a.clear_seen);
        /* the caller's Tone A (its retrain) during our cleardown: still our
         * cleardown, at once, no retrain */
        base(&s, "18e the caller's Tone A in our cleardown", 0, 0, 3, 0);
        s.d_clear_ms = 5500; s.tone_a_ms = 5600; s.seconds = 9;
        run(&s, &o);
        printf("  %-34s state %d err %d after %d ms\n", s.name, o.state, o.error, (o.d_end_at - 5500 * 8) / 8);
        CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_HANGUP);
        CHECK_EQ_I(o.retrain_at, -1);
        CHECK(o.d_end_at < 5600 * 8 + 120 * 8);
        /* both ends clear down at once: the caller's CPu with drn = 0 in
         * ours ends it as ours */
        base(&s, "18f both ends clear down at once", 0, 0, 3, 0);
        s.d_clear_ms = 5500; s.a_clear_ms = 5500; s.seconds = 9;
        run(&s, &o);
        printf("  %-34s state %d err %d after %d ms\n", s.name, o.state, o.error, (o.d_end_at - 5500 * 8) / 8);
        CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_HANGUP);
        CHECK_EQ_I(o.retrain_at, -1);
        CHECK(o.d_end_at < 5500 * 8 + 4 * 8000);
        base(&s, "19 the caller's Tone A", 0, 0, 3, 0);
        s.tone_a_ms = 5500;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.state, V92D_RETRAIN); CHECK_EQ_I(d.retrain_ours, 0);
        CHECK(o.d_end_at > 5500 * 8 + 400 && o.d_end_at < 5500 * 8 + 1000);
        base(&s, "20 we retrain", 0, 0, 3, 0);
        s.d_retrain_ms = 5500;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.state, V92D_RETRAIN); CHECK_EQ_I(d.retrain_ours, 1);
    }
    /* ---- the line gets worse in data mode: our own rate renegotiations ---- */
    if (WANT(11)) {
        /* the noise rises twice (each time back to what it was once the
         * drop has begun: the design does not see the noise of data mode);
         * each time the decoder's bad frames bring a renegotiation of ours
         * whose design may go no higher than one step (4000 bit/s) below
         * the rate then in use */
        int n;
        base(&s, "36 the noise rises in data mode, twice", 0, 0, 3, 0);
        s.burst_ms[0] = 4500; s.burst_ms[1] = 15000; s.burst = 90;
        s.a_reneg_ms = 22000; s.seconds = 26;      /* then the caller renegotiates */
        run(&s, &o); report(&s, &o);
        for (n = 0; n < o.drops; n++) printf("    drop %d: at %d bit/s, ceiling %d\n", n + 1, o.drop_rate[n], o.drop_ceil[n]);
        printf("    the caller's renegotiation after them: ceiling %d, %d bit/s\n", d.dsn.cfg.max_rate, o.up_rate);
        CHECK_EQ_I(o.drops, 2);
        for (n = 0; n < o.drops && n < 4; n++) CHECK_EQ_I(o.drop_ceil[n], o.drop_rate[n] - 4000);
        /* the drops do not hold the caller's later renegotiation down */
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.renegs, 3); CHECK_EQ_I(o.state, V92D_DATA);
        CHECK_EQ_I(d.dsn.cfg.max_rate, 0);
    }
    /* ---- the echo delay measured during TRN1d; the caller's signal lost ---- */
    if (WANT(7)) {
        base(&s, "22 echo 60 samples late, not told", 0, 0, 3, 40);
        s.echo = 0.25; s.echo_delay = 60; s.echo_search = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        /* a little room before the echo path, as p34d leaves */
        CHECK(d.ecd.delay >= 60 - 12 && d.ecd.delay <= 60);
        base(&s, "23 A-law, echo 400 late, not told", 1, 1, 3, 0);
        s.echo_delay = 400; s.echo_search = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(d.ecd.delay >= 400 - 12 && d.ecd.delay <= 400);
        base(&s, "24 the caller falls silent", 0, 0, 3, 0);
        s.mute_ms = 5500;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_CARRIER);
        CHECK(o.d_end_at > 5500 * 8 + 3000 && o.d_end_at < 5500 * 8 + 6000);
        /* next to nothing was delivered from the silence: the tail in the
         * decoder and at most the 10 ms the carrier watch needs to see it */
        CHECK(o.up_bad < 400);
        /* the line goes dead (the caller hung up; the network sends
         * digital silence): what the canceller gives out is then its own
         * estimate of our echo, and that is no carrier, nor (when what we
         * send is Rd) the caller's Ru */
        base(&s, "24b the line goes dead", 0, 0, 3, 0);
        s.dead_ms = 5500;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_CARRIER);
        CHECK(o.d_end_at > 5500 * 8 + 3000 && o.d_end_at < 5500 * 8 + 6000);
        CHECK_EQ_I(o.reneg_at, -1); CHECK_EQ_I(o.renegs, 0);
        CHECK(o.up_bad < 400);
        base(&s, "24c the line goes dead as we renegotiate", 0, 0, 3, 0);
        s.dead_ms = 5500; s.d_reneg_ms = 5500;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.req_rc, 0);
        CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_CARRIER);
        CHECK(o.d_end_at > 5500 * 8 + 3000 && o.d_end_at < 5500 * 8 + 6000);
        CHECK_EQ_I(d.revs, 0); CHECK_EQ_I(o.renegs, 0);
        CHECK(o.up_bad < 400);
        /* and the caller's Ru as weak as the detector takes it (20: 18 is
         * not taken), in place of its data with our echo under it */
        base(&s, "24d the caller's weakest Ru", 0, 0, 3, 0);
        s.ru_ms = 5500; s.ru_amp = 20; s.seconds = 7;
        run(&s, &o);
        printf("  %-34s Ru of amplitude %d taken after %d samples\n", s.name, s.ru_amp, o.reneg_at - 5500 * 8);
        CHECK(o.reneg_at > 5500 * 8 && o.reneg_at < 5500 * 8 + 1200);
    }
    /* ---- what the first real V.92 caller showed (live/v92a.raw, readings 53 on) ---- */
    if (WANT(8)) {
        /* an FXS port: our echo as loud as the caller (9 dB under what we
         * send) and 270 samples late, with a slow tail that a 12 ms canceller
         * leaves as a wander of some 150 units */
        base(&s, "25 echo at the caller's level, slow tail", 0, 0, 5, 40);
        s.level = 1400; s.echo = 0.2; s.echo_tail = 0.57; s.echo_delay = 270; s.echo_search = 1;
        s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);

        /* a U.S. Robotics 5686 on the same port: the echo 25 dB under what we
         * send, its start no larger than one tap of the slow tail behind it;
         * the delay must still be found (live/usr2.raw: 300 taken for 270,
         * the canceller beside the echo, the design gave up) */
        base(&s, "25b weak echo under its own slow tail", 0, 0, 5, 40);
        s.level = 2700; s.echo = 0.02; s.echo_tail = 4.0; s.echo_delay = 270; s.echo_search = 1;
        s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        printf("    echo delay found: %d\n", d.ecd.delay);
        CHECK(d.ecd.delay >= 270 - 12 && d.ecd.delay <= 270 + 4);

        /* the same modem's patience: it gives up 3.8 s into TRN2u without
         * SUVd and 3.5 s after its SUVu without CPd, and does not send SUVu
         * by itself. A design at eight samples a step takes longer than
         * the first and must still arrive within the second; CPu is in by
         * then, and a first CPd that says so is answered with silence. */
        base(&s, "25c a caller that waits 3.8 s for SUVd, 3.5 s for CPd", 0, 0, 5, 40);
        s.level = 2700; s.echo = 0.02; s.echo_tail = 1.2; s.echo_delay = 270; s.echo_search = 1;
        s.pace = 8; s.suvd_wait = 30400; s.cpd_wait = 28000; s.first_cpd_plain = 1; s.seconds = 14;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.cpd_copies, 1); CHECK_EQ_I(o.cpd_run, 1);

        /* a caller that misses its first CPds (9.6.1.1.3): CPd again, each
         * copy with SUVd before it, 9.6.1.1.2's single CPd followed by
         * SUVd once per copy (Figures 12 to 14 draw no repeat); a real
         * Agere engine cleared down on CPds sent back to back */
        {
            static const int drop[] = { 2, 4 };
            static char name[48];
            unsigned n;
            for (n = 0; n < sizeof drop / sizeof drop[0]; n++) {
                snprintf(name, sizeof name, "25e the caller misses %d CPds", drop[n]);
                base(&s, name, (int)n, 0, 3, n ? 40 : 0);
                s.drop_cpd = drop[n]; s.seconds = 11;
                run(&s, &o); report(&s, &o); check_good(&s, &o);
                printf("    CPds sent %d, at most %d in a row\n", o.cpd_copies, o.cpd_run);
                CHECK_EQ_I(a.cpd_dropped, drop[n]);
                CHECK(o.cpd_copies >= drop[n] + 1);
                CHECK_EQ_I(o.cpd_run, 1);
            }
        }

        /* and what it showed in data mode: a slow wander under the symbols
         * (our own low frequencies, which the tail estimate does not take
         * away once the downstream carries data), a hundred units where
         * the points are 250 apart. Here it begins at 5 s, after the
         * design, as a sine of 25 Hz. */
        base(&s, "25d a slow wander under the data", 0, 0, 5, 40);
        s.level = 2700; s.echo = 0.02; s.echo_tail = 1.2; s.echo_delay = 270; s.echo_search = 1;
        s.wander = 100; s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);

        /* a data frame of Ki = 0 before B1u (reading 57) */
        base(&s, "26 a data frame of Ki = 0 before B1u", 0, 1, 3, 40);
        s.b1u_lead = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(d.b1_leads, 1);
        base(&s, "27 the same, A-law, another line", 1, 0, 3, 0);
        s.b1u_lead = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(d.b1_leads, 1);

        /* the caller's prefilter has no coefficient 64 (reading 58) */
        base(&s, "28 z2(64) taken as zero", 0, 1, 3, 40);
        s.z2_dead = 64;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(d.cpd.filt.coef[d.cpd.filt.lp1 + 64], 0);

        /* the convolutional encoder does not start B1u from zero (reading 59) */
        base(&s, "30 trellis encoder not at zero for B1u", 0, 1, 3, 40);
        s.conv_start = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "31 the same after a data frame of Ki = 0, with a rate renegotiation", 0, 0, 3, 40);
        s.conv_start = 6; s.b1u_lead = 1; s.a_reneg_ms = 5000; s.seconds = 12;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.state, V92D_DATA); CHECK_EQ_I(o.renegs, 1); CHECK_EQ_I((int)o.up_bad2, 0); CHECK(o.up_bits2 > 24000);

        /* all of it, with the limits the CX93001 gives in INFO1a */
        base(&s, "29 as the CX93001 on the FXS port", 0, 0, 5, 40);
        s.level = 1400; s.echo = 0.2; s.echo_tail = 0.57; s.echo_delay = 270; s.echo_search = 1;
        s.b1u_lead = 1; s.z2_dead = 64; s.ltot = 192; s.lmax = 128; s.uinfo = 79;
        s.conv_start = 9; s.ja_no_ones = 1; s.ntrn1 = 16368; s.jd_wait = 30000;
        s.seconds = 17;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
    }
    /* ---- with Phase 2 in front ---- */
    if (WANT(6)) {
        phase2(&p2, 1);
        CHECK_EQ_I(p2.d.state, P2D_DONE); CHECK_EQ_I(p2.a.state, P2A_DONE);
        CHECK_EQ_I(p2.d.mode, P2D_MODE_V92);
        CHECK_EQ_I(info1a_kind(&p2.d.info1a), INFO1A_PCM);
        base(&s, "21 after Phase 2 (V.92 caller)", 0, 1, 3, 40);
        s.uinfo = p2.d.info1a.uinfo;
        s.ltot = 192 + 64 * p2.d.info1a.ltot_code; s.lmax = 128 + 64 * p2.d.info1a.lmax_code;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(d.cpd.filt.lz2 <= s.lmax && d.cpd.filt.lz2 + d.cpd.filt.lp1 <= s.ltot);

        phase2(&p2, 0);                     /* a V.90 caller */
        CHECK_EQ_I(p2.d.state, P2D_DONE); CHECK_EQ_I(p2.a.state, P2A_DONE);
        CHECK_EQ_I(p2.d.mode, P2D_MODE_V90);
        v90_call(&p2);
    }
    /* ---- the caller's data mode off the level the design asked for ---- */
    if (WANT(9)) {
        /* Implementations differ on the power reference of 8.8.3 (V.92
         * servers have been seen to build for an upstream some 9 dB louder
         * than another reading): the caller's data signal arrives louder or
         * weaker than the design asked for. The digital modem measures the
         * gain on B1u and decodes on. */
        static const struct { double g; int alaw, shape; } c[] = {
            { 1.4, 0, 0 }, { 0.7, 1, 0 }, { 0.5, 0, 1 }, { 0.6, 1, 0 }, { 0.85, 0, 0 }, { 0.9, 1, 1 },
            { 1.1, 0, 0 }, { 1.15, 1, 0 }, { 1.25, 0, 1 }, { 1.7, 1, 0 }, { 2.0, 0, 0 }, { 2.4, 1, 0 },
            { 2.8, 0, 0 } };
        static char name[48];
        unsigned n;
        for (n = 0; n < sizeof c / sizeof c[0]; n++) {
            snprintf(name, sizeof name, "32 data mode at %.2f of the design", c[n].g);
            base(&s, name, c[n].alaw, c[n].shape, 3, n & 1 ? 40 : 0);
            s.gain_err = c[n].g;
            run(&s, &o); report(&s, &o); check_good(&s, &o);
        }
        /* with a data frame of Ki = 0 before B1u (reading 57): looked for
         * again when the decoder starts over */
        base(&s, "32b 0.6 of the design, Ki = 0 before B1u", 0, 1, 3, 40);
        s.gain_err = 0.6; s.b1u_lead = 1; s.conv_start = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(d.b1_leads, 1);
        base(&s, "32c 2.8 of the design, Ki = 0 before B1u", 1, 0, 3, 0);
        s.gain_err = 2.8; s.b1u_lead = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(d.b1_leads, 1);
        /* and on a noisy line, through a rate renegotiation (as 15b): the
         * gain measured again on the new B1u */
        base(&s, "33 1.4 of the design, renegotiates, noisy", 0, 0, 40, 40);
        s.phase0 = 0.4; s.gain_err = 1.4;
        s.a_reneg_ms = 5541; s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.renegs, 1);
        CHECK(o.up_bits2 > 2 * 24000); CHECK_EQ_I(o.up_bad2, 0);
        /* The limit: louder still, on a long line, the largest points the
         * precoder asks for pass the codec's full scale. B1u is right and
         * data flows; the clipped symbols are decided wrong. */
        base(&s, "34 2.4 of the design on a long line", 1, 1, 3, 40);
        s.gain_err = 2.4;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.state, V92D_DATA); CHECK_EQ_I(d.b1_bad, 0);
        CHECK(o.up_bits > 3 * 24000); CHECK(o.up_bad * 500 < o.up_bits);
    }
    /* ---- the gain measurement on its own: never a scale the power does
     * not allow ---- */
    if (WANT(10)) {
        /* Points at the odd multiples of 64; the received samples at the
         * odd multiples of 32. At scale 1 none is on a point; at scale 2
         * (G.711's octaves) every one is. With the power at what the design
         * expects, the scale stays 1; with it 6 dB under, it is 2. (The
         * symbols are small: these points do not widen as G.711 levels do,
         * so larger ones would want a finer search.) */
        static v92_map_t m;
        static v92_trn_t t;
        uint64_t ms = 0;
        int32_t k0 = 1000, c1, c2;
        int n;
        memset(&m, 0, sizeof m); memset(&t, 0, sizeof t);
        m.lc[0] = 40;
        for (n = 0; n < 40; n++) m.pt[0][n] = (uint16_t)(64 * (2 * n + 1));
        for (n = 0; n < V92D_UP_EST; n++) {
            int16_t v = (int16_t)(32 * (2 * (n * 3 % 8) + 1) * (n % 3 ? 1 : -1));
            t.rb[(k0 + n) & 255] = v;
            ms += (uint64_t)((int32_t)v * v);
        }
        ms /= V92D_UP_EST;
        c1 = v92d_up_scale(&m, &t, k0, (uint32_t)ms);
        c2 = v92d_up_scale(&m, &t, k0, (uint32_t)(4 * ms));
        printf("  35 symbols on the points at scale 2 only: scale %.3f with the power as designed, %.3f with it 6 dB under\n",
               c1 / 16384.0, c2 / 16384.0);
        CHECK_EQ_I(c1, 16384);
        CHECK(c2 > 32768 - 32768 / 20 && c2 < 32768 + 32768 / 20);
    }
    T_REPORT("v92call");
}
