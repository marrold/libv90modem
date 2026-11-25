/* Test harness for the V.90 analogue modem: a line between it and a digital
 * modem of the library's.
 *
 *   downstream: the digital modem's octets, pulled on the network clock by
 *     sim/dschan (pad, robbed bit, codec and loop filters, noise, our A/D on
 *     its own clock, the hybrid echo of what we send);
 *   upstream: what we send, on our clock, resampled on to the network clock
 *     (windowed sinc), a flat loss, white noise, the network codec (G.711)
 *     -> the digital modem.
 *
 * The digital modem is either the whole answerer (answer_t: V.8 and all) or
 * its V.90 parts alone (p2d, then p34d as answer.c starts it, or with V.92
 * offered v92/v92_p34d when INFO1a selects PCM upstream), for tests of the
 * analogue modem's own sequencer that leave V.8 out. The analogue modem is
 * orig_t or v90a_t.
 *
 * up_line: the upstream also passes a line and codec response (sampled on
 * the network clock): the band limit a PCM upstream needs at the far codec
 * (G.712), which the V.34 upstream of C5 did without. 1: the one the
 * answerer's own V.92 tests use (sim/v92chan: codec filters, transformer,
 * the short line or the long one); 2: the downstream's loop and codec
 * filters (sim/dschan), whose low frequency tail outlasts the digital
 * modem's 12 ms channel estimate (docs/history/PROGRESS.md, C6-2). And our clock is carried on to the network's
 * by a 320-tap resampler (sim/resamp's lresamp): the 48 taps of C5 change
 * the line near 4 kHz with the phase between the clocks, where a PCM
 * upstream has its share of power. Off, the upstream is as in C5. Data both ways is a 2^23 - 1 sequence (O.151), checked
 * by the self-synchronising rule b(n) = b(n - 18) XOR b(n - 23), so that a
 * check needs no alignment: each wrong bit fails it three times.
 * Host only. */
#ifndef V90D_TEST_V90LINK_H
#define V90D_TEST_V90LINK_H
#include <string.h>
#include <math.h>
#include "../modem/answer.h"
#include "../modem/orig.h"
#include "../modem/v90a.h"
#include "../modem/p2d.h"
#include "../modem/p34d.h"
#include "../v92/v92_p34d.h"
#include "../modem/pcmenc.h"
#include "../v90/v90_pcm.h"
#include "../sim/dschan.h"
#include "../sim/v92chan.h"
#include "../sim/resamp.h"
#include "../sim/noise.h"

/* ---- 2^23 - 1 data and its check ---- */
typedef struct { uint32_t gen; uint32_t chk; int64_t sent, got, fails; int armed; } bert_t;
static void bert_init(bert_t *b, uint32_t seed) { memset(b, 0, sizeof *b); b->gen = seed | 1; }
static int bert_get(bert_t *b)
{
    int bit = (int)(((b->gen >> 17) ^ (b->gen >> 22)) & 1);
    b->gen = ((b->gen << 1) | (uint32_t)bit) & 0x7fffff;
    b->sent++;
    return bit;
}
static void bert_put(bert_t *b, int bit)
{
    int want = (int)(((b->chk >> 17) ^ (b->chk >> 22)) & 1);
    b->chk = ((b->chk << 1) | (uint32_t)(bit & 1)) & 0x7fffff;
    if (++b->got > 23 && b->armed && want != (bit & 1)) b->fails++;
}

/* ---- or 16-bit counter words (test/wctr.h; vl_t.words) ---- */
#include "wctr.h"

