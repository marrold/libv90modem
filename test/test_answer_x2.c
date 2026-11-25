/* The answering modem (modem/answer) with x2 offered (ANS_MODE_X2), against
 * callers that are not x2 callers: what they get must be what they get
 * without it (docs/superpowers/specs/2026-11-20-x2-answering-design.md,
 * "Anti-goals").
 *   - A V.34 caller (sim/v34peer over test/line34.h, mu-law): JM is flipped
 *     after each complete JM and INFO0a carries max_sr_diff = 4 and cme
 *     (docs/history/x2-facts.md "Phase 1", "Phase 2 marker"); the call
 *     connects V.34 at the same rates, the data clean both ways, as without
 *     x2.
 *   - The same caller on an A-law line: not one transmit sample differs.
 *   - A caller whose CM has PCM (V.90): not one transmit sample differs.
 *   - A V.34 caller that sends the x2 marker in place of its L1 / L2 and
 *     then nothing: the call is x2 (ANS_X2) and ends with x2_failed.
 * And x2 itself, against the simulated client (sim/x2call) over a digital
 * link (our octets through the network's pad to it, its samples back
 * through a mu-law codec with our own echo and noise):
 *   - behind no pad, 3 dB and 6 dB: x2 at 32000 down and 24000 up, the
 *     client's checks of our script clean, 100000 random bits each way
 *     equal end to end;
 *   - an MP asking for an index we have no bank for, or reporting a pad we
 *     have no levels for: the call ends, x2_failed (plan, Review Focus 4);
 *   - the client silent after its marker, after its J, after its MP, in
 *     data: the call ends within 10 s, with an end reason (Review Focus 5);
 *   - an MP with other downstream signs than MD = 5: the call ends;
 *   - the client's rate change 12.7 s into data (docs/history/x2-facts.md
 *     "Rate change"), asking for index 8 with the mask 1FFE, and another,
 *     for index 14, 12.7 s into the first one's data (reneg_ms again),
 *     behind no pad, 3 dB and 6 dB: we answer after its MP (no marker
 *     frame before it), it checks our answer frame by frame, and the call
 *     goes on at 46666 and then 54666 down, 28800 up, the bits end to end; one asking for an index without a bank (0),
 *     or other MD, ends the call (x2_failed: V90M_END_X2_FAILED); our
 *     cleardown during it ends the call as ours;
 *   - a click in data mode (Task 20: 0.75 ms of full scale on the line to
 *     us, 15 s into the call of tools/rec_x2): the upstream clean from 2 s
 *     after it; and two noisy lines (TEST_SHORT: not run), 60 s of data
 *     each: the lowest SNR x2 connects at is not ended, the lowest with a
 *     clean upstream stays clean. */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../sim/v34peer.h"
/* the old cleared flag, from the end reason: 1 the caller's cleardown, 2 ours */
static int cleared_of(const answer_t *a)
{
    int e = answer_end(a);
    return e == ANS_END_CLEARDOWN_FAR ? 1 : e == ANS_END_CLEARDOWN_OURS ? 2 : 0;
}
/* the old x2_failed flag: 1 x2's start-up or rate change, 2 the client's retrain */
static int x2_failed_of(const answer_t *a)
{
    int e = answer_end(a);
    return e == ANS_END_X2_FAILED ? 1 : e == ANS_END_RETRAIN_FAILED ? 2 : 0;
}
#include "../sim/v8call.h"
#include "../sim/p2a.h"
#include "../sim/channel.h"
#include "../sim/g711.h"
#include "../sim/x2call.h"
#include "../sim/noise.h"
#include "line34.h"
#include <inttypes.h>
#include "wctr.h"

#define FS 8000

/* x^7 + x^6 + 1 sent; the receiver predicts each bit from the seven before */
typedef struct { unsigned tx, rx; long n, bad; } prbs_t;
static int p_get(void *u)
{
    prbs_t *p = (prbs_t *)u;
    int b = ((p->tx >> 6) ^ (p->tx >> 5)) & 1;
    p->tx = ((p->tx << 1) | (unsigned)b) & 127;
    return b;
}
static void p_put(void *u, int bit)
{
    prbs_t *p = (prbs_t *)u;
    int want = ((p->rx >> 6) ^ (p->rx >> 5)) & 1;
    if (p->n >= 7 && want != (bit & 1)) p->bad++;
    p->rx = ((p->rx << 1) | (unsigned)(bit & 1)) & 127;
    p->n++;
}

static answer_t A; static v34peer_t C; static line34_t L; static prbs_t UA, UC;

/* what our side sent, per run slot */
#define TXN (FS * 25)
static int16_t txs[2][TXN];
static long ntx;
static int slot;

/* the x2 client's marker: from the start of our L1 the caller's Phase 2 is
 * silent, and mk_at ms later the marker frame goes out on 1200 Hz */
static int mk_at; static long l1_at, mk_off; static dpsktx_t MK; static uint8_t mkbits[64]; static int nmk;

