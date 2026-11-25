/* Test peer: a K56flex client. See k56call.h. Written from
 * docs/history/k56-facts.md; no code of D-Modem. */
#include <math.h>
#include <string.h>
#include "k56call.h"
#include "g711.h"
#include "x2call.h"
#include "../modem/v8msg.h"
#include "../modem/p2ans.h"
#include "../modem/v34train.h"
#include "../modem/k56tab.h"
#include "../v90/v90_pcm.h"

#define MS(ms) ((int32_t)(ms) * 8)
#define TWO_PI 6.283185307179586
#define BLOCK 80

/* the responder's steps */
enum { B_CRE, B_CRE_IN, B_CRD, B_CL, B_MS, B_ACK, B_OFF };

/* SM56's MS (k56-facts.md "V.8bis messages") */
static const uint8_t sm56_ms[13] = { 0x11, 0xc9, 0x80, 0x80, 0x80, 0x07, 0xb5, 0x02, 0x00, 0x94, 0x81, 0x83, 0x21 };

void k56call_default(k56call_cfg *cfg, int level)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->level = level;
    cfg->v8bis = K56C_V8BIS_FULL;
    cfg->info1c_k56 = 1;
    cfg->j_octet = -1;
    cfg->rate = 13;
    cfg->up_mask = 0x3fff;
}

void k56call_init(k56call_t *c, const k56call_cfg *cfg)
{
    v8_menu_t cm;
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    c->state = K56C_V8BIS;
    c->bst = cfg->v8bis == K56C_V8BIS_NONE ? B_OFF : B_CRE;
    c->crd_at = -1; c->ms_at = -1; c->v8_sel = -1;
    c->data_at = -1; c->j24_at = -1; c->quiet_at = -1; c->e_at = -1;
    c->rn_at = -1; c->rate_now = cfg->rate; c->nb = 1;
    c->rt_at = -1;
    /* the CM of v34peer's own V.8; with cm_pcm, an analogue modem's PCM octet too */
    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA;
    cm.modes = V8_MOD_V34;
    cm.n_mod_octets = 3;
    if (cfg->cm_pcm) { cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG; cm.has_access = 1; }
    v8call_init(&c->v8, 8000, (int16_t)cfg->level, &cm);
}

k56c_state k56call_state(const k56call_t *c) { return c->state; }

static void listen_ch1(k56call_t *c)
{
    v21demod_init(&c->dem, V21L_MARK, V21L_SPACE, 8000, 300);
    c->squelch = v21demod_energy_of(&c->dem, 110);
    c->level = 1; c->clk = 0;
    v8bis_rx_init(&c->rx);
}

/* V.21 channel 1: a frame's octet count when one ends, -1 an invalid one, 0 */
static int listen_tick(k56call_t *c, int16_t x)
{
    int level = v21demod_tick(&c->dem, x);
    if (c->dem.energy < c->squelch) level = 1;
    if (level != c->level) { c->level = level; c->clk = 4000; }
    c->clk += 300;
    if (c->clk < 8000) return 0;
    c->clk -= 8000;
    return v8bis_rx_bit(&c->rx, level, c->msg);
}

/* our CL's NS block, after the S field's data Par(2) block if there is one */
static int cl_ok(const uint8_t *o, int n)
{
    int i = 5;
    if (n < 6 || (o[0] & 15) != V8B_MSG_CL) return 0;
    if (o[4] & 1) { while (i < n && !(o[i] & 0x40)) i++; i++; }
    return i < n && v8bis_cl_is_k56(o + i, n - i);
}

/* the initiator's dual tone (1375 + 2002 Hz) in 10 ms blocks; 1 at a
 * block's end with it, 0 without, -1 within a block. quiet: the block's
 * RMS was below 50. */
static int dual_block(k56call_t *c, int16_t x, int *quiet)
{
    static const double f[2] = { 1375, 2002 };
    int k, on = 1;
    for (k = 0; k < 2; k++) {
        c->gr[k] += x * cos(c->gph[k]); c->gi[k] += x * sin(c->gph[k]);
        c->gph[k] += TWO_PI * f[k] / 8000;
        if (c->gph[k] > TWO_PI) c->gph[k] -= TWO_PI;
    }
    c->gpw += (double)x * x;
    if (++c->gn < BLOCK) return -1;
    /* a tone's share of the block's power: 2 |sum|^2 / (N pw) */
    for (k = 0; k < 2; k++) {
        double sh = 2 * (c->gr[k] * c->gr[k] + c->gi[k] * c->gi[k]) / (BLOCK * (c->gpw + 1));
        if (sh < 0.25) on = 0;
        c->gr[k] = c->gi[k] = 0;
    }
    *quiet = c->gpw < (double)BLOCK * 2500;
    if (*quiet) on = 0;
    c->gpw = 0; c->gn = 0;
    return on;
}