/* ---- the line ---- */
typedef struct {
    /* downstream (dschan) */
    double loop_km, noise_dbm0, ppm, phase, pad_db, echo_db;
    int rbs, rbs_phase, alaw;
    /* upstream */
    double up_loss_db, up_noise_dbm0;
    int up_line;                    /* 1: the answerer's own V.92 upstream line (sim/v92chan's pulse,
                                       the short line to 2 km, the long one beyond); 2: the loop and
                                       codec filters of the downstream (see above) */
    /* the digital modem */
    int full;                       /* 1: answer_t, 0: p2d + p34d */
    int max_down, max_up;           /* its ceilings */
    int v92;                        /* it offers V.92 (PCM upstream) */
    int d_retrain;                  /* p2d + p34d / v92d: a retrain of either end restarts the digital
                                       modem as answer.c does (70 ms of silence, Phase 2 again) */
    /* an impulse added to one A/D sample of ours (a click): at imp_ms, imp_amp */
    int imp_ms, imp_amp;
    int trn1d_ms;                   /* p2d + p34d: its TRN1d lasts this long, Jd at its end (9.3.1.4
                                       allows 4000 ms: a live server's habit); 0: p34d's own */
    /* the analogue modem */
    int caller_orig;                /* 1: orig_t, 0: v90a_t */
    int caller_level;
    int caller_v92;                 /* it offers V.92 */
    uint32_t seed;
} vl_cfg;

#define VL_FIFO 64
#define VL_UPN  DSCH_LEN            /* the upstream line's taps */
#define VL_PREFILL 8                /* samples in the FIFO before it is read: the two clocks may slip apart */

typedef struct {
    vl_cfg cfg;
    dschan_t ch;
    sresamp_t rs;
    lresamp_t lrs;                  /* (up_line: the long window, see sim/resamp.h) */
    double fifo[VL_FIFO]; int fh, ft, primed;
    double uph[VL_UPN], uplh[VL_UPN]; int upos;  /* up_line: taps, history */
    rng_t rng;
    /* digital */
    answer_t ans;
    p2d_t p2; p34d_t p34; int dstate;  /* 0 Phase 2, 1 p34d, 2 dead, 3 v92d */
    v92d_t v92d;
    int dmute;                      /* the digital modem's output silenced (a test's doing) */
    int d_rt_wait;                  /* d_retrain: samples of silence before its Phase 2 */
    int d_v92_data, d_pcm_up;       /* d_retrain: v92d reached data mode; PCM upstream offered */
    int d_retrains;                 /* d_retrain: retrains of the digital modem */
    int d_rt_recent;                /* ... in a row, as answer.c's rt_recent: past 3 it gives up (dead);
                                       a minute of data mode forgets one */
    int64_t d_rt_clock;             /* samples of data mode toward that minute */
    int64_t clips;                  /* our A/D samples at full scale (+-32767) */
    /* a digital modem of the test's own (tools/rec_orig: the answerer through the
       public API): when set, it takes each network sample's codeword in place of
       the ones above and gives the codeword it sends */
    uint8_t (*dig)(void *u, uint8_t in);
    void *dig_user;
    /* analogue */
    orig_t org;
    v90a_t va;
    int16_t last_tx, last_rx;       /* what we sent and received in the last tick */
    int64_t k;                      /* our samples */
    int64_t n;                      /* network samples */
    bert_t down, up;
    int words;                      /* the data both ways: counter words (wctr_t), not 2^23 - 1 */
    wctr_t wdown, wup;
    double up_gain, up_nrms;
} vl_t;

static void vl_put_down(void *u, int bit)
{
    vl_t *l = (vl_t *)u;
    if (l->words) wctr_put(&l->wdown, bit); else bert_put(&l->down, bit);
}
static int  vl_get_up(void *u)
{
    vl_t *l = (vl_t *)u;
    return l->words ? wctr_get(&l->wup) : bert_get(&l->up);
}
static int  vl_get_down(void *u)
{
    vl_t *l = (vl_t *)u;
    return l->words ? wctr_get(&l->wdown) : bert_get(&l->down);
}
static void vl_put_up(void *u, int bit)
{
    vl_t *l = (vl_t *)u;
    if (l->words) wctr_put(&l->wup, bit); else bert_put(&l->up, bit);
}

