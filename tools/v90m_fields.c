/* v90m-fields: what the calling modem of a recorded call sent, field by
 * field and phase by phase, as the library's answering side decodes it.
 *
 *   v90m-fields [-a] [-9] [-u BPS] [-d BPS] [-l DBM0] [-e SAMPLES] [-P CODE] RECORDING
 *
 * The options and the recording format are v90m-replay's (tools/README.md):
 * the answering modem is set up as the recording modem was and run over the
 * recording the same way. What it decoded is read out of it after each
 * sample; next to it, over every sample the caller sent from the start of
 * the recording, the library's own receivers run on their own:
 *
 *   - V.21 channel 1 (980 / 1180 Hz) start-stop characters, as the V.8
 *     receiver hears them (CI, CM, CJ, and V.21 data);
 *   - the Phase 2 DPSK receiver on 2400 Hz (Tone A: a V.90 / V.92 analogue
 *     caller) and on 1200 Hz (Tone B: a V.34 caller): every CRC-clean INFO
 *     frame (INFO0, INFO1a, INFO1c) and every phase reversal of the tone.
 *
 * Every line is "time TAG fields", time in seconds from the answer tone. The
 * same tool reads a recording of the library's own caller (tools/rec_orig),
 * so that the two can be compared line by line (test/realcheck/).
 * Tool, not signal path: it may use double. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "v90modem_priv.h"
#include "v90_pcm.h"
#include "../modem/v21.h"
#include "../modem/async.h"
#include "../modem/dpsk.h"
#include "../modem/info.h"
#include "../v92/v92_seq.h"
#include "../v90/v90_seq.h"
#include "../modem/v34mp.h"
#include "../modem/v8msg.h"

typedef struct { const uint8_t *d; long n, bs, txo; int lin; } rec_t;
static uint8_t rec_sent(const rec_t *r, long i) { return r->d[(i / 80) * r->bs + r->txo + i % 80]; }
static int16_t rec_heard(const rec_t *r, long i, int alaw)
{
    if (r->lin) {
        const uint8_t *q = r->d + (i / 80) * r->bs + 2 * (i % 80);
        return (int16_t)(uint16_t)(q[0] | (q[1] << 8));
    }
    return (int16_t)v90_octet_linear(alaw, r->d[(i / 80) * r->bs + i % 80]);
}
static int silent(int alaw, uint8_t c) { return alaw ? (c & 0x7F) == 0x55 : (c & 0x7F) == 0x7F; }

static long recorded_answer(const rec_t *r, int alaw)
{
    long i, run = 0;
    for (i = 0; i < r->n; i++) {
        if (silent(alaw, rec_sent(r, i))) run++;
        else { if (run >= 1590 && run <= 1612 && i > 8000) return i; run = 0; }
    }
    return -1;
}

static int level_rms(int dbm0)
{
    int32_t v = 15770;
    if (dbm0 > 0) dbm0 = 0;
    for (; dbm0 < 0; dbm0++) v = (v * 29205 + 16384) >> 15;
    return (int)v;
}

static int opt_alaw, opt_v92, opt_dbm0 = -12, opt_up, opt_down = 56000, opt_echo = -1, opt_code;
static int ones(void *u) { (void)u; return 1; }
static void drop(void *u, int b) { (void)u; (void)b; }

static long ans0;                       /* the answer tone, samples into the recording */
static int cur_phase = -1;              /* the answerer's phase (ans_phase), -1 before it starts */
static double T(long i) { return (double)(i - ans0) / 8000.0; }
static double dbm0(double ms) { return 10 * log10(ms / (15770.0 * 15770.0) + 1e-12); }

static void bits(const char *tag, const uint8_t *b, int n)
{
    int k;
    printf("%s", tag);
    for (k = 0; k < n; k++) { if (k && k % 8 == 0) putchar('_'); putchar('0' + (b[k] & 1)); }
    putchar('\n');
}

/* What our own transmitters make of the fields just decoded, against the
 * bits that came in: the caller's builders (the ones modem/p2call,
 * modem/v90a and modem/v34dx call) given what the parser read. */