static int16_t responder(k56call_t *c, int16_t x)
{
    int r, quiet = 0, done;
    double y = 0;
    switch (c->bst) {
    case B_CRE:
        r = dual_block(c, x, &quiet);
        if (r >= 0) c->gblocks = r ? c->gblocks + 1 : 0;
        if (c->gblocks >= 10) { c->got_cre = 1; c->bst = B_CRE_IN; }
        break;
    case B_CRE_IN:
        /* its second segment over: CRd 175 ms later (SM56's) */
        if (dual_block(c, x, &quiet) >= 0 && quiet) { c->crd_at = c->now + MS(175); c->bst = B_CRD; }
        break;
    case B_CRD:
        if (c->now >= c->crd_at) {
            int32_t k = c->now - c->crd_at;
            if (k < MS(400)) {
                y = c->cfg.level * (sin(c->ph[0]) + sin(c->ph[1]));
                c->ph[0] += TWO_PI * 1529 / 8000; c->ph[1] += TWO_PI * 2225 / 8000;
            } else if (k < MS(500)) {
                y = c->cfg.level * 1.41421356 * sin(c->ph[2]);
                c->ph[2] += TWO_PI * 1900 / 8000;
            } else {
                listen_ch1(c); c->bst = B_CL;
            }
        }
        break;
    case B_CL:
        r = listen_tick(c, x);
        if (r > 0 && (c->msg[0] & 15) == V8B_MSG_CL) {
            c->got_cl = 1; c->cl_ok = cl_ok(c->msg, r);
            listen_ch1(c);
            if (c->cfg.v8bis == K56C_V8BIS_NO_MS || !c->cl_ok) { c->bst = B_ACK; break; }
            c->nbits = v8bis_frame(sm56_ms, 13, c->bits);
            /* a bad FCS: octet 0's first bit (a one) sent as a zero */
            if (c->cfg.v8bis == K56C_V8BIS_BAD_FCS) c->bits[32 + 24] = 0;
            c->pos = 0; c->ms_at = c->now + MS(10);
            v21mod_init(&c->mod, V21H_MARK, V21H_SPACE, 8000, 300, (int16_t)(c->cfg.level * 181 / 128));
            c->bst = B_MS;
        }
        break;
    case B_MS:
        if (c->now < c->ms_at) break;
        y = v21mod_tick(&c->mod, c->bits[c->pos], &done);
        if (done && ++c->pos >= c->nbits) { c->sent_ms = 1; c->bst = B_ACK; }
        break;
    case B_ACK:
        r = listen_tick(c, x);
        if (r == 1 && c->msg[0] == 0x14) c->got_ack = 1;
        if (r == 1 && c->msg[0] == 0x18) c->got_nak = 1;
        if (r != 0) c->bst = B_OFF;
        break;
    default:
        break;
    }
    return (int16_t)lround(y);
}

/* ==== K56flex, from our flagged INFO1a on (k56-facts.md) ==== */

#define R_MAG       0x54
#define ONE_MAG     0x4f
#define SIL_ARM     256         /* octets of silence before R1 is looked for */
#define P3_TRN      4000        /* its Phase 3 TRN, T ("echo delay + 4000 to 6430 T") */
#define P4_TRN      1280        /* its Phase 4 TRN, T (16 points) */
#define J24_SYMS    12
#define B1_FRAMES   6
#define PATIENCE    6000        /* its J24 to our R2: about 0.75 s ("Training down") */
#define RN_TRN      512         /* its renegotiation's TRN, T (4 points, as modem/v34dx's) */

/* SIG_RTRN: a renegotiation's TRN; there, TRN, MP, MP' and E are on 4 points
 * (V.34 11.6, as modem/v34dx sends them) */
enum { SIG_SIL, SIG_S, SIG_SBAR, SIG_PP, SIG_TRN, SIG_J24, SIG_JP, SIG_TRN16, SIG_MP, SIG_E, SIG_DATA, SIG_RTRN };

static const uint8_t trn1b_mag[8] = { 0x40, 0x41, 0x4b, 0x4c, 0x50, 0x51, 0x50, 0x51 };
static const uint8_t pad2_mag[3][4] = {
    { 0x2f, 0x45, 0x51, 0x57 }, { 0x35, 0x4d, 0x58, 0x60 }, { 0x3d, 0x54, 0x61, 0x67 }
};
static const uint8_t pad1_mag[3][2] = { { 0x3d, 0x55 }, { 0x44, 0x5d }, { 0x4c, 0x65 } };

int k56call_down_rate(const k56call_t *c) { return c->data_at >= 0 && c->r_used ? 30000 + 2000 * c->r_used : 0; }

static int pad_of(const k56call_t *c) { return c->cfg.pad_mode >= 0 && c->cfg.pad_mode <= 2 ? c->cfg.pad_mode : 0; }

/* this line sample's octet has its last bit robbed */
static int robbed_now(const k56call_t *c) { return (c->cfg.rbs >> (c->line_n % 6)) & 1; }

/* what octet o, sent now, arrives as: the pad, then the robbed bit */
static uint8_t arrive(const k56call_t *c, uint8_t o)
{
    uint8_t a = x2call_pad(o, pad_of(c));
    if (robbed_now(c)) a |= 1;
    return a;
}

static uint8_t mag_octet(int m, int positive) { return (uint8_t)(positive ? 0xff - m : 0x7f - m); }