static int16_t a_tick(void *u, int16_t rx)
{
    int16_t t = answer_tick((answer_t *)u, rx);
    if (ntx < TXN) txs[slot][ntx++] = t;
    return t;
}
static int16_t c_tick(void *u, int16_t rx)
{
    int16_t t;
    if (mk_at && l1_at < 0 && A.phase == ANS_V34 && A.v34.state == V34A_PHASE2 && A.v34.p2.state == P2N_L1) {
        l1_at = L.n; C.p2.mute = 1;
    }
    t = v34peer_tick((v34peer_t *)u, rx);
    if (mk_at && l1_at >= 0) {
        int32_t v;
        if (L.n == l1_at + 8L * mk_at) { dpsktx_send(&MK, mkbits, nmk); mk_off = -1; }
        if (mk_off < 0 && !dpsktx_busy(&MK)) mk_off = L.n + 60;
        if (L.n == mk_off) dpsktx_off(&MK);
        v = t + dpsktx_tick(&MK);
        t = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return t;
}

static void start(unsigned modes, int law, int cm_access)
{
    answer_cfg ac; v34peer_cfg pc; line34_cfg lc;
    memset(&lc, 0, sizeof lc);
    lc.law = law; lc.d_ca = 24; lc.d_ac = 20; lc.gain = 0.5; lc.echo_a = 0.3; lc.echo_c = 0.3;
    memset(&UA, 0, sizeof UA); memset(&UC, 0, sizeof UC); UA.tx = 1; UC.tx = 77;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = modes; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
    v34peer_default(&pc, 3000);
    pc.cm_access = cm_access;
    pc.dx.getbit = p_get; pc.dx.putbit = p_put; pc.dx.user = &UC;
    answer_init(&A, &ac);
    if (law == 2) { p2d_cfg p = A.p2cfg; p.alaw = 1; answer_v90_cfg(&A, &p); }
    answer_v90_line(&A, 0, 0, 0);
    v34peer_init(&C, &pc); line34_init(&L, &lc);
    ntx = 0; l1_at = -1; mk_off = 0;
}

static void run(long samples) { while (samples-- > 0) (void)line34_step(&L, a_tick, &A, c_tick, &C); }

static int to_data(int max_ms)
{
    long end = L.n + 8L * max_ms;
    while (L.n < end) {
        run(8);
        if (A.connected && C.state == VP_DATA) return 1;
        if (A.phase == ANS_FAILED || C.state == VP_ENDED) return 0;
    }
    return 0;
}

typedef struct { int ok, tx, rx, sym; long bad; int sr_diff, cme, x2; } v34_outcome;

static void v34_call(unsigned modes, int law, int cm_access, v34_outcome *o)
{
    memset(o, 0, sizeof *o);
    start(modes, law, cm_access);
    o->ok = to_data(25000);
    o->x2 = v8ans_x2(&A.v8);
    o->sr_diff = C.p2.info0a.max_sr_diff; o->cme = C.p2.info0a.cme;
    if (!o->ok) return;
    o->tx = A.bitrate; o->rx = A.rx_bitrate; o->sym = A.v34.dx.cfg.sym_rate;
    UA.n = UA.bad = UC.n = UC.bad = 0;
    run(8000L * 3);
    o->bad = UA.bad + UC.bad;
    if (UA.n < 2L * A.rx_bitrate || UC.n < 2L * A.bitrate) o->bad = -1;
}

static int same_tx(long n)
{
    long k;
    for (k = 0; k < n; k++) if (txs[0][k] != txs[1][k]) return 0;
    return 1;
}

/* a V.90 caller (V.8 with PCM, then V.90 Phase 2 as the analogue modem)
 * through the channel simulator for 8 s; what we sent goes to txs[slot] */
static void v90_call(unsigned modes)
{
    static v8call_t v8; static p2a_t p2;
    static const double one[1] = {1.0};
    channel_cfg cfg; channel_t ch;
    answer_cfg ac; v8_menu_t cm; p2a_cfg pc;
    int16_t atx = 0, ctx = 0;
    int n, in_p2 = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = -60.0; cfg.us_noise_dbfs = -60.0;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.1; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 5;
    channel_init(&ch, &cfg);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = modes; ac.getbit = p_get; ac.putbit = p_put; ac.user = &UA;
    answer_init(&A, &ac);
    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA;
    cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3;
    cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG; cm.has_access = 1;
    v8call_init(&v8, FS, 4000, &cm);
    memset(&pc, 0, sizeof pc);
    pc.level = 4000; pc.v90 = 1; pc.up_sr = -1; pc.uinfo = 91;
    pc.info0a.lo3000 = pc.info0a.hi3000 = 1; pc.info0a.lo3200 = pc.info0a.hi3200 = 1;
    pc.info0a.pwr_reduce = 1; pc.info0a.max_sr_diff = 5; pc.info0a.c1664 = 1;
    ntx = 0;
    for (n = 0; n < 8 * FS; n++) {
        double crx; uint8_t srv; int16_t t, x;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &crx, &srv);
        t = answer_tick(&A, (int16_t)ulaw_to_linear(srv));
        if (ntx < TXN) txs[slot][ntx++] = t;
        x = (int16_t)lround(crx);
        if (!in_p2) {
            int16_t o = 0;
            if (v8call_tick(&v8, x, &o) == V8C_DONE) { in_p2 = 1; p2a_init(&p2, &pc); }
            ctx = o;
        } else {
            ctx = p2a_tick(&p2, x);
        }
        atx = t;
        if (A.phase == ANS_FAILED) break;
    }
}

/* ---- x2 against the simulated client ---- */

/* random bits, the same sequence at the far end: xorshift32 */
typedef struct { uint32_t tx, rx; long n, bad; } rbits_t;
static int rnext(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return (int)(*s & 1); }
static void r_check(rbits_t *r, int bit)
{
    if (rnext(&r->rx) != (bit & 1)) r->bad++;
    r->n++;
}

#define XD 4096
static x2call_t X; static rbits_t DN, UP;
/* the client's rate change, for the next x2_call (which clears it): ms
 * into data mode (0 none), its index, mask and MD (0: 5); index2 a second
 * one's, ms after the first's data (0 none); md0 the first MP's MD;
 * hangup: our cleardown once x2d is in the rate change; whole: the
 * client's S once its mapping frame under way is out (x2call_cfg) */
static struct { int ms, index, index2, md, md0, hangup, whole, words, up; uint16_t mask; } RN;
/* RN.words: the upstream is counter words (test/wctr.h), the client's S
 * after its mapping frame (RN.whole) or where it falls */
static wctr_t WU;
/* the upstream across the client's rate change: its V.34 stops where it
 * is (the frame it cut, and the frame our receiver keeps back as a margin
 * before S, do not come; or its S comes out as bits before we have seen
 * it). The bits it took from its source up to its S (at, st the generator
 * after them) are checked as they come; those past them before its E are
 * counted (extra); from its new data on, the check goes on from at (lost:
 * those taken that never came, over every S so far). */
static struct { int armed, done; uint32_t st; long at, lost, extra; } UG;
/* the client's retrain in data mode, for the next x2_call (which clears
 * it): ms into data mode it falls silent, and 70 ms later sends Tone A with
 * its 1800 Hz guard tone (80 % / 20 % of the power, as the U.S. Robotics
 * 56K did on 2026-11-21) until the call ends; at (sample) when the tone began */
static struct { int ms; long at; double amp, df; } TA;
/* a click on the line to us (Task 20), for the next x2_call (which clears
 * it): from sample at, six samples replaced by full-scale mu-law (0x80 and
 * 0x00 alternated, 0.75 ms); the upstream errors and bits counted once
 * from2 samples later. The noise at our side (rms, 2.0 unless set), for the
 * next x2_call too */