static void rebuild(long i, const char *tag, const uint8_t *in, int n, const uint8_t *out, int m)
{
    int k, nd = 0;
    printf("%8.3f RAW %s %d bits: ", T(i), tag, n);
    for (k = 0; k < n; k++) putchar('0' + (in[k] & 1));
    printf("\n%8.3f REBUILD %s", T(i), tag);
    if (m != n) printf(" length %d (in %d)", m, n);
    for (k = 0; k < n && k < m; k++) if ((in[k] & 1) != (out[k] & 1)) { if (nd < 40) printf(" %s%d", nd ? "" : "differs at bits ", k); nd++; }
    printf(nd || m != n ? (nd ? " (%d bits)\n" : "\n") : " same\n", nd);
}

/* ---- the free-running receivers ---- */
#define SQUELCH_AMP 110
static v21demod_t v21d; static arx_t v21u; static int64_t v21sq;
static uint8_t v21seq[64]; static int v21n; static long v21at;
static void v21_flush(void)
{
    int k;
    if (!v21n) return;
    printf("%8.3f V21L chars %d:", T(v21at), v21n);
    for (k = 0; k < v21n; k++) printf(" %02x", v21seq[k]);
    printf("\n");
    v21n = 0;
}
static void v21_tick(long i, int16_t x)
{
    int level = v21demod_tick(&v21d, x), c;
    if (v21d.energy < v21sq) level = 1;
    c = arx_tick(&v21u, level);
    if (!(cur_phase < 0 || cur_phase == ANS_V8 || cur_phase == ANS_V21 || cur_phase == ANS_V22)) { v21n = 0; return; }
    if (c == ARX_NONE) { if (v21n && arx_idle_bits(&v21u) >= 8) v21_flush(); return; }
    if (c == ARX_FRAMEERR) { v21_flush(); return; }
    if (arx_pre_bits(&v21u) >= 8) v21_flush();
    if (!v21n) v21at = i;
    if (v21n < 64) v21seq[v21n++] = (uint8_t)c;
}

/* DPSK: two carriers, three frame lengths each */
static const int plen[3] = { INFO_PAYLOAD(INFO0A_BITS), INFO_PAYLOAD(INFO1A_BITS), INFO_PAYLOAD(INFO1C_BITS) };
static dpskrx_t drx[2][3];
static long last_rev[2] = { -1, -1 };
/* 0: carrier A (2400 Hz) is the caller's (V.90 / V.92), 1: B (1200 Hz, V.34);
 * -1: not known yet (V.8). The other carrier is the echo of ours. */