/* the octet sent among magnitudes mags[0..n) (both signs) that arrives as r:
 * its index, its sign in *pos; -1 none. A robbed bit can make two of them
 * arrive alike (TRN1A's 0x4e and 0x4f, TRN1B's pairs), before the server
 * knows the line: the real client averages over them ("Robbed bits"); this
 * one, to go on checking the server's sequence and its running sum, breaks
 * the tie with the octet as it left the server (c->sent), and counts it bad
 * when that is neither */
static int which(const k56call_t *c, uint8_t r, const uint8_t *mags, int n, int *pos)
{
    int k, s, found = -1, fpos = 0, ties = 0;
    for (k = 0; k < n; k++)
        for (s = 0; s < 2; s++)
            if (arrive(c, mag_octet(mags[k], s)) == r) {
                if (found < 0) { found = k; fpos = s; }
                else ties = 1;
            }
    if (ties) {
        found = -1;
        for (k = 0; k < n && found < 0; k++)
            for (s = 0; s < 2; s++)
                if (mag_octet(mags[k], s) == c->sent && arrive(c, c->sent) == r) { found = k; fpos = s; break; }
    }
    *pos = fpos;
    return found;
}

static void bad(k56call_t *c) { if (c->dseg < K56TX_DATA) c->seg_bad[c->dseg]++; }

/* a sent code into the running sum (of what was sent) */
static void add_sum(k56call_t *c, int m, int pos) { c->sum += v90_octet_linear(0, mag_octet(m, pos)); }

/* the sign of a symbol set against the running sum (before it) */
static int sum_sign(const k56call_t *c) { return c->sum <= 0; }

/* one scrambled training bit: into the history and the descrambler; the
 * input bit once 23 known bits are in, else -1 */
static int tbit(k56call_t *c, int b)
{
    int d = v90_descramble_gpa(&c->tdscr, b);
    c->hist = c->hist << 1 | (uint32_t)b;
    return c->tknown++ >= 23 ? d : -1;
}

/* ---- the server's MP and MP' ---- */

static void our_mp(k56call_t *c, const v34_mp_t *m)
{
    int up_n = 0, n;
    /* what it must be: type 0, our rate code and its up rate, a mask,
     * asymmetric, for our transmitter 16 states, linear, minimum shaping */
    for (n = 13; n >= 1; n--) if ((c->cfg.up_mask >> (n - 1)) & 1) { up_n = n; break; }
    if (m->type || !m->max_ac || m->max_ac > 13 || m->max_ac > c->rate_now || !m->max_ca || m->max_ca > up_n
        || !m->rates || !m->asym || m->trellis || m->nonlinear || m->shaping) {
        c->mp_wrong++;
        return;
    }
    if (m->ack) c->mpa_good++; else c->mp_good++;
    c->our_mp = *m;
    if (!c->got_our_mp) c->r_used = m->max_ac < c->rate_now ? m->max_ac : c->rate_now;
    c->got_our_mp = 1;
    if (m->ack) c->got_our_mpa = 1;
}

/* the descrambled bits of MP and MP': frame sync (17 ones), then the
 * sequence (as v34dx collects them) */
static void mp_bit(k56call_t *c, int bit)
{
    if (c->mpfn > 0 && c->mpfn < (int)sizeof c->mpb) c->mpb[c->mpfn++] = (uint8_t)bit;
    if (c->mpfn >= V34_MP0_BITS) {
        v34_mp_t m;
        memset(&m, 0, sizeof m);
        if (v34_mp_parse(c->mpb, c->mpfn, &m) == V34_MP0_BITS) our_mp(c, &m);
        c->mpfn = 0;
    }
    if (bit) { c->mpones++; return; }
    if (c->mpones >= 17 && c->mpfn == 0) { memset(c->mpb, 1, 17); c->mpb[17] = 0; c->mpfn = 18; }
    c->mpones = 0;
}

/* ---- data ---- */

/* the octet sent (a point of the constellation of the rate and pad) that
 * arrives as r here; r itself when none does */
static uint8_t data_sent(const k56call_t *c, uint8_t r)
{
    int ru = c->r_used ? c->r_used : 1;     /* the server's MP was never accepted: any rate */
    const k56_rate_t *t = &k56_rates[ru - 1];
    const uint8_t *bios = k56_bios(ru, pad_of(c));
    int n = t->M[pad_of(c)] << (t->u - 1), k, s;
    /* on a robbed symbol the server sends only points with bit 0 clear
     * ("Robbed bits"; none to rob at q = 0) */
    int step = robbed_now(c) && t->u > 1 ? 2 : 1;
    for (k = 0; k < n; k += step)
        for (s = 0; s < 2; s++)
            if (arrive(c, mag_octet(bios[k], s)) == r) return mag_octet(bios[k], s);
    return r;
}