/* the digital modem's V.90 parts alone: p2d, then p34d as answer.c's start_p34 */
static void vl_start_p34(vl_t *l)
{
    p34d_cfg c;
    int sr = l->p2.info1a.up_sr;
    memset(&c, 0, sizeof c);
    c.alaw = l->cfg.alaw;
    c.uinfo = l->p2.info1a.uinfo; c.md_len = l->p2.info1a.md_len; c.rtd = l->p2.rtd;
    c.echo_delay = -1;
    c.max_up_rate = l->cfg.max_up;
    c.getbit = vl_get_down; c.putbit = vl_put_up; c.user = l;
    if (sr == 3) { c.sym_rate = 3000; c.car_num = 3; c.car_den = 5; }
    else { c.sym_rate = 3200; c.car_num = 4; c.car_den = 7; }
    if (l->cfg.max_down > 0) {
        int r; uint32_t m = 0;
        for (r = 0; r < V90_NUM_RATES; r++) if ((int32_t)(r + 21) * 8000 / 6 <= l->cfg.max_down) m |= 1u << r;
        c.down_rates = m ? m : 1;
    }
    if (p34d_init(&l->p34, &c) != 0) l->dstate = 2; else l->dstate = 1;
}

/* ... or v92d (answer.c's start_v92) */
static void vl_start_v92(vl_t *l)
{
    v92d_cfg c;
    const info1a_t *i = &l->p2.info1a;
    int k; uint32_t r = 15770;
    memset(&c, 0, sizeof c);
    c.alaw = l->cfg.alaw;
    c.uinfo = i->uinfo; c.md_len = i->md_len; c.rtd = l->p2.rtd;
    c.sections = i->sections;
    c.ltot = 192 + 64 * i->ltot_code; c.lmax = 128 + 64 * i->lmax_code;
    c.echo_delay = -1;
    c.max_up_rate = l->cfg.max_up;
    if (l->cfg.max_down > 0) {
        uint32_t m = 0;
        for (k = 0; k < V90_NUM_RATES; k++) if ((int32_t)(k + 21) * 8000 / 6 <= l->cfg.max_down) m |= 1u << k;
        c.down_rates = m ? m : 1;
    }
    for (k = 0; k <= ANS_MAX_TX_CODE; k++) r = r * 967 / 1024;
    c.down_rms = (int)r;
    c.dsn_pace = 2;
    c.getbit = vl_get_down; c.putbit = vl_put_up; c.user = l;
    l->d_v92_data = 0;              /* (answer.c's start_v92: this start-up's own) */
    l->dstate = v92d_init(&l->v92d, &c) != 0 ? 2 : 3;
}

/* one network sample: the upstream as the network codec has it, in; the
 * digital modem's octet, out */