static int caller_car(void)
{
    if (cur_phase == ANS_P2 || cur_phase == ANS_P3 || cur_phase == ANS_P3X || cur_phase == ANS_V92 || cur_phase == ANS_V90) return 0;
    if (cur_phase == ANS_V34) return 1;
    return -1;
}
static void info_print(long i, int car, int kind, const uint8_t *f)
{
    const char *cn = car ? "B" : "A";
    int n = plen[kind];
    printf("%8.3f INFO%s len %d level %.1f dBm0 ", T(i), cn, n + 16, dbm0((double)dpskrx_level(&drx[car][kind]) * dpskrx_level(&drx[car][kind])));
    bits("bits ", f, n);
    {
        static uint8_t fr[INFO_MAX_BITS + 8];
        int m;
        if (kind == 0) { info0_t q; info0a_parse(f, &q); m = info0a_build(&q, fr); }
        else if (kind == 1) { info1a_t q; info1a_parse(f, &q); m = info1a_build(&q, fr); }
        else { info1c_t q; info1c_parse(f, &q); m = info1c_build(&q, fr); }
        rebuild(i, kind == 0 ? "INFO0" : kind == 1 ? "INFO1a" : "INFO1c", f, n, fr + INFO_HDR_BITS, m - INFO_HDR_BITS - 4);
    }
    if (kind == 0) {
        info0_t q; info0a_parse(f, &q);
        printf("%8.3f   INFO0%s: 2743 %d 2800 %d 3429 %d lo3000 %d hi3000 %d lo3200 %d hi3200 %d allow3429 %d pwr_reduce %d max_sr_diff %d cme %d 1664 %d bit26 %d bit27 %d ack %d\n",
               T(i), car ? "c" : "a", q.sr2743, q.sr2800, q.sr3429, q.lo3000, q.hi3000, q.lo3200, q.hi3200, q.allow3429, q.pwr_reduce,
               q.max_sr_diff, q.cme, q.c1664, f[14], f[15], q.ack);
    } else if (kind == 1) {
        info1a_t q; info1a_parse(f, &q);
        printf("%8.3f   INFO1a: kind %d bits12:17 %d%d%d%d%d%d md %d uinfo %d bits32:33 %d%d up_sr(34:36) %d bits37:39 %d freq_off(40:49) %d (raw 0x%03x)\n",
               T(i), info1a_kind(&q), f[0], f[1], f[2], f[3], f[4], f[5], q.md_len, q.uinfo, f[20], f[21],
               f[22] | f[23] << 1 | f[24] << 2, f[25] | f[26] << 1 | f[27] << 2, q.freq_off, (unsigned)q.freq_off & 0x3ff);
        if (!q.v90)
            printf("%8.3f   INFO1a (V.34): min_pwr_red %d add_pwr_red %d md %d hi_carrier %d preemph %d max_rate %d up_sr %d down_sr %d\n",
                   T(i), q.min_pwr_red, q.add_pwr_red, q.md_len, q.hi_carrier, q.preemph, q.max_rate, q.up_sr, q.down_sr);
    } else {
        info1c_t q; int k; info1c_parse(f, &q);
        printf("%8.3f   INFO1c: min_pwr_red %d add_pwr_red %d md %d", T(i), q.min_pwr_red, q.add_pwr_red, q.md_len);
        for (k = 0; k < 6; k++) printf(" sr%d(%d,%d,%d)", k, q.sr[k].hi_carrier, q.sr[k].preemph, q.sr[k].max_rate);
        printf(" freq_off %d\n", q.freq_off);
    }
}
static void dpsk_tick(long i, int16_t x)
{
    int car, k;
    for (car = 0; car < 2; car++)
        for (k = 0; k < 3; k++) {
            int ev = dpskrx_tick(&drx[car][k], x);
            if (caller_car() != car) continue;
            if (ev & DPSK_EV_FRAME) info_print(i, car, k, drx[car][k].frame);
            if (k == 0 && (ev & DPSK_EV_REVERSAL)) {
                printf("%8.3f REV%s caller's tone reversal (%.1f ms after its last), tone level %.1f dBm0\n", T(i), car ? "B" : "A",
                       last_rev[car] < 0 ? 0.0 : (i - last_rev[car]) / 8.0,
                       dbm0((double)dpskrx_level(&drx[car][k]) * dpskrx_level(&drx[car][k])));
                last_rev[car] = i;
            }
        }
}

/* the caller's power while we are silent: per 10 ms, onsets and ends */
static double blk_ms; static int blk_n, blk_quiet; static int act = 0; static long act_at = -1; static double act_sum; static long act_n;
static void power_tick(long i, int16_t x, int we_silent)
{
    blk_ms += (double)x * x; blk_n++;
    if (!we_silent) blk_quiet = 0;
    if (blk_n < 80) return;
    {
        double ms = blk_ms / blk_n, lv = dbm0(ms);
        int on = lv > -48;
        if (blk_quiet) {
            if (on && !act) { act = 1; act_at = i - 79; act_sum = 0; act_n = 0; }
            if (!on && act) {
                printf("%8.3f POWER caller alone %.3f to %.3f s (%.0f ms): %.1f dBm0\n", T(act_at), T(act_at), T(i - 79),
                       (i - 79 - act_at) / 8.0, dbm0(act_sum / act_n));
                act = 0;
            }
            if (act) { act_sum += ms * blk_n; act_n += blk_n; }
        } else if (act) {
            printf("%8.3f POWER caller alone %.3f to %.3f s (%.0f ms, we began to send): %.1f dBm0\n", T(act_at), T(act_at), T(i - 79),
                   (i - 79 - act_at) / 8.0, dbm0(act_sum / act_n));
            act = 0;
        }
        blk_ms = 0; blk_n = 0; blk_quiet = 1;
    }
}

/* The caller's level over each state of the answerer's engines, and the
 * share of it that came while we were quiet (our transmit silent for 50 ms:
 * no echo of ours in it). Over the first 15..155 ms of a state and from 175
 * ms on (L1 and L2, where the state is the one that receives them), and the
 * 1800 Hz guard tone next to the 2400 Hz Tone A. */