static void data_frame(k56call_t *c)
{
    uint8_t bits[64];
    int n = k56enc_unframe(&c->dec, c->dfr, bits), i;
    if (n < 0) { if (c->dframes < B1_FRAMES) c->b1_bad++; else c->data_lost++; c->dframes++; return; }
    for (i = 0; i < n; i++) bits[i] = (uint8_t)v90_descramble_gpa(&c->ddscr, bits[i]);
    if (c->dframes < B1_FRAMES) {
        /* B1: ones once the descrambler has its 23 bits; dropped */
        int from = c->dframes == 0 ? 23 : 0;
        for (i = from; i < n; i++) if (!bits[i]) { c->b1_bad++; break; }
    } else {
        c->data_frames++;
        if (c->cfg.putbit) for (i = 0; i < n; i++) c->cfg.putbit(c->cfg.user, bits[i]);
    }
    c->dframes++;
}

/* ---- the server's training, octet by octet ---- */

static void go_seg(k56call_t *c, int seg) { c->dseg = seg; c->dn = 0; }

/* R1 / R2: 0x54, signs + + - -, the last 24 inverted */
static void r_octet(k56call_t *c, uint8_t r)
{
    int pos = !((c->dn >> 1) & 1);
    if (c->dn >= 264) pos = !pos;
    if (arrive(c, mag_octet(R_MAG, pos)) != r) bad(c);
    if (++c->dn == 288) {
        if (c->dseg == K56TX_R1) go_seg(c, K56TX_TRN1A);
        else { go_seg(c, K56TX_TRN2A); c->alt = 0; c->tones = 0; c->trn2a_seen = 1; }
    }
}

/* TRN1C, Js, TRN2A, MARK: even symbols 0x4f with a scrambled bit as the
 * sign (want: the input bit, -1 anything); odd ones 0x4f against the sum.
 * Returns the even symbol's descrambled bit (-1 unknown or odd). */
static int one_octet(k56call_t *c, uint8_t r, int want)
{
    static const uint8_t one[1] = { ONE_MAG };
    int pos, d = -1;
    if (which(c, r, one, 1, &pos) < 0) { bad(c); c->tknown = 0; return -1; }
    if (!(c->alt & 1)) {
        d = tbit(c, pos);
        if (want >= 0 && d >= 0 && d != want) bad(c);
        /* at 2 bits a symbol the second, a scrambled zero, is not on the
         * line: what GPA makes of it from the history */
        if (c->nb == 2) (void)tbit(c, (int)(((c->hist >> 4) ^ (c->hist >> 22)) & 1));
    } else if (pos != sum_sign(c)) {
        bad(c);
    }
    add_sum(c, ONE_MAG, pos);
    c->alt++;
    return d;
}

/* TRN2B, MP, MP', E: 2 scrambled bits a symbol on the pad's 4 magnitudes;
 * even: negative when hist bit 24 is set; odd: against the sum. The two
 * input bits in in[], -1 unknown; 0 when the octet is none of them. */
static int p4_octet(k56call_t *c, uint8_t r, int in[2])
{
    int pos, k = which(c, r, pad2_mag[pad_of(c)], 4, &pos);
    in[0] = in[1] = -1;
    if (k < 0) { bad(c); c->tknown = 0; return 0; }
    in[0] = tbit(c, k & 1); in[1] = tbit(c, k >> 1);
    if (!(c->alt & 1) ? pos != !((c->hist >> 24) & 1) : pos != sum_sign(c)) bad(c);
    add_sum(c, pad2_mag[pad_of(c)][k], pos);
    c->alt++;
    return 1;
}

/* a renegotiation's MP, MP' and E: 1 scrambled bit a symbol on the pad's 2
 * magnitudes; signs as p4_octet's. The input bit in *in, -1 unknown; 0 when
 * the octet is neither of them. */
static int p1_octet(k56call_t *c, uint8_t r, int *in)
{
    int pos, k = which(c, r, pad1_mag[pad_of(c)], 2, &pos);
    *in = -1;
    if (k < 0) { bad(c); c->tknown = 0; return 0; }
    *in = tbit(c, k);
    if (!(c->alt & 1) ? pos != !((c->hist >> 24) & 1) : pos != sum_sign(c)) bad(c);
    add_sum(c, pad1_mag[pad_of(c)][k], pos);
    c->alt++;
    return 1;
}

/* R in place of a data frame: the server's renegotiation (R2 onward, its
 * scrambler, history and running sum going on from its last E) */
static void r2_begin(k56call_t *c)
{
    if (!c->rn) c->r_unasked++;
    go_seg(c, K56TX_R2);
    c->dn = K56ENC_N;               /* the frame's 8 octets were R's */
    c->rn_down = 1;
}

static void data_begin(k56call_t *c)
{
    uint8_t rbs = (uint8_t)(c->j24[16] | c->j24[17] << 1 | c->j24[18] << 2 | c->j24[19] << 3 | c->j24[20] << 4 | c->j24[21] << 5);
    go_seg(c, K56TX_DATA);
    c->data_at = c->line_n + 1;         /* (this octet is E's last) */
    if ((c->data_at - c->trn1b_at) % 24) c->seg_bad[K56TX_TRN2B]++;     /* the grid of 24 */
    k56enc_init(&c->dec, c->r_used ? c->r_used : 1, pad_of(c), rbs);
    v90_scr_init(&c->ddscr);
    c->dfpos = 0; c->dframes = 0; c->rmatch = 0;
    c->rn_down = 0;
}