static struct { long at, from2, bad, n; } CK;
static double NZ;
/* the client behind a packet network (an ATA's jitter buffers), for the
 * next x2_call (which clears it): d samples each way, and our echo off its
 * line's hybrid a round trip late, 20 dB down; 0: the line above */
static struct { int d; } FAR;
static long up_taken;
/* downstream: our getbit to the client's putbit; upstream the other way */
/* our data bits taken from the point x2tx left data mode for its answer to
 * the client's rate change, until data moves again (x2d back in X2D_DATA):
 * the host learns that the line is back (V90M_EV_DATA_MOVING, and with it
 * v90m_ec_line_restored) before any of the new data goes, so that V.42 can
 * open it with flags */
static long dn_taken, dn_mark = -1, dn_early;
static int s_get(void *u) { (void)u; dn_taken++; return rnext(&DN.tx); }
static void s_put(void *u, int bit)
{
    (void)u;
    if (RN.words) {
        if (UG.armed && x2call_state(&X) != X2C_RENEG) { UG.armed = 0; UG.done = 1; }
        wctr_put(&WU, bit); UP.n++;
        return;
    }
    if (UG.armed) {
        if (x2call_state(&X) != X2C_RENEG) {
            UG.armed = 0; UG.done = 1;
            UG.lost = UG.at - UP.n; UP.rx = UG.st;     /* taken less received: every S so far */
        } else if (UP.n >= UG.at) {
            UG.extra++;
            return;
        }
    }
    r_check(&UP, bit);
}
static int c_get(void *u) { (void)u; up_taken++; return RN.words ? wctr_get(&WU) : rnext(&UP.tx); }
static void c_put(void *u, int bit) { (void)u; r_check(&DN, bit); }
typedef struct {
    int ok;                     /* both in data mode */
    int down, up, mod;
    long x2_at, quiet_at, end_at;   /* samples: ANS_X2, the client's silence, ANS_FAILED; -1 none */
    int error, x2_failed, connected_once;
    long rn_at, rn_seen, rn_back;   /* the client's last S, x2d's X2D_RENEG, back in X2D_DATA; -1 none */
    long dn0, up0;              /* bits checked each way at the client's last S */
    int renegs, downs[2];       /* rate changes back in X2D_DATA, our rate after each */
    int cd_ret;                 /* answer_cleardown in the rate change */
    int marked, back_seg;       /* our marker out in the last rate change; x2tx's segment as data moved again */
} x2_outcome;

/* One x2 call: the client with pad (0, 1, 2; 3 reported only), the index it
 * asks for, its upstream mask and where it falls silent; for up to secs of
 * line, or data_bits each way once both are in data mode. */
static void x2_call(int pad, int index, uint16_t up_mask, int silent, int echo_delay, int secs, long data_bits, x2_outcome *o)
{
    static uint8_t down[XD]; static int16_t up[XD]; static double dl[XD];
    const int d_ac = FAR.d ? FAR.d : 37, d_ca = FAR.d ? FAR.d : 41;     /* one-way delays, samples */
    /* our echo off the line's hybrid comes back this late: behind a driver's
     * buffers when we are not told (echo_delay -1: x2d measures it); or the
     * far hybrid's, a round trip late (FAR) */
    const int e = FAR.d ? d_ac + d_ca : echo_delay < 0 ? 300 : echo_delay;
    const double g = FAR.d ? 0.65 : 1.0;   /* (FAR: 20 dB down) */
    answer_cfg ac; x2call_cfg xc; rng_t rng;
    uint8_t arx = 0xff;
    long n, ta_n = 0;
    int nr = 0;
    TA.at = -1;
    memset(o, 0, sizeof *o);
    o->x2_at = o->quiet_at = o->end_at = -1;
    o->rn_at = o->rn_seen = o->rn_back = o->dn0 = o->up0 = -1; o->cd_ret = 1;
    memset(&UG, 0, sizeof UG); up_taken = 0; dn_taken = 0; dn_mark = -1; dn_early = 0;
    memset(down, 0x7f, sizeof down); memset(up, 0, sizeof up); memset(dl, 0, sizeof dl);
    memset(&DN, 0, sizeof DN); memset(&UP, 0, sizeof UP);
    DN.tx = DN.rx = 0x2468ace1u; UP.tx = UP.rx = 0x13579bdfu;
    rng_seed(&rng, 4242);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_X2;
    ac.getbit = s_get; ac.putbit = s_put; ac.user = NULL;
    answer_init(&A, &ac);
    answer_v90_line(&A, echo_delay, 0, 0);
    memset(&xc, 0, sizeof xc);
    xc.level = 3000; xc.pad_mode = pad; xc.max_index = index; xc.up_mask = up_mask; xc.silent = silent;
    xc.getbit = c_get; xc.putbit = c_put; xc.user = NULL;
    xc.md = RN.md0;
    xc.reneg_ms = RN.ms; xc.reneg_index = RN.index; xc.reneg_mask = RN.mask; xc.reneg_md = RN.md;
    xc.reneg_index2 = RN.index2;
    xc.reneg_whole = RN.whole; xc.reneg_up = RN.up;
    memset(&WU, 0, sizeof WU); WU.gen = 0x2c00;
    x2call_init(&X, &xc);
    for (n = 0; n < 8000L * secs; n++) {
        int i = (int)(n & (XD - 1));
        double v;
        uint8_t atx;
        if (CK.at && n >= CK.at && n < CK.at + 6) arx = (n - CK.at) & 1 ? 0x00 : 0x80;
        if (CK.at && n == CK.at + CK.from2) { CK.bad = UP.bad; CK.n = UP.n; }
        atx = answer_tick_pcm(&A, arx);
        down[i] = atx;
        dl[i] = (double)ulaw_to_linear(atx);
        up[i] = x2call_tick(&X, down[(n - d_ac) & (XD - 1)]);
        if (TA.ms && o->connected_once && ++ta_n > (long)TA.ms * 8) {
            long k = ta_n - (long)TA.ms * 8 - 560;          /* 70 ms of silence first */
            up[i] = 0;
            if (k >= 0) {
                if (TA.at < 0) TA.at = n;
                double g = TA.amp > 0 ? TA.amp : 1.0;
                up[i] = (int16_t)lround(g * (5000.0 * sin(6.283185307179586 * (2400.0 + TA.df) * k / 8000)
                                             + 2500.0 * sin(6.283185307179586 * (1800.0 + TA.df) * k / 8000)));
            }
        }
        /* what reaches us: the client, our own echo off the line's hybrid, noise */
        v = 0.5 * up[(n - d_ca) & (XD - 1)]
            + g * 0.12 * dl[(n - e - 2) & (XD - 1)] + g * 0.08 * dl[(n - e - 4) & (XD - 1)] - g * 0.05 * dl[(n - e - 9) & (XD - 1)]
            + (NZ > 0 ? NZ : 2.0) * rng_gauss(&rng);
        arx = linear_to_ulaw((int32_t)lround(v));
        if (A.phase == ANS_X2 && o->x2_at < 0) o->x2_at = n;
        if (X.quiet && o->quiet_at < 0) o->quiet_at = n;
        if (A.connected && A.modulation == ANS_MOD_X2) o->connected_once = 1;
        if (X.renegs > nr) {
            /* the client's S: it has taken its last upstream bit */
            nr = X.renegs;
            o->rn_at = n; o->dn0 = DN.n; o->up0 = UP.n; o->rn_seen = o->rn_back = -1;
            UG.armed = 1; UG.done = 0; UG.st = UP.tx; UG.at = up_taken;
        }
        if (A.phase == ANS_X2 && A.u.x2.state == X2D_RENEG && o->rn_at >= 0 && o->rn_seen < 0) {
            o->rn_seen = n; o->marked = 0;
            if (RN.hangup) o->cd_ret = answer_cleardown(&A);
        }
        if (o->rn_seen >= 0 && o->rn_back < 0 && A.phase == ANS_X2 && A.u.x2.tx.seg == X2TX_MARK) o->marked = 1;
        if (o->rn_seen >= 0 && o->rn_back < 0 && dn_mark < 0 && A.phase == ANS_X2 && A.u.x2.tx.seg != X2TX_DATA)
            dn_mark = dn_taken;
        if (o->rn_seen >= 0 && o->rn_back < 0 && A.phase == ANS_X2 && A.u.x2.state == X2D_DATA) {
            if (dn_mark >= 0) dn_early += dn_taken - dn_mark;
            dn_mark = -1;
            o->rn_back = n; o->back_seg = A.u.x2.tx.seg;
            if (o->renegs < 2) o->downs[o->renegs] = x2d_down_rate(&A.u.x2);
            o->renegs++;
        }
        if (A.phase == ANS_FAILED) { o->end_at = n; break; }
        if (data_bits && A.connected && x2call_state(&X) == X2C_DATA && DN.n >= data_bits && UP.n >= data_bits) {
            if (!RN.ms) break;
            if (o->renegs == (RN.index2 ? 2 : 1) && UG.done && DN.n - o->dn0 >= data_bits && UP.n - o->up0 >= data_bits) break;
        }
    }
    memset(&RN, 0, sizeof RN); TA.ms = 0; TA.amp = TA.df = 0; CK.at = 0; NZ = 0; FAR.d = 0;
    o->ok = A.connected && A.phase == ANS_X2 && x2call_state(&X) == X2C_DATA;
    o->down = A.bitrate; o->up = A.rx_bitrate; o->mod = A.modulation;
    o->error = A.u.x2.error; o->x2_failed = x2_failed_of(&A);
}