static struct { int key; long from, n, quiet, n1, n2; double ms, ms1, ms2, re[2], im[2]; } seg = { -1, 0, 0, 0, 0, 0, 0, 0, 0, { 0, 0 }, { 0, 0 } };
static double ctab[2][40], stab[2][40];
static void seg_end(long i)
{
    if (seg.key >= 0 && seg.n >= 80) {
        double g18 = seg.re[0] * seg.re[0] + seg.im[0] * seg.im[0], g24 = seg.re[1] * seg.re[1] + seg.im[1] * seg.im[1];
        printf("%8.3f SEG %d/%d %.0f ms: %.1f dBm0, quiet %d %%; L1 part %.1f, L2 part %.1f dBm0; 1800/2400 Hz %.1f dB\n", T(seg.from), seg.key / 100, seg.key % 100,
               seg.n / 8.0, dbm0(seg.ms / seg.n), (int)(100 * seg.quiet / seg.n), seg.n1 ? dbm0(seg.ms1 / seg.n1) : -99.0,
               seg.n2 ? dbm0(seg.ms2 / seg.n2) : -99.0, 10 * log10((g18 + 1) / (g24 + 1)));
    }
    (void)i;
}
static void seg_tick(long i, int key, int16_t x, int quiet)
{
    long k;
    if (key != seg.key) { seg_end(i); memset(&seg, 0, sizeof seg); seg.key = key; seg.from = i; }
    k = seg.n++;
    seg.ms += (double)x * x;
    if (quiet) seg.quiet++;
    if (k >= 120 && k < 1240) { seg.ms1 += (double)x * x; seg.n1++; }
    if (k >= 1400) { seg.ms2 += (double)x * x; seg.n2++; }
    seg.re[0] += x * ctab[0][i % 40]; seg.im[0] -= x * stab[0][i % 40];
    seg.re[1] += x * ctab[1][i % 40]; seg.im[1] -= x * stab[1][i % 40];
}
static int seg_key(const answer_t *a)
{
    int sub = 0;
    switch (a->phase) {
    case ANS_V8: sub = (int)a->v8.state; break;
    case ANS_P2: sub = (int)a->p2.state; break;
    case ANS_P3: sub = (int)a->p34.state; break;
    case ANS_P3X: case ANS_V92: sub = (int)a->u.v92.state; break;
    case ANS_V34: sub = a->v34.state == V34A_PHASE2 ? (int)a->v34.p2.state : 50 + (int)a->v34.dx.state; break;
    default: break;
    }
    return (int)a->phase * 100 + sub;
}