static uint8_t vl_src(void *ctx)
{
    vl_t *l = (vl_t *)ctx;
    double up = 0;
    uint8_t in, out;
    int alaw = l->cfg.alaw;
    if (!l->primed && (l->fh - l->ft + VL_FIFO) % VL_FIFO >= VL_PREFILL) l->primed = 1;
    if (l->primed && l->fh != l->ft) { up = l->fifo[l->ft]; l->ft = (l->ft + 1) % VL_FIFO; }
    if (l->cfg.up_line) {
        double a = 0;
        int k;
        l->uplh[l->upos] = up;
        for (k = 0; k < VL_UPN; k++) a += l->uph[k] * l->uplh[(l->upos - k + VL_UPN) % VL_UPN];
        l->upos = (l->upos + 1) % VL_UPN;
        up = a;
    }
    up = up * l->up_gain + l->up_nrms * rng_gauss(&l->rng);
    in = pcm_encode(alaw, (int16_t)(up > 32767 ? 32767 : up < -32768 ? -32768 : floor(up + 0.5)));
    l->n++;
    if (l->dig) out = l->dig(l->dig_user, in);
    else if (l->cfg.full) out = answer_tick_pcm(&l->ans, in);
    else {
        int16_t x = (int16_t)v90_octet_linear(alaw, in);
        if (l->dstate == 0) {
            out = pcm_encode(alaw, p2d_tick(&l->p2, x));
            if (l->p2.state == P2D_DONE) { if (l->p2.mode == P2D_MODE_V92) vl_start_v92(l); else vl_start_p34(l); }
            else if (l->p2.state == P2D_FAILED) l->dstate = 2;
        } else if (l->dstate == 1) {
            out = p34d_tick(&l->p34, x);
            if (l->cfg.trn1d_ms && l->p34.state == P34_SD && v90tx_signal(&l->p34.tx) == V90TX_TRN1D) {
                /* p34d moves on to Jd when its count of TRN1d symbols is
                 * up: held short of that (past its canceller's training)
                 * until trn1d_ms, then run out */
                if ((int32_t)(l->p34.now - l->p34.t_trn1d) < l->cfg.trn1d_ms * 8) {
                    if (l->p34.tx.nsym >= 6000) l->p34.tx.nsym = 5999;
                } else l->p34.tx.nsym = 1u << 24;
            }
            if (l->p34.state == P34_DATA && ++l->d_rt_clock >= 8 * 60000) { l->d_rt_clock = 0; if (l->d_rt_recent > 0) l->d_rt_recent--; }
            if (l->cfg.d_retrain && l->p34.state == P34_RETRAIN) {
                if (l->d_rt_recent >= 3) l->dstate = 2;         /* answer.c: MAX_RETRAINS, no line */
                else { l->dstate = 4; l->d_rt_wait = 0; l->d_retrains++; l->d_rt_recent++; }
            }
        } else if (l->dstate == 3) {
            out = v92d_tick(&l->v92d, x);
            if (l->v92d.state == V92D_DATA) {
                l->d_v92_data = 1;
                if (++l->d_rt_clock >= 8 * 60000) { l->d_rt_clock = 0; if (l->d_rt_recent > 0) l->d_rt_recent--; }
            }
            if (l->cfg.d_retrain && (l->v92d.state == V92D_RETRAIN
                                     || (l->v92d.state == V92D_FAILED && l->v92d.error != V92D_ERR_CLEARDOWN
                                         && l->v92d.error != V92D_ERR_HANGUP && l->v92d.error != V92D_ERR_CARRIER))) {
                /* as answer.c: PCM upstream is offered again unless the
                 * digital modem never reached data mode, failed, or its
                 * upstream did not hold the lowest rate */
                if (l->v92d.state == V92D_FAILED || !l->d_v92_data || l->v92d.up_floor) l->d_pcm_up = 0;
                if (l->d_rt_recent >= 3) l->dstate = 2;         /* answer.c: MAX_RETRAINS, no line */
                else { l->dstate = 4; l->d_rt_wait = 0; l->d_retrains++; l->d_rt_recent++; }
            }
        } else if (l->dstate == 4) {
            /* 9.5.1 / 9.7.1: 70 ms of silence, then Phase 2 of a retrain */
            out = v90_pcm_octet(alaw, 0, 1);
            if (++l->d_rt_wait >= 8 * 70) {
                p2d_cfg pc = l->p2.cfg;
                p2d_init_retrain(&l->p2, &pc);
                if (l->cfg.v92) p2d_retrain_v92(&l->p2, l->d_pcm_up);
                l->dstate = 0;
            }
        } else out = v90_pcm_octet(alaw, 0, 1);
    }
    if (l->dmute) out = v90_pcm_octet(alaw, 0, 1);
    return out;
}