static void x2_report(const char *what, const x2_outcome *o)
{
    const x2call_seen *s = &X.seen;
    printf("  x2 %s: %s %d / %d bit/s; down %ld bits %ld bad, up %ld bits %ld bad\n", what,
           o->ok ? "data" : "NO DATA", o->down, o->up, DN.n, DN.bad, UP.n, UP.bad);
    printf("     pad mode %d in the MP and the mapper; echo delay %d; upstream SNR %.1f dB\n",
           A.u.x2.pad_mode, A.u.x2.ecd.delay, answer_snr_db10(&A) / 10.0);
    printf("     client saw: preamble %ld bad, TRN %ld bad, J words %d + %d, ones %ld bad, alignment %ld bad,\n"
           "     sync %ld frames (%ld bad), records %d (%d bad), start %ld (%ld bad), data frames %ld (%ld lost)\n",
           s->pre_bad, s->trn_bad, s->jw_good, s->jw_last, s->ones_bad, s->align_bad, s->sync_frames, s->sync_bad,
           s->rec_good, s->rec_bad, s->start_ones, s->start_bad, s->data_frames, s->data_lost);
}

/* the client's script checks, clean */
static void x2_seen_clean(void)
{
    const x2call_seen *s = &X.seen;
    CHECK_EQ_I(s->not_x2, 0);
    CHECK_EQ_I(s->pre_bad, 0); CHECK_EQ_I(s->trn_bad, 0);
    CHECK_EQ_I(s->jw_good, 99); CHECK_EQ_I(s->jw_last, 1);
    CHECK_EQ_I(s->ones_bad, 0); CHECK_EQ_I(s->align_bad, 0);
    CHECK(s->sync_frames >= 1667); CHECK_EQ_I(s->sync_bad, 0);
    CHECK(s->rec_good >= 1); CHECK_EQ_I(s->rec_bad, 0);
    CHECK_EQ_I(s->start_ones, 680); CHECK_EQ_I(s->start_bad, 0);
    CHECK_EQ_I(s->data_lost, 0);
}