static void down_octet(k56call_t *c, uint8_t r)
{
    int pos, k, in[2], d;
    switch (c->dseg) {
    case K56TX_SIL1:
        if (r == 0xff) { c->dn++; break; }
        if (c->dn < SIL_ARM) { c->dn = 0; break; }      /* the tail of INFO1a */
        /* R1: the scrambler, its history and the running sum start here */
        go_seg(c, K56TX_R1);
        v90_scr_init(&c->tdscr); c->hist = 0; c->sum = 0; c->tknown = 0;
        r_octet(c, r);
        break;
    case K56TX_R1:
    case K56TX_R2:
        r_octet(c, r);
        break;
    case K56TX_TRN1A: {
        static const uint8_t m[2] = { 0x4e, 0x4f };
        k = which(c, r, m, 2, &pos);
        if (k < 0) { bad(c); c->tknown = 0; }
        else {
            d = tbit(c, pos);
            if (d == 0 || k != (int)((c->hist >> 24) & 1)) bad(c);
            add_sum(c, m[k], pos);
        }
        if (++c->dn == K56TX_TRN1A_N) { go_seg(c, K56TX_TRN1B); c->alt = 0; c->trn1b_at = c->line_n + 1; }
        break;
    }
    case K56TX_TRN1B:
        /* 3 scrambled bits a symbol, not all of them seen: the magnitudes,
         * and the odd signs against the sum */
        k = which(c, r, trn1b_mag, 6, &pos);
        if (k < 0) bad(c);
        else {
            if ((c->alt & 1) && pos != sum_sign(c)) bad(c);
            add_sum(c, trn1b_mag[k], pos);
        }
        c->tknown = 0;
        c->alt++;
        if (++c->dn == K56TX_TRN1B_N) go_seg(c, K56TX_TRN1C);
        break;
    case K56TX_TRN1C:
        (void)one_octet(c, r, 1);
        if (++c->dn == K56TX_TRN1C_N) { go_seg(c, K56TX_JS); c->js_ok = 1; }
        break;
    case K56TX_JS:
        d = one_octet(c, r, -1);
        if (!(c->dn & 1) && d != v34_j16[(c->dn >> 1) & 15]) c->js_ok = 0;
        if (++c->dn == K56TX_JS_N) {
            go_seg(c, K56TX_SIL2);
            if (c->js_ok) c->js_seen = 1;           /* our Phase 3 */
        }
        break;
    case K56TX_SIL2:
        if (r == 0xff) { c->dn++; break; }
        c->sil2_n = c->dn;
        if (c->dn < K56TX_SIL2_MIN || (c->dn & 7)) bad(c);
        if (c->j24_at < 0 || c->line_n - c->j24_at > PATIENCE) bad(c);
        go_seg(c, K56TX_R2);
        r_octet(c, r);
        break;
    case K56TX_TRN2A:
        d = one_octet(c, r, -1);
        c->dn++;
        if (d == 1) c->tones++;
        else if (d == 0) {
            /* 16 ones, then MARK's zeros */
            if (c->tones < 16) bad(c);
            c->trn2a_n = c->dn - 1;
            if (c->trn2a_n != (c->rn_down ? K56TX_RN_TRN2A_N : K56TX_TRN2A_N)) bad(c);
            go_seg(c, K56TX_MARK); c->dn = 1;
        }
        break;
    case K56TX_MARK:
        (void)one_octet(c, r, 0);
        if (++c->dn == K56TX_MARK_N) { go_seg(c, K56TX_TRN2B); c->nb = 2; }
        break;
    case K56TX_TRN2B:
        if (c->rn_down && which(c, r, pad1_mag[pad_of(c)], 2, &pos) >= 0) {
            /* a renegotiation's MP, 1 bit a symbol: its frame sync from here */
            c->trn2b_n = c->dn;
            if (c->dn < K56TX_TRN2B_MIN) c->seg_bad[K56TX_TRN2B]++;
            if ((c->line_n - c->trn1b_at + K56TX_RN_TO_DATA) % 24) c->seg_bad[K56TX_TRN2B]++;
            go_seg(c, K56TX_MP);
            c->nb = 1;
            c->mpfn = 0; c->mpones = 0;
            if (p1_octet(c, r, &d) && d >= 0) mp_bit(c, d);
            if (d != 1) bad(c);
            c->dn = 1;
            break;
        }
        if (!p4_octet(c, r, in)) { c->dn++; break; }
        if (in[0] == 1 && in[1] == 1) {
            /* the MP's frame sync: MP from this symbol */
            c->trn2b_n = c->dn;
            if (c->dn < K56TX_TRN2B_MIN) c->seg_bad[K56TX_TRN2B]++;
            if ((c->line_n - c->trn1b_at + K56TX_TO_DATA) % 24) c->seg_bad[K56TX_TRN2B]++;
            go_seg(c, K56TX_MP);
            c->mpfn = 0; c->mpones = 0;
            mp_bit(c, 1); mp_bit(c, 1);
            c->dn = 1;
            break;
        }
        if (in[0] >= 0 && (in[0] != 1 || in[1] != 0)) bad(c);
        c->dn++;
        break;
    case K56TX_MP:
    case K56TX_MPA:
        if (c->rn_down) { if (p1_octet(c, r, &d) && d >= 0) mp_bit(c, d); }
        else if (p4_octet(c, r, in) && in[0] >= 0) { mp_bit(c, in[0]); mp_bit(c, in[1]); }
        if (++c->dn == K56TX_MP_N) go_seg(c, c->dseg + 1);
        break;
    case K56TX_E:
        if (c->rn_down) { if (p1_octet(c, r, &d) && d != 1) bad(c); }
        else if (p4_octet(c, r, in) && (in[0] != 1 || in[1] != 1)) bad(c);
        if (++c->dn == (c->rn_down ? K56TX_RN_E_N : K56TX_E_N)) {
            c->e_seen = c->seg_bad[K56TX_E] == 0;
            data_begin(c);
        }
        break;
    default:    /* data: B1 first; R in place of a frame, the server's renegotiation */
        if (arrive(c, mag_octet(R_MAG, !((c->dfpos >> 1) & 1))) == r) c->rmatch++;
        c->dfr[c->dfpos++] = data_sent(c, r);
        if (c->dfpos == K56ENC_N) {
            c->dfpos = 0;
            if (c->rmatch == K56ENC_N) r2_begin(c); else data_frame(c);
            c->rmatch = 0;
        }
        break;
    }
}