static void vl_init(vl_t *l, const vl_cfg *c)
{
    dschan_cfg dc;
    memset(l, 0, sizeof *l);
    l->cfg = *c;
    l->d_pcm_up = 1;
    rng_seed(&l->rng, c->seed + 101);
    bert_init(&l->down, 0x1234 + c->seed); bert_init(&l->up, 0x4321 + c->seed);
    l->down.armed = l->up.armed = 1;
    memset(&dc, 0, sizeof dc);
    dc.alaw = c->alaw; dc.pad_db = c->pad_db; dc.rbs = c->rbs; dc.rbs_phase = c->rbs_phase;
    dc.loop_km = c->loop_km; dc.level_db = -6; dc.noise_dbm0 = c->noise_dbm0; dc.ppm = c->ppm;
    dc.phase = c->phase; dc.echo_db = c->echo_db; dc.seed = c->seed;
    dschan_init(&l->ch, &dc, vl_src, l);
    if (c->up_line) {
        /* the response on the network clock, unit gain at 1 kHz */
        static v92chan_t vc;
        double re = 0, im = 0, g;
        int k;
        if (c->up_line == 1) {
            v92chan_cfg cc;
            memset(&cc, 0, sizeof cc);
            cc.shape = c->loop_km > 2 ? 1 : 0; cc.level = 1000; cc.seed = 1;
            v92chan_init(&vc, &cc);
        }
        for (k = 0; k < VL_UPN; k++) {
            l->uph[k] = c->up_line == 1 ? v92chan_pulse(&vc, k + 0.37) : dschan_pulse(&l->ch, k - DSCH_PRE + 0.37);
            re += l->uph[k] * cos(2 * 3.14159265358979323846 * k / 8.0);
            im -= l->uph[k] * sin(2 * 3.14159265358979323846 * k / 8.0);
        }
        g = sqrt(re * re + im * im);
        for (k = 0; k < VL_UPN; k++) l->uph[k] /= g;
    }
    /* our samples, on the network clock: out / in = 1 / (1 + ppm) */
    sresamp_init(&l->rs, 1.0 / (1.0 + c->ppm * 1e-6));
    lresamp_init(&l->lrs, 1.0 / (1.0 + c->ppm * 1e-6));
    l->up_gain = pow(10, -c->up_loss_db / 20);
    l->up_nrms = 15770.0 * pow(10, c->up_noise_dbm0 / 20);
    if (c->full) {
        answer_cfg ac;
        p2d_cfg pc;
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_V90 | (c->v92 ? ANS_MODE_V92 : 0);
        ac.getbit = vl_get_down; ac.putbit = vl_put_up; ac.user = l;
        answer_init(&l->ans, &ac);
        pc = l->ans.p2cfg; pc.alaw = c->alaw;
        answer_v90_cfg(&l->ans, &pc);
        answer_v90_line(&l->ans, -1, c->max_down, c->max_up);
    } else {
        p2d_cfg pc;
        memset(&pc, 0, sizeof pc);
        pc.tx_dbm0 = -12; pc.max_tx_code = ANS_MAX_TX_CODE; pc.alaw = c->alaw; pc.up_sr_mask = (1 << 3) | (1 << 4);
        p2d_init(&l->p2, &pc);
        if (c->v92) p2d_offer_v92(&l->p2, 1);
    }
    if (c->caller_orig) {
        orig_cfg oc;
        memset(&oc, 0, sizeof oc);
        oc.level = c->caller_level ? c->caller_level : 3000;
        oc.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21 | ORIG_MODE_V90 | (c->caller_v92 ? ORIG_MODE_V92 : 0);
        oc.echo_delay = 0; oc.law = 0;
        oc.getbit = vl_get_up; oc.putbit = vl_put_down; oc.user = l;
        orig_init(&l->org, &oc);
    } else {
        v90a_cfg vc;
        memset(&vc, 0, sizeof vc);
        vc.level = c->caller_level ? c->caller_level : 3000;
        vc.v92 = c->caller_v92;
        vc.getbit = vl_get_up; vc.putbit = vl_put_down; vc.user = l;
        v90a_init(&l->va, &vc);
    }
}

/* one of our samples */
static void vl_tick(vl_t *l)
{
    int16_t rx = dschan_tick(&l->ch, l->last_tx), tx;
    double o[4];
    int i, n;
    if (l->cfg.imp_amp && l->k == (int64_t)l->cfg.imp_ms * 8) {
        int32_t v = rx + l->cfg.imp_amp;
        rx = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
    l->last_rx = rx;
    if (rx >= 32767 || rx <= -32767) l->clips++;
    tx = l->cfg.caller_orig ? orig_tick(&l->org, rx) : v90a_tick(&l->va, rx);
    n = l->cfg.up_line ? lresamp_push(&l->lrs, tx, o, 4) : sresamp_push(&l->rs, tx, o, 4);
    for (i = 0; i < n; i++) {
        int h = (l->fh + 1) % VL_FIFO;
        if (h != l->ft) { l->fifo[l->fh] = o[i]; l->fh = h; }
    }
    l->last_tx = tx;
    l->k++;
}

#endif