static void x2_tests(void)
{
    static const char *const pad_name[3] = { "no pad", "3 dB pad", "6 dB pad" };
    x2_outcome o;
    int pad;

    /* 1. x2 at 32000 down, 24000 up (the Courier's mask, 03FE), behind each pad */
    for (pad = 0; pad < 3; pad++) {
        x2_call(pad, 1, 0x03FE, X2C_SIL_NONE, pad == 0 ? -1 : 0, 40, 100000, &o);
        x2_report(pad_name[pad], &o);
        CHECK(o.ok);
        CHECK_EQ_I(o.mod, ANS_MOD_X2); CHECK_EQ_I(o.mod, 10 /* V90M_CONN_X2 */);
        CHECK_EQ_I(o.down, 32000); CHECK_EQ_I(o.up, 24000);
        CHECK_EQ_I(o.x2_failed, 0);
        CHECK_EQ_I(A.u.x2.pad_mode, pad); CHECK_EQ_I(A.u.x2.tx.map.pad_mode, pad);
        if (pad == 0) CHECK(A.u.x2.ecd.delay > 300 - 24 && A.u.x2.ecd.delay <= 300 + 2);   /* measured */
        x2_seen_clean();
        CHECK(DN.n >= 100000); CHECK_EQ_I(DN.bad, 0);
        CHECK(UP.n >= 100000); CHECK_EQ_I(UP.bad, 0);
        CHECK(answer_snr_db10(&A) > 200);
    }

    /* 2. the MP asks for index 0 (we have banks for 1..15: 0x7FFF), or
     *    reports a pad of mode 3, or other downstream signs than MD = 5 (our
     *    banks' MD, x2-facts.md "Banks"): the call ends, as x2's, soon after
     *    it */
    {
        static const struct { int pad, index, md, err; const char *what; } no[] = {
            { 0, 0, 0, X2D_ERR_RATE, "MP asks for index 0" }, { 3, 1, 0, X2D_ERR_PAD, "MP reports pad mode 3" },
            { 0, 1, 6, X2D_ERR_RATE, "MP has MD 6" } };
        int j;
        for (j = 0; j < 3; j++) {
            RN.md0 = no[j].md;
            x2_call(no[j].pad, no[j].index, 0x03FE, X2C_SIL_NONE, 0, 30, 0, &o);
            printf("  x2 %s: ended %.2f s after the marker, error %d, x2_failed %d, %d MPs sent\n", no[j].what,
                   (o.end_at - o.x2_at) / 8000.0, o.error, o.x2_failed, X.mp_count);
            CHECK(o.x2_at > 0); CHECK(o.end_at > o.x2_at);
            CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(o.x2_failed, 1); CHECK_EQ_I(o.connected_once, 0);
            CHECK_EQ_I(o.error, no[j].err);
            CHECK(X.mp_count >= 2 && X.mp_count <= 4);     /* at its second MP */
            CHECK_EQ_I(X.seen.rec_good, 0);
        }
    }

    /* 3. the client falls silent after its marker, after its J, after its
     *    MP, in data mode: the call ends within 10 s, with a reason */
    {
        static const struct { int sil, err; const char *what; } q[] = {
            { X2C_SIL_MARKER, X2D_ERR_NO_J, "after its marker" }, { X2C_SIL_J, X2D_ERR_NO_S4, "after its J" },
            { X2C_SIL_MP, X2D_ERR_NO_E, "after its MP" }, { X2C_SIL_DATA, X2D_ERR_CARRIER, "in data mode" } };
        int j;
        for (j = 0; j < 4; j++) {
            x2_call(0, 1, 0x03FE, q[j].sil, 0, 40, 0, &o);
            printf("  x2 client silent %s: the call ended %.2f s later, error %d, x2_failed %d\n", q[j].what,
                   (o.end_at - o.quiet_at) / 8000.0, o.error, o.x2_failed);
            CHECK(o.quiet_at > 0); CHECK(o.end_at > o.quiet_at);
            CHECK(o.end_at - o.quiet_at <= 8000L * 10);
            CHECK_EQ_I(A.phase, ANS_FAILED);
            CHECK_EQ_I(o.error, q[j].err);
            /* the end reason: x2's before data mode, the connection lost in it */
            CHECK_EQ_I(o.x2_failed, q[j].sil != X2C_SIL_DATA);
            if (q[j].sil == X2C_SIL_DATA) CHECK_EQ_I(answer_end(&A), ANS_END_LOST);
            CHECK_EQ_I(o.connected_once, q[j].sil == X2C_SIL_DATA);
        }
    }

    /* 4. the client's rate change 12.7 s into data mode (D-Modem's client;
     *    the Courier's is 12.1 s), asking for index 8 with its mask 1FFE,
     *    and 12.7 s into its data (reneg_ms again) another, for index 14:
     *    our answer after each MP and not before, as the facts give it,
     *    checked by the client frame by frame; then 46666 and 54666 down, 28800 up. The downstream bits
     *    run on end to end; the upstream ones up to each of the client's S
     *    and from its new data on (see UG). Behind each pad (TEST_SHORT: no
     *    pad, 2 s into data, 2 s apart). */
    for (pad = 0; pad < 3; pad++) {
        const x2call_seen *s = &X.seen;
#ifdef TEST_SHORT
        if (pad) break;
        RN.ms = 2000;
#else
        RN.ms = 12700;
#endif
        RN.index = 8; RN.index2 = 14; RN.mask = 0x1FFE;
        x2_call(pad, 1, 0x03FE, X2C_SIL_NONE, pad == 0 ? -1 : 0, 50, 50000, &o);
        printf("  x2 rate changes to 8 and 14, %s: %s %d then %d / %d bit/s; x2d saw the client's last S %.3f s after it began, data again %.2f s after it\n",
               pad_name[pad], o.ok ? "data" : "NO DATA", o.downs[0], o.down, o.up,
               o.rn_seen < 0 ? -1.0 : (o.rn_seen - o.rn_at) / 8000.0, o.rn_back < 0 ? -1.0 : (o.rn_back - o.rn_at) / 8000.0);
        printf("     client saw: markers %ld + %d (%ld bad, %ld before its MP), records %d 733C then %d F33C (%d bad),"
               " ones %d + %ld (%ld bad), data frames %ld (%ld lost)\n",
               s->mark_frames, s->mark_last, s->mark_bad, s->mark_early, s->rr_nak, s->rr_ack, s->rr_bad,
               s->rr_ones, s->rstart_ones, s->rstart_bad, s->data_frames, s->data_lost);
        printf("     down %ld bits %ld bad (%ld after the last change); up %ld bits %ld bad (%ld after),"
               " the client's S: %ld bits lost, %ld extra\n",
               DN.n, DN.bad, DN.n - o.dn0, UP.n, UP.bad, UP.n - o.up0, UG.lost, UG.extra);
        CHECK(o.ok); CHECK_EQ_I(X.renegs, 2); CHECK_EQ_I(o.renegs, 2);
        CHECK_EQ_I(o.downs[0], 46666); CHECK_EQ_I(o.downs[1], 54666);
        CHECK_EQ_I(o.down, 54666); CHECK_EQ_I(o.up, 28800);
        CHECK_EQ_I(o.x2_failed, 0); CHECK_EQ_I(A.u.x2.state, X2D_DATA);
        CHECK(o.rn_seen > o.rn_at); CHECK(o.rn_back > o.rn_seen);
        x2_seen_clean();
        CHECK_EQ_I(s->mark_early, 0);
        CHECK_EQ_I(s->mark_frames, 16); CHECK_EQ_I(s->mark_last, 1); CHECK_EQ_I(s->mark_bad, 0);
        CHECK_EQ_I(s->rr_nak, 20); CHECK_EQ_I(s->rr_ack, 66); CHECK_EQ_I(s->rr_bad, 0); CHECK_EQ_I(s->rr_order, 0);
        CHECK_EQ_I(s->rr_ones, 2); CHECK_EQ_I(s->rstart_ones, 2 * 681); CHECK_EQ_I(s->rstart_bad, 0);
        CHECK_EQ_I(A.u.x2.pad_mode, pad); CHECK_EQ_I(A.u.x2.tx.map.pad_mode, pad); CHECK_EQ_I(A.u.x2.tx.map.index, 14);
        CHECK_EQ_I(DN.bad, 0); CHECK(o.dn0 > 0); CHECK(DN.n - o.dn0 >= 50000);
        CHECK_EQ_I(UP.bad, 0); CHECK(o.up0 > 0); CHECK(UP.n - o.up0 >= 50000);
        CHECK_EQ_I(UG.done, 1); CHECK(UG.lost >= 0 && UG.lost <= 2 * 512); CHECK(UG.extra <= 2 * 512);
        CHECK_EQ_I(dn_early, 0);        /* no data bit before data moves again */
    }

    /* 4b. the upstream whole across the client's rate change: counter words.
     *    A client that sends its mapping frame under way whole before its S
     *    loses exactly the one frame x2d keeps as a margin against an S
     *    found late (v34_rx_flush_before); one that cuts it loses that one
     *    too. Nothing repeated or wrong either way. */
    for (pad = 0; pad < 2; pad++) {
        int64_t frame, gap;
        const int j = pad;
        RN.ms = 2000; RN.index = 8; RN.mask = 0x1FFE; RN.whole = !j; RN.words = 1;
        x2_call(0, 1, 0x03FE, X2C_SIL_NONE, 0, 40, 50000, &o);
        frame = 24000 * 8 / 3200;
        gap = WU.gap_bits;
        printf("  x2 rate change to 8, the client's S %s, counter words up: %ld words, lost %" PRId64 ", again %" PRId64
               "; breaks %" PRId64 " (%" PRId64 " bits missing, the mapping frame %" PRId64 ")\n",
               j ? "where it falls" : "after its mapping frame", (long)WU.words, WU.lost, WU.again, WU.breaks, gap, frame);
        CHECK(o.ok); CHECK_EQ_I(X.renegs, 1); CHECK_EQ_I(o.renegs, 1);
        CHECK(UP.n - o.up0 >= 50000);
        CHECK_EQ_I(WU.again, 0);
        CHECK_EQ_I(WU.breaks, 1);
        if (!j) CHECK(gap >= frame - 1 && gap <= frame);
        else CHECK(gap >= frame - 1 && gap <= 2 * frame);
        CHECK(16 * WU.lost >= gap && 16 * WU.lost < gap + 32);
        CHECK_EQ_I(WU.locked, 1);
    }

    /* 4c. its S near the start of its upstream data (Task 10d): 40 .. 450
     *    ms after its B1's first symbol, before our start segment is out
     *    (x2d in X2D_START until some 510 ms after its E): followed as a
     *    rate change, no bit delivered that it did not send as data (nothing
     *    inserted; before the first lock only the tail of the word before
     *    it), nothing repeated; its S before its B1 is decided (40 ms; some
     *    55 ms after its E) drops what came before it (nothing of it is
     *    known to be data), later ones lose the margin frame. Data moves
     *    again only once our marker, records and start segment at the new
     *    rate are out (none of our data before) */
    {
        static const struct { int ms, kept; } st[] = { { 40, 0 }, { 50, 1 }, { 60, 1 }, { 70, 1 }, { 150, 1 }, { 450, 1 } };
        int k;
        for (k = 0; k < (int)(sizeof st / sizeof st[0]); k++) {
            int64_t frame = 24000 * 8 / 3200, gap;
            RN.ms = st[k].ms; RN.index = 8; RN.mask = 0x1FFE; RN.whole = 1; RN.words = 1; RN.up = 1;
            x2_call(0, 1, 0x03FE, X2C_SIL_NONE, 0, 40, 50000, &o);
            gap = WU.gap_bits;
            printf("  x2 rate change, the client's S %d ms into its upstream data, counter words up: %ld words, lost %" PRId64
                   ", again %" PRId64 "; breaks %" PRId64 " (%" PRId64 " bits missing), %" PRId64 " bits before the first;"
                   " our marker %d, data again in segment %d\n",
                   st[k].ms, (long)WU.words, WU.lost, WU.again, WU.breaks, gap, WU.lead, o.marked, o.back_seg);
            CHECK(o.ok); CHECK_EQ_I(X.renegs, 1); CHECK_EQ_I(o.renegs, 1);
            CHECK(UP.n - o.up0 >= 50000);
            CHECK_EQ_I(o.marked, 1);
            CHECK(o.back_seg == X2TX_START || o.back_seg == X2TX_DATA);
            CHECK_EQ_I(dn_early, 0);
            CHECK_EQ_I(WU.again, 0);
            CHECK(WU.lead >= 0 && WU.lead < 16); CHECK_EQ_I(WU.lead_tail, 1);
            CHECK_EQ_I(WU.locked, 1);
            if (st[k].kept) {
                CHECK_EQ_I(WU.breaks, 1);
                CHECK(gap >= frame - 1 && gap <= frame);
                CHECK(16 * WU.lost >= gap && 16 * WU.lost < gap + 32);
            } else {
                CHECK_EQ_I(WU.breaks, 0); CHECK_EQ_I(WU.lost, 0);
            }
        }
    }

    /* 5. a rate change we cannot serve: an index without a bank (0), or
     *    other MD: the call ends at its MP, said as x2's
     *    (V90M_END_X2_FAILED); no marker of ours. Our cleardown in a rate
     *    change: the call ends at once, as ours. */
    {
        static const struct { int index, md, hangup, err; const char *what; } rq[] = {
            { 0, 0, 0, X2D_ERR_RATE, "asks for index 0" }, { 1, 6, 0, X2D_ERR_RATE, "has MD 6" },
            { 1, 0, 1, X2D_ERR_HANGUP, "and our cleardown in it" } };
        int j;
        for (j = 0; j < 3; j++) {
            RN.ms = 1000; RN.index = rq[j].index; RN.mask = 0x1FFE; RN.md = rq[j].md; RN.hangup = rq[j].hangup;
            x2_call(0, 1, 0x03FE, X2C_SIL_NONE, 0, 40, 0, &o);
            printf("  x2 rate change %s: ended %.2f s after the client's S, error %d, x2_failed %d, cleared %d,"
                   " marker frames %ld + %ld\n", rq[j].what, o.rn_at < 0 || o.end_at < 0 ? -1.0 : (o.end_at - o.rn_at) / 8000.0,
                   o.error, o.x2_failed, cleared_of(&A), X.seen.mark_early, X.seen.mark_frames);
            CHECK_EQ_I(X.renegs, 1); CHECK(o.rn_at > 0); CHECK(o.rn_seen > o.rn_at);
            CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(o.connected_once, 1);
            CHECK_EQ_I(o.error, rq[j].err);
            CHECK(o.end_at > o.rn_at && o.end_at - o.rn_at <= 8000L * 2);
            CHECK_EQ_I(X.seen.mark_early, 0); CHECK_EQ_I(X.seen.mark_frames, 0);
            if (rq[j].hangup) {
                CHECK_EQ_I(o.cd_ret, 0); CHECK_EQ_I(answer_end(&A), ANS_END_CLEARDOWN_OURS); CHECK_EQ_I(o.x2_failed, 0);
                CHECK(o.end_at - o.rn_seen <= 1);
            } else {
                CHECK_EQ_I(o.x2_failed, 1);
                CHECK(X.mp_count >= 2);
            }
        }
    }
    /* 6. the client's retrain in data mode (its Tone A with the guard tone):
     *    x2 has no way to serve it (docs/history/x2-facts.md "Unsettled"):
     *    the call ends within a fraction of a second of the tone, said as a
     *    retrain that failed (V90M_END_RETRAIN_FAILED), not left in data
     *    mode while the client gives up */
    {
        /* at the client's level, 12 dB under it, and some Hz off */
        static const struct { double amp, df; } tv[] = { { 1.0, 0 }, { 0.25, 0 }, { 1.0, -8 }, { 0.25, 8 } };
        int j;
        for (j = 0; j < 4; j++) {
            TA.ms = 1500; TA.amp = tv[j].amp; TA.df = tv[j].df;
            x2_call(0, 1, 0x03FE, X2C_SIL_NONE, 0, 20, 0, &o);
            printf("  x2 client's Tone A in data mode (x%.2f, %+.0f Hz): ended %.3f s after the tone began, error %d, x2_failed %d\n",
                   tv[j].amp, tv[j].df, TA.at < 0 || o.end_at < 0 ? -1.0 : (o.end_at - TA.at) / 8000.0, o.error, o.x2_failed);
            CHECK_EQ_I(o.connected_once, 1); CHECK(TA.at > 0);
            CHECK_EQ_I(A.phase, ANS_FAILED);
            CHECK_EQ_I(o.error, X2D_ERR_RETRAIN);
            CHECK_EQ_I(o.x2_failed, 2);
            CHECK(o.end_at > TA.at && o.end_at - TA.at <= 8000L / 5);
        }
    }
    /* 7. a click in data mode (Task 20): 0.75 ms of full scale on the line
     *    to us, 15.000 s into the call of tools/rec_x2 (40 s, no pad, the
     *    echo delay measured). From 2 s after it the upstream is clean
     *    again, or the call has ended within 3 s of it as lost: never
     *    up and deaf */
    {
        long errs;
        int end;
        CK.at = 8000L * 15; CK.from2 = 8000L * 2;
        x2_call(0, 1, 0x03FE, X2C_SIL_NONE, -1, 40, 0, &o);
        end = answer_end(&A);
        errs = UP.bad - CK.bad;
        printf("  x2 click at 15.000 s: up %ld bits %ld bad, %ld bits %ld bad from 2 s after it; end %d%s\n",
               UP.n, UP.bad, UP.n - CK.n, errs, end, o.end_at < 0 ? "" : " (ended)");
        CHECK(o.connected_once);
        CHECK((errs == 0 && UP.n - CK.n >= 8L * 24000 && end == ANS_END_NONE)
              || (o.end_at >= 0 && end == ANS_END_LOST && o.end_at <= 8000L * 15 + 8000L * 3));
    }
#ifndef TEST_SHORT     /* on ARM under qemu: 144 s of line */
    /* 8. noisy lines, 60 s of data mode each: the lowest SNR at which x2
     *    connects today (noise rms 92 at our side, some 23 dB; the
     *    upstream then has errors) is not ended, and the lowest at which
     *    its upstream is clean (rms 68, some 27 dB) stays clean */
    {
        static const double nz[2] = { 92, 68 };
        int j;
        for (j = 0; j < 2; j++) {
            NZ = nz[j];
            x2_call(0, 1, 0x03FE, X2C_SIL_NONE, -1, 72, 0, &o);
            printf("  x2 with noise rms %.0f: %s %d / %d bit/s, upstream SNR %.1f dB, up %ld bits %ld bad; end %d\n",
                   nz[j], o.ok ? "data" : "NO DATA", o.down, o.up, answer_snr_db10(&A) / 10.0, UP.n, UP.bad, answer_end(&A));
            CHECK(o.ok); CHECK(UP.n >= 60L * 24000);
            CHECK_EQ_I(answer_end(&A), ANS_END_NONE);
            if (j == 1) CHECK_EQ_I(UP.bad, 0);
        }
    }
#endif
}