/* ---- what the answering modem decoded ---- */
static const char *phn[] = { "V8", "V22", "V21", "FAILED", "P2", "P3", "V90", "RETRAIN", "V34", "V32", "V32_SIL", "P3X", "V92", "X2", "V8BIS", "K56", "K56RT", "V23" };
/* one name for each ans_phase (modem/answer.h), as in tools/trace.c */
typedef char phn_check[sizeof phn / sizeof *phn == ANS_NPHASES ? 1 : -1];
static void menu(long i, const char *n, const v8_menu_t *m, const uint8_t *oct, int noct)
{
    int k;
    printf("%8.3f %s call function %u, modes 0x%04x (%u octets), pcm %s%u, protocol %s%u, access %s%u; octets", T(i), n, m->call_function,
           m->modes, m->n_mod_octets, m->has_pcm ? "" : "absent ", m->pcm, m->has_prot ? "" : "absent ", m->prot,
           m->has_access ? "" : "absent ", m->access);
    for (k = 0; k < noct; k++) printf(" %02x", oct[k]);
    printf("\n");
}
static void dil(long i, const char *n, const v90_dil_t *d)
{
    int k;
    printf("%8.3f %s DIL N %d Lsp %d Ltp %d H", T(i), n, d->n, d->lsp, d->ltp);
    for (k = 0; k < 8; k++) printf(" %d", d->h[k]);
    printf(" REF");
    for (k = 0; k < 8; k++) printf(" %d", d->ref[k]);
    printf(" SP ");
    for (k = 0; k < d->lsp; k++) putchar('0' + ((d->sp[k >> 3] >> (k & 7)) & 1));
    printf(" TP ");
    for (k = 0; k < d->ltp; k++) putchar('0' + ((d->tp[k >> 3] >> (k & 7)) & 1));
    printf(" train");
    for (k = 0; k < d->n; k++) printf(" %d", d->train[k]);
    printf("\n");
}
static void cp(long i, const char *n, const v90_cp_t *q)
{
    int c, k;
    printf("%8.3f %s is_cp %d drn %d silence %d Sr %d ack %d alaw %d rates 0x%04x ld %d rms_ratio %u a1 %d a2 %d b1 %d b2 %d idx %d %d %d %d %d %d codec_differs %d\n",
           T(i), n, q->is_cp, q->drn, q->silence, q->sr, q->ack, q->alaw, q->rates, q->ld, q->rms_ratio, q->a1, q->a2, q->b1, q->b2,
           q->idx[0], q->idx[1], q->idx[2], q->idx[3], q->idx[4], q->idx[5], q->codec_differs);
    for (c = 0; c < 6; c++) {
        int m = 0;
        for (k = 0; k < 128; k++) m += (q->mask[c][k >> 3] >> (k & 7)) & 1;
        if (!m) continue;
        printf("%8.3f %s   constellation %d: %d points:", T(i), n, c, m);
        for (k = 0; k < 128; k++) if ((q->mask[c][k >> 3] >> (k & 7)) & 1) printf(" %d", k);
        printf("\n");
    }
}
static void cpu(long i, const char *n, const v92_cpu_t *q)
{
    int c, k;
    printf("%8.3f %s type %d drn %d Sr %d ack %d alaw %d ld %d rms_ratio %u a1 %d a2 %d b1 %d b2 %d idx %d %d %d %d %d %d codec_differs %d\n",
           T(i), n, q->type, q->drn, q->sr, q->ack, q->alaw, q->ld, q->rms_ratio, q->a1, q->a2, q->b1, q->b2,
           q->idx[0], q->idx[1], q->idx[2], q->idx[3], q->idx[4], q->idx[5], q->codec_differs);
    for (c = 0; c < 6; c++) {
        int m = 0;
        for (k = 0; k < 128; k++) m += (q->mask[c][k >> 3] >> (k & 7)) & 1;
        if (!m) continue;
        printf("%8.3f %s   constellation %d: %d points:", T(i), n, c, m);
        for (k = 0; k < 128; k++) if ((q->mask[c][k >> 3] >> (k & 7)) & 1) printf(" %d", k);
        printf("\n");
    }
}

static struct {
    int ph, v8, p2, p34, v34, v92, dx, got0a, got_cpt, got_ja, got_cpt92, got_cpu, got_suvu, mp, p2n, cpu_ack;
    v90_cp_t cp; v92_cpu_t cpx; info0_t info0c; int have0c;
} L;