/* ---- its upstream: V.34's call modem ---- */

static void strike(k56call_t *c, int when)
{
    if (c->cfg.silent == when && !c->quiet) { c->quiet = 1; c->quiet_at = c->now; }
}

static void build_mp(k56call_t *c, int ack)
{
    v34_mp_t m;
    memset(&m, 0, sizeof m);
    m.max_ca = 13;
    m.max_ac = (uint8_t)(c->cfg.mp_bad == K56C_MP_RATE0 ? 0 : c->rate_now);
    if (c->rn && c->cfg.reneg_rate0) m.max_ca = m.max_ac = 0;     /* V.34's cleardown (11.7) */
    m.rates = c->cfg.mp_bad == K56C_MP_MASK0 ? 0 : c->cfg.mp_bad == K56C_MP_UP_NONE ? 0x2000 : c->cfg.up_mask;
    m.asym = 1;
    m.ack = (uint8_t)ack;
    if (v34_mp_build(&m, c->mpbits) != V34_MP0_BITS) memset(c->mpbits, 0, sizeof c->mpbits);
    c->mp_ack = ack; c->mpn = 0;
}

static void go_sig(k56call_t *c, int sig) { c->sig = sig; c->sn = 0; }

/* its renegotiation, due at this data symbol: S, S-bar, TRN, MP asking
 * rate_now, MP' once it has our MP, E once it has our MP', then data */
static int rn_due(const k56call_t *c)
{
    if (c->sig != SIG_DATA) return 0;
    if (c->renegs == 0 && c->cfg.reneg_ms) return c->sn == (int32_t)c->cfg.reneg_ms * 16 / 5;
    if (c->renegs == 1 && c->cfg.reneg_ms2) return c->sn == (int32_t)c->cfg.reneg_ms2 * 16 / 5;
    return 0;
}

static void rn_begin(k56call_t *c)
{
    c->rn = 1; c->renegs++; c->rn_at = c->now;
    c->rate_now = c->renegs == 1 ? c->cfg.reneg_rate : c->cfg.reneg_rate2;
    c->got_our_mp = c->got_our_mpa = 0; c->mps_sent = 0;
    go_sig(c, SIG_S);
}