/* the client behind an ATA or a VoIP gateway (ISP calls of 2026-11-23: a
 * Cisco IOS FXS port's hybrid at 302 ms, 19.5 dB down): our echo a round
 * trip late, measured (echo delay not known). x2d finds it near the round
 * trip of Phase 2 and cancels it: the upstream as clean as on a short path.
 * And a round trip of 100 ms, inside the search as it always was. */
static void x2_far_echo_tests(void)
{
    static const int ms[3] = { 300, 400, 100 };
    x2_outcome o;
    int k;
    for (k = 0; k < 3; k++) {
        int e;
#ifdef TEST_SHORT
        if (k > 0) break;
#endif
        FAR.d = ms[k] * 4; e = 2 * FAR.d + 2;      /* (the echo path's first tap) */
        x2_call(0, 1, 0x03FE, X2C_SIL_NONE, -1, 40, 100000, &o);
        printf("  x2, far echo %d ms, 20 dB down:", ms[k]); x2_report("", &o);
        printf("     echo at %d samples, round trip of Phase 2 %d\n", e, answer_rtd(&A));
        CHECK(o.ok);
        CHECK(A.u.x2.ecd.delay >= e - 12 && A.u.x2.ecd.delay <= e);
        CHECK(answer_snr_db10(&A) > 300);              /* upstream error below -30 dB */
        CHECK(DN.n >= 100000); CHECK_EQ_I(DN.bad, 0);
        CHECK(UP.n >= 100000); CHECK_EQ_I(UP.bad, 0);
    }
}