static void watch(answer_t *a, long i)
{
    if ((int)a->phase != L.ph) { L.ph = (int)a->phase; printf("%8.3f PHASE %s\n", T(i), phn[L.ph]); L.p2 = L.p34 = L.v92 = L.p2n = L.dx = -1; }
    if (a->phase == ANS_V8 && (int)a->v8.state != L.v8) {
        L.v8 = (int)a->v8.state;
        printf("%8.3f V8 state %d\n", T(i), L.v8);
        if (L.v8 == V8A_JM) {
            uint8_t o[V8_MAX_OCTETS], bi[8 * V8_MAX_OCTETS], bo[8 * V8_MAX_OCTETS]; int no, k, b;
            menu(i, "CM", &a->v8.cm, a->v8.rx.msg, a->v8.rx.nmsg); menu(i, "JM(ours)", &a->v8.jm, NULL, 0);
            no = v8_encode(&a->v8.cm, o, V8_MAX_OCTETS);
            for (k = 0; k < a->v8.rx.nmsg; k++) for (b = 0; b < 8; b++) bi[8 * k + b] = (uint8_t)((a->v8.rx.msg[k] >> b) & 1);
            for (k = 0; k < no; k++) for (b = 0; b < 8; b++) bo[8 * k + b] = (uint8_t)((o[k] >> b) & 1);
            rebuild(i, "CM", bi, 8 * a->v8.rx.nmsg, bo, 8 * no);
        }
        if (L.v8 == V8A_DONE) printf("%8.3f V8 result %d selected %d\n", T(i), a->v8.result, a->v8.selected);
    }
    if (a->phase == ANS_P2) {
        const p2d_t *p = &a->p2;
        if ((int)p->state != L.p2) {
            L.p2 = (int)p->state;
            printf("%8.3f P2 state %d rtd %d probe_from %.3f l2_from %.3f\n", T(i), L.p2, (int)p->rtd,
                   p->probe_from ? T(i) - (p->now - p->probe_from) / 8000.0 : -1.0, p->l2_from ? T(i) - (p->now - p->l2_from) / 8000.0 : -1.0);
            if (L.p2 == P2D_WAIT_REV3) {
                int k;
                printf("%8.3f P2 probe rx_level(0.1dB)", T(i));
                for (k = 1; k <= PROBE_NBIN; k++) printf(" %d", p->rx_level[k]);
                printf("\n%8.3f P2 probe snr(0.1dB)", T(i));
                for (k = 1; k <= PROBE_NBIN; k++) printf(" %d", p->snr[k]);
                printf("\n");
            }
            if (L.p2 == P2D_WAIT_INFO1A) {
                printf("%8.3f P2 INFO0a as taken: v92 %d short_p2 %d\n", T(i), p->info0a.v92, p->info0a.short_p2);
                printf("%8.3f P2 our INFO1d: min_pwr_red %d add_pwr_red %d md %d sr3 (%d,%d,%d) sr4 (%d,%d,%d) pcm_up %d\n", T(i),
                       p->info1d.min_pwr_red, p->info1d.add_pwr_red, p->info1d.md_len, p->info1d.sr[3].hi_carrier, p->info1d.sr[3].preemph,
                       p->info1d.sr[3].max_rate, p->info1d.sr[4].hi_carrier, p->info1d.sr[4].preemph, p->info1d.sr[4].max_rate, p->pcm_up);
            }
        }
    }
    if (a->phase == ANS_P3) {
        const p34d_t *p = &a->p34;
        if ((int)p->state != L.p34) {
            L.p34 = (int)p->state;
            printf("%8.3f P34 state %d error %d\n", T(i), L.p34, p->error);
            if (L.p34 == P34_SD) {
                static uint8_t o[V90_DIL_MAX_BITS + 16]; v90_dil_t d; int n = v90_dil_parse(p->fbuf, P34_FBUF, &d);
                dil(i, "JA", &p->dil);
                if (n > 0) rebuild(i, "JA", p->fbuf, n, o, v90_dil_build(&d, o));
            }
        }
        if ((p->got_cpt && !L.got_cpt) || (p->cp.is_cp && memcmp(&L.cp, &p->cp, sizeof L.cp))) {
            static uint8_t o[V90_CP_MAX_BITS + 16]; v90_cp_t q; int n = v90_cp_parse(p->fbuf, P34_FBUF, &q);
            if (!L.got_cpt) { L.got_cpt = 1; cp(i, "CPT", &p->cp); } else { L.cp = p->cp; cp(i, "CP", &p->cp); }
            if (n > 0) rebuild(i, q.is_cp ? "CP" : "CPT", p->fbuf, n, o, v90_cp_build(&q, o));
        }
    }
    if (a->phase == ANS_P3X || a->phase == ANS_V92) {
        const v92d_t *m = &a->u.v92;
        if ((int)m->state != L.v92) { L.v92 = (int)m->state; printf("%8.3f V92 state %d error %d\n", T(i), L.v92, m->error); }
        if (m->got_ja && !L.got_ja) {
            static uint8_t o[V92_JA_MAX_BITS + 16]; v92_ja_t q; int n = v92_ja_parse(m->fbuf, V92D_FBUF, &q);
            L.got_ja = 1; dil(i, "JA92", &m->ja.dil); printf("%8.3f JA92 rates 0x%05lx\n", T(i), (unsigned long)m->ja.rates);
            if (n > 0) rebuild(i, "JA92", m->fbuf, n, o, v92_ja_build(&q, o));
        }
        if (m->got_cpt && !L.got_cpt92) {
            static uint8_t o[V92_CPU_MAX_BITS + 16]; v92_cpu_t q; int n = v92_cpu_parse(m->fbuf, V92D_FBUF, 12, &q);
            L.got_cpt92 = 1; cpu(i, "CPT92", &m->cpx);
            if (n > 0) rebuild(i, "CPT92", m->fbuf, n, o, v92_cpu_build(&q, 12, o));
        }
        if (m->got_suvu && !L.got_suvu) {
            v92_suvu_t s; int n;
            L.got_suvu = 1;
            for (n = V92_SUV_BITS; n <= 96; n++) if (n % 24 == 0 && v92_suvu_parse(m->fbuf, n, 24, &s) > 0) break;
            if (n <= 96) {
                uint8_t o[128];
                printf("%8.3f SUVU wait_cpu %d level %d (q2.2 dB) silence %d ack %d\n", T(i), s.wait_cpu, s.level, s.silence, s.ack);
                rebuild(i, "SUVU", m->fbuf, n, o, v92_suvu_build(&s, 24, o));
            }
            else printf("%8.3f SUVU (not parsed again)\n", T(i));
        }
        if (m->got_cpu && !L.got_cpu) {
            static uint8_t o[V92_CPU_MAX_BITS + 16]; v92_cpu_t q; int n = v92_cpu_parse(m->fbuf, V92D_FBUF, 24, &q);
            L.got_cpu = 1; cpu(i, "CPU", &m->cpx);
            if (n > 0) rebuild(i, "CPU", m->fbuf, n, o, v92_cpu_build(&q, 24, o));
        }
        if (m->got_ack && !L.cpu_ack) { L.cpu_ack = 1; printf("%8.3f CPU' (acknowledge bit) heard\n", T(i)); }
    }
    if (a->phase == ANS_V34) {
        const v34ans_t *v = &a->v34;
        if ((int)v->p2.state != L.p2n && v->state == V34A_PHASE2) {
            L.p2n = (int)v->p2.state;
            printf("%8.3f P2N state %d rtd %d\n", T(i), L.p2n, (int)v->p2.rtd);
            if (L.p2n == P2N_WAIT_INFO1C) {
                int k;
                printf("%8.3f P2N probe rx_level(0.1dB)", T(i));
                for (k = 1; k <= PROBE_NBIN; k++) printf(" %d", v->p2.rx_level[k]);
                printf("\n%8.3f P2N probe snr(0.1dB)", T(i));
                for (k = 1; k <= PROBE_NBIN; k++) printf(" %d", v->p2.snr[k]);
                printf("\n");
            }
        }
        if ((int)v->dx.state != L.dx) { L.dx = (int)v->dx.state; printf("%8.3f DX state %d error %d\n", T(i), L.dx, (int)v->dx.error); }
        if (v->dx.got_far_mp && !L.mp) {
            const v34_mp_t *m = &v->dx.far_mp;
            L.mp = 1;
            {
                static uint8_t o[V34_MP1_BITS + 16]; v34_mp_t q; int n = v34_mp_parse(v->dx.fbuf, DX_FBUF, &q);
                if (n > 0) rebuild(i, "MP", v->dx.fbuf, n, o, v34_mp_build(&q, o));
            }
            printf("%8.3f MP type %d max_ca %d max_ac %d aux %d trellis %d nonlinear %d shaping %d ack %d rates 0x%04x asym %d h (%d,%d) (%d,%d) (%d,%d)\n",
                   T(i), m->type, m->max_ca, m->max_ac, m->aux, m->trellis, m->nonlinear, m->shaping, m->ack, m->rates, m->asym,
                   m->h[0][0], m->h[0][1], m->h[1][0], m->h[1][1], m->h[2][0], m->h[2][1]);
        }
    }
}