static void tx_symbol(k56call_t *c, int32_t *re, int32_t *im)
{
    v34_pt_t pt;
    int32_t g = 16384;
    int k;
    *re = *im = 0;
    if (rn_due(c)) rn_begin(c);
    switch (c->sig) {
    case SIG_SIL:
        if (!c->js_seen) return;
        strike(c, K56C_SIL_BEFORE_S);
        go_sig(c, SIG_S);
        /* fall through */
    case SIG_S:
        v34_s(c->sn, V34_S_LEN, re, im);
        if (++c->sn == V34_S_LEN) {
            if (c->rn) strike(c, K56C_SIL_RENEG);
            go_sig(c, SIG_SBAR);
        }
        return;
    case SIG_SBAR:
        v34_sbar(c->sn, re, im);
        if (++c->sn == V34_SBAR_LEN) {
            if (c->rn) { v34_tmod_init(&c->tmod, V34_GPC, 4); go_sig(c, SIG_RTRN); }
            else go_sig(c, SIG_PP);
        }
        return;
    case SIG_RTRN:
        pt = v34_tmod_trn(&c->tmod);
        if (c->cfg.retrain_in_reneg && !c->retrains && c->sn == RN_TRN / 2) c->rt_due = 1;   /* it gives up */
        if (++c->sn == RN_TRN) { build_mp(c, 0); go_sig(c, SIG_MP); }
        break;
    case SIG_PP:
        v34_pp(c->sn, re, im);
        if (++c->sn == V34_PP_LEN) { v34_tmod_init(&c->tmod, V34_GPC, 4); go_sig(c, SIG_TRN); }
        return;
    case SIG_TRN:
        pt = v34_tmod_trn(&c->tmod);
        if (++c->sn == P3_TRN) { strike(c, K56C_SIL_BEFORE_J); go_sig(c, SIG_J24); }
        break;
    case SIG_J24:
        k = (int)c->sn * 2;
        pt = v34_tmod_put(&c->tmod, (unsigned)(c->j24[k] | (c->j24[k + 1] << 1)));
        if (++c->sn == J24_SYMS) {
            if (c->j24_at < 0) c->j24_at = c->now;
            c->sn = 0;
            if (c->trn2a_seen) { strike(c, K56C_SIL_BEFORE_MP); go_sig(c, SIG_JP); }
        }
        break;
    case SIG_JP:
        k = (int)c->sn * 2;
        pt = v34_tmod_put(&c->tmod, (unsigned)(v34_jprime[k] | (v34_jprime[k + 1] << 1)));
        if (++c->sn == 8) { v34_tmod_init(&c->tmod, V34_GPC, 16); go_sig(c, SIG_TRN16); }
        break;
    case SIG_TRN16:
        pt = v34_tmod_trn(&c->tmod); g = c->g16;
        if (++c->sn == P4_TRN) { build_mp(c, 0); go_sig(c, SIG_MP); }
        break;
    case SIG_MP: {
        unsigned b = 0;
        int bps = c->rn ? 2 : 4;
        for (k = 0; k < bps; k++) b |= (unsigned)c->mpbits[c->mpn + k] << k;
        pt = v34_tmod_put(&c->tmod, b);
        if (!c->rn) g = c->g16;
        c->mpn += bps;
        if (c->mpn >= V34_MP0_BITS) {
            c->mps_sent++;
            if ((c->mp_ack && c->got_our_mpa) || (c->cfg.early_e && c->mps_sent >= c->cfg.early_e)) {
                v34_cfg_t vc;
                strike(c, K56C_SIL_BEFORE_E);
                memset(&vc, 0, sizeof vc);
                vc.symbol_rate = 3200; vc.data_rate = (uint16_t)(c->our_mp.max_ca * 2400);
                vc.trellis = c->our_mp.trellis; vc.expanded = c->our_mp.shaping;
                vc.nonlinear = c->our_mp.nonlinear; vc.scrambler = V34_GPC;
                if (!c->got_our_mpa) {          /* early E: what our MP' will ask for */
                    vc.data_rate = 31200;     /* the client's best at 3200 baud */
                    vc.trellis = V34_TRELLIS_16; vc.expanded = 0; vc.nonlinear = 0;
                }
                if (c->cfg.heavy_up == K56C_HEAVY_ALWAYS || (c->cfg.heavy_up == K56C_HEAVY_FIRST && !c->renegs)) {
                    /* what our MP asked is ignored */
                    vc.trellis = V34_TRELLIS_64; vc.expanded = 1; vc.nonlinear = 1;
                }
                if (v34_params_init(&c->tpar, &vc) == 0) {
                    c->up_rate = vc.data_rate;
                    c->tx_dg = (int32_t)v34_gain_q14(c->tpar.energy_tx_q8);
                    c->e_at = c->now;
                    go_sig(c, SIG_E);
                } else {
                    go_sig(c, SIG_SIL); c->js_seen = 0;     /* nothing it can send */
                }
            } else {
                build_mp(c, c->mp_ack || c->got_our_mp);
            }
        }
        break;
    }
    case SIG_E:
        if (c->rn) pt = v34_tmod_put(&c->tmod, 3);
        else { pt = v34_tmod_put(&c->tmod, 15); g = c->g16; }
        if (++c->sn * (c->rn ? 2 : 4) >= 20) {
            v34_tx_init(&c->vtx, &c->tpar, c->cfg.getbit, c->cfg.user);
            c->rn = 0;
            go_sig(c, SIG_DATA);
        }
        break;
    default:    /* SIG_DATA: B1, then data */
        v34_tx_symbol(&c->vtx, &pt, NULL); g = c->tx_dg;
        if (c->cfg.b1_bad && c->sn < 400 && c->sn % 3 == 0) { pt.re = (int16_t)-pt.re; pt.im = (int16_t)-pt.im; }  /* not all: a 180 degree turn alone is differentially coded away */
        if (++c->sn == 3200) strike(c, K56C_SIL_DATA);
        if (c->retrains < c->cfg.retrains && c->cfg.retrain_ms && c->sn == (int32_t)c->cfg.retrain_ms * 16 / 5)
            c->rt_due = 1;
        break;
    }
    *re = (pt.re * g) >> 9; *im = (pt.im * g) >> 9;
}

/* the flagged INFO1a has come: silent until our Js; the J24 it will send.
 * After a retrain the whole training again: what it found so far is kept
 * (seg_bad, the MP counts, the data counts, ...), the rest starts afresh. */