int main(void)
{
    const unsigned base = ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21;
    v34_outcome o0, o1;
    long n0;

    /* 1. a V.34 caller, mu-law: x2 is offered to it and it does not take it */
    slot = 0; v34_call(base, 1, 0, &o0);
    slot = 1; v34_call(base | ANS_MODE_X2, 1, 0, &o1);
    printf("  V.34 caller, x2 offered: %d / %d bit/s at %d baud, %ld bad (without x2: %d / %d at %d, %ld bad)\n",
           o1.tx, o1.rx, o1.sym, o1.bad, o0.tx, o0.rx, o0.sym, o0.bad);
    CHECK_EQ_I(o0.ok, 1); CHECK_EQ_I(o1.ok, 1);
    CHECK_EQ_I(A.x2_offered, 1);
    CHECK_EQ_I(o0.x2, 0); CHECK_EQ_I(o1.x2, 1);
    CHECK_EQ_I(o0.sr_diff, 0); CHECK_EQ_I(o0.cme, 0);
    CHECK_EQ_I(o1.sr_diff, 4); CHECK_EQ_I(o1.cme, 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V34); CHECK_EQ_I(A.phase, ANS_V34);
    CHECK_EQ_I(o1.tx, o0.tx); CHECK_EQ_I(o1.rx, o0.rx); CHECK_EQ_I(o1.sym, o0.sym);
    CHECK_EQ_I(o0.bad, 0); CHECK_EQ_I(o1.bad, 0);
#ifndef TEST_SHORT     /* on ARM under qemu (test.h): cases 1, 3 and 4 */
    /* its CM carries the PSTN access octet: the JM gives it back with the
     * digital-network bit; still V.34 */
    slot = 1; v34_call(base | ANS_MODE_X2, 1, 1, &o1);
    CHECK_EQ_I(o1.ok, 1); CHECK_EQ_I(o1.x2, 1);
    CHECK(A.v8.jm.has_access); CHECK_EQ_I(A.v8.jm.access & V8_ACC_DIGITAL, V8_ACC_DIGITAL);
    CHECK(C.v8.got_jm && C.v8.jm.has_access);
    CHECK_EQ_I(o1.tx, o0.tx); CHECK_EQ_I(o1.rx, o0.rx); CHECK_EQ_I(o1.bad, 0);

    /* 2. the same caller on an A-law line: x2 is not offered at all */
    slot = 0; v34_call(base, 2, 1, &o0); n0 = ntx;
    slot = 1; v34_call(base | ANS_MODE_X2, 2, 1, &o1);
    CHECK_EQ_I(o0.ok, 1); CHECK_EQ_I(o1.ok, 1);
    CHECK_EQ_I(A.x2_offered, 0); CHECK_EQ_I(o1.x2, 0);
    CHECK_EQ_I(o1.sr_diff, 0); CHECK_EQ_I(o1.cme, 0);
    CHECK_EQ_I(ntx, n0);
    CHECK(same_tx(ntx < n0 ? ntx : n0));
    printf("  A-law V.34 caller, x2 offered: %ld samples sent, %s\n", ntx, same_tx(ntx < n0 ? ntx : n0) ? "all as without x2" : "SOME DIFFER");

#endif

    /* 3. a V.90 caller (its CM has PCM): V.90, and the same samples */
    slot = 0; v90_call(base); n0 = ntx;
    CHECK_EQ_I(A.v8.selected, V8_SEL_V90); CHECK(A.phase == ANS_P3 || A.phase == ANS_P2);
    slot = 1; v90_call(base | ANS_MODE_X2);
    CHECK_EQ_I(A.v8.selected, V8_SEL_V90); CHECK(A.phase == ANS_P3 || A.phase == ANS_P2);
    CHECK_EQ_I(A.x2_offered, 1); CHECK_EQ_I(v8ans_x2(&A.v8), 0);
    CHECK_EQ_I(ntx, n0);
    CHECK(same_tx(ntx < n0 ? ntx : n0));
    printf("  V.90 caller, x2 offered: %ld samples sent, %s\n", ntx, same_tx(ntx < n0 ? ntx : n0) ? "all as without x2" : "SOME DIFFER");

    /* 4. an x2 client's marker (4d) 170 ms after our L1 began, and then
     *    nothing: ANS_X2, and the end, said as x2's */
    {
        long t, x2_at = -1;
        slot = 0; mk_at = 170;
        dpsktx_init(&MK, DPSK_FC_B, 3000, 0);
        nmk = p2ans_x2_info_frame(0x4d, mkbits);
        start(base | ANS_MODE_X2, 1, 1);
        for (t = 0; t < 8000L * 14 && A.phase != ANS_FAILED; t++) {
            run(1);
            if (A.phase == ANS_X2 && x2_at < 0) x2_at = L.n;
        }
        printf("  x2 marker: ANS_X2 %.0f ms after our L1 began; the call ended (x2_failed %d)\n",
               (x2_at - l1_at) / 8.0, x2_failed_of(&A));
        CHECK(l1_at > 0); CHECK(x2_at > l1_at);
        CHECK_EQ_I(A.v34.p2.x2_marker, 0x4d);
        CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&A), ANS_END_X2_FAILED); CHECK_EQ_I(A.connected, 0);
        mk_at = 0;
    }

    x2_tests();
    x2_far_echo_tests();
    T_REPORT("answer_x2");
}