static void configure(v90m_cfg *c, int lin)
{
    v90m_cfg_default(c);
    c->modulations = V90M_MOD_V21 | V90M_MOD_V22 | V90M_MOD_V32BIS | V90M_MOD_V34 | V90M_MOD_V90 | (opt_v92 ? V90M_MOD_V92 : 0);
    c->tx_level = level_rms(opt_dbm0);
    c->max_down_bps = opt_down;
    c->max_up_bps = opt_up ? opt_up : opt_v92 ? 48000 : 33600;
    c->alaw = opt_alaw;
    c->codec_linear_rx = lin;
    c->echo_delay = opt_echo;
    c->max_tx_code = opt_code;
    c->getbit = ones;
    c->putbit = drop;
}

int main(int argc, char **argv)
{
    static uint8_t file[(24 << 20) + 8];
    rec_t r; v90m_cfg c; v90m_t *m; answer_t *a;
    long len, k0, i, diff = 0, first = -1, quiet_run = 0;
    int arg, err, car, k;
    FILE *f;

    for (arg = 1; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        const char *o = argv[arg];
        if (!strcmp(o, "-a")) opt_alaw = 1;
        else if (!strcmp(o, "-9")) opt_v92 = 1;
        else if (arg + 1 < argc && !strcmp(o, "-u")) opt_up = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-d")) opt_down = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-l")) opt_dbm0 = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-e")) opt_echo = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-P")) opt_code = atoi(argv[++arg]);
        else break;
    }
    if (arg >= argc || !(f = fopen(argv[arg], "rb"))) {
        fprintf(stderr, "usage: v90m-fields [-a] [-9] [-u BPS] [-d BPS] [-l DBM0] [-e SAMPLES] [-P CODE] RECORDING\n");
        return 2;
    }
    len = (long)fread(file, 1, sizeof file, f);
    fclose(f);
    memset(&r, 0, sizeof r);
    r.d = file; r.bs = 160; r.txo = 80;
    if (len >= 8 && !memcmp(file, "V90DLIN1", 8)) { r.lin = 1; r.bs = 240; r.txo = 160; r.d = file + 8; len -= 8; }
    r.n = len / r.bs * 80;

    configure(&c, r.lin);
    if (!(m = v90m_create(&c, &err))) { fprintf(stderr, "v90m_create: %d\n", err); return 1; }
    a = v90m__answer(m);
    for (k0 = 0; k0 < 8000 * 5; k0++) {
        uint8_t s = opt_alaw ? 0xD5 : 0xFF, t;
        v90m_process_g711(m, &s, &t, 1);
        if (!silent(opt_alaw, t)) break;
    }
    ans0 = recorded_answer(&r, opt_alaw);
    if (ans0 < 0) { fprintf(stderr, "no answer tone in the recording\n"); return 1; }
    printf("%8.3f RECORDING %s receive, %ld samples, answer tone at %.3f s into it\n", 0.0, r.lin ? "linear" : "companded", r.n, ans0 / 8000.0);
    v90m_reset(m, &c);
    memset(&L, 0, sizeof L);
    L.ph = L.v8 = L.p2 = L.p34 = L.v92 = L.p2n = L.dx = -1;

    for (k = 0; k < 40; k++) {
        ctab[0][k] = cos(2 * 3.14159265358979323846 * 1800 * k / 8000); stab[0][k] = sin(2 * 3.14159265358979323846 * 1800 * k / 8000);
        ctab[1][k] = cos(2 * 3.14159265358979323846 * 2400 * k / 8000); stab[1][k] = sin(2 * 3.14159265358979323846 * 2400 * k / 8000);
    }
    v21demod_init(&v21d, V21L_MARK, V21L_SPACE, 8000, 300);
    arx_init(&v21u, 8000, 300);
    v21sq = v21demod_energy_of(&v21d, SQUELCH_AMP);
    for (car = 0; car < 2; car++)
        for (k = 0; k < 3; k++) { dpskrx_init(&drx[car][k], car ? DPSK_FC_B : DPSK_FC_A); dpskrx_expect(&drx[car][k], plen[k]); }

    for (i = 0; i < r.n; i++) {
        uint8_t want = rec_sent(&r, i), tx;
        int16_t x = rec_heard(&r, i, opt_alaw);
        quiet_run = silent(opt_alaw, want) ? quiet_run + 1 : 0;
        v21_tick(i, x);
        dpsk_tick(i, x);
        power_tick(i, x, quiet_run > 400);
        if (i < ans0 - k0) continue;
        seg_tick(i, seg_key(a), x, quiet_run > 400);
        if (r.lin) v90m_process_lin_rx(m, &x, &tx, 1);
        else { uint8_t y = r.d[(i / 80) * r.bs + i % 80]; v90m_process_g711(m, &y, &tx, 1); }
        if (tx != want) { if (first < 0) first = i; diff++; }
        watch(a, i);
        cur_phase = (int)a->phase;
    }
    v21_flush();
    seg_end(r.n);
    printf("%8.3f END our transmit differs from the recording in %ld samples (first at %.3f s)\n", T(r.n), diff, first < 0 ? -1.0 : T(first));
    v90m_destroy(m);
    return 0;
}