static void k56_begin(k56call_t *c)
{
    const info1a_t *i = &c->peer.p2.info1a;
    int idx = i->down_sr, num, den, k;
    if (c->k56_starts++) {
        c->alt = 0; c->trn1b_at = 0; c->sum = 0; c->hist = 0; c->tknown = 0; c->tones = 0;
        c->mpfn = c->mpones = 0;
        memset(&c->our_mp, 0, sizeof c->our_mp);
        c->got_our_mp = c->got_our_mpa = 0; c->r_used = 0;
        c->dfpos = 0; c->dframes = 0; c->data_at = -1; c->rmatch = 0;
        c->sig = SIG_SIL; c->sn = 0; c->mpn = 0; c->mp_ack = 0; c->mps_sent = 0;
        c->js_seen = c->trn2a_seen = 0; c->j24_at = -1; c->up_rate = 0; c->e_at = -1;
        c->rate_now = c->cfg.rate; c->rn = 0; c->rn_down = 0; c->nb = 1;
    }
    v34_carrier(idx, i->hi_carrier, &num, &den);
    (void)v34mod_init(&c->umod, v34_symbol_rate(idx), num, den, i->preemph > 10 ? 0 : i->preemph,
                      c->cfg.level, i->min_pwr_red);
    c->g16 = (int32_t)v34_gain_q14(V34_ENERGY_16PT_Q8);
    c->dseg = K56TX_SIL1; c->dn = 0;
    for (k = 0; k < 16; k++) c->j24[k] = v34_j16[k];
    c->state = K56C_K56;
}

/* the J24's octet, once TRN1B has shown where the 6-cycle starts */
static void set_octet(k56call_t *c)
{
    int o = c->cfg.j_octet, k;
    if (o < 0) {
        o = pad_of(c) << 6;
        for (k = 0; k < 6; k++) if ((c->cfg.rbs >> ((c->trn1b_at + k) % 6)) & 1) o |= 1 << k;
    }
    for (k = 0; k < 8; k++) c->j24[16 + k] = (uint8_t)((o >> k) & 1);
}

/* its retrain (V.34 11.5): v34peer's, from its 70 ms of silence; after our
 * INFO1a, flagged again, K56flex's training from the start (k56_begin) */
static void rt_begin(k56call_t *c)
{
    c->rt_due = 0; c->retrains++; c->rt_at = c->now;
    if (c->cfg.retrain_200) c->peer.cfg.p2.k56 = P2C_K56_DECLINE;
    c->peer.state = VP_RETRAIN; c->peer.t = 0;
    c->state = K56C_V34;
}

static int16_t k56_tick(k56call_t *c, uint8_t octet)
{
    if (c->dseg == K56TX_TRN1B && c->dn == 0) set_octet(c);
    c->sent = octet;
    down_octet(c, arrive(c, octet));
    if (v34mod_due(&c->umod)) {
        int32_t re, im;
        tx_symbol(c, &re, &im);
        v34mod_put(&c->umod, re, im);
    }
    if (c->rt_due) { rt_begin(c); return 0; }
    return v34mod_tick(&c->umod);
}

int16_t k56call_tick(k56call_t *c, uint8_t octet)
{
    int16_t x = (int16_t)ulaw_to_linear(octet), out = 0, v = 0;
    int32_t s;
    c->now++; c->line_n++;
    switch (c->state) {
    case K56C_V8BIS:
    case K56C_V8:
        if (c->bst != B_OFF) out = responder(c, x);
        if (v8call_tick(&c->v8, x, &v) != V8C_WAIT) c->state = K56C_V8;
        if (c->v8.state == V8C_DONE) {
            c->v8_sel = v8_selected(&c->v8.jm);
            if (c->v8_sel == V8_SEL_V34) {
                /* as v34peer after its own V.8: Phase 2 from the next sample */
                v34peer_cfg pc;
                v34peer_default(&pc, c->cfg.level);
                pc.skip_v8 = 1;
                pc.dx.getbit = c->cfg.getbit; pc.dx.putbit = c->cfg.putbit; pc.dx.user = c->cfg.user;
                if (c->cfg.up_sr_3000) pc.p2.info0c.lo3200 = pc.p2.info0c.hi3200 = 0;
                if (c->got_ack) {               /* agreed in V.8bis */
                    pc.k56 = 1;
                    pc.p2.k56 = c->cfg.info1c_k56 ? P2C_K56_ACCEPT : P2C_K56_DECLINE;
                }
                v34peer_init(&c->peer, &pc);
                c->state = K56C_V34;
            } else {
                c->state = K56C_IDLE;
            }
        }
        break;
    case K56C_V34:
        v = v34peer_tick(&c->peer, x);
        /* its retrain: 70 ms of silence, then tone B */
        if (c->retrains && c->now - c->rt_at >= MS(70 + 200)) strike(c, K56C_SIL_RETRAIN);
        if (c->quiet) v = 0;
        if (c->peer.state == VP_ENDED) c->state = K56C_ENDED;
        else if (c->peer.state == VP_K56) k56_begin(c);
        break;
    case K56C_K56:
        v = k56_tick(c, octet);
        if (c->quiet) v = 0;
        break;
    default:
        break;
    }
    s = (int32_t)out + v;
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}
