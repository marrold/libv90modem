/* Test peer: an x2 client. See x2call.h. Written from
 * docs/history/x2-facts.md; no code of D-Modem or courier-emu. */
#include <string.h>
#include "x2call.h"
#include "g711.h"
#include "../modem/p2ans.h"
#include "../modem/v8msg.h"
#include "../modem/v34train.h"
#include "../modem/v34mp.h"
#include "../modem/x2rec.h"
#include "../modem/x2tab.h"

#define MS(ms) ((int32_t)(ms) * 8)
#define MARKER 0x4d             /* high carrier, index 6, 3200 baud: the Courier's */
#define SYM_IDX 4
#define P3_TRN 6400             /* symbols of Phase 3 TRN (the Courier's, about 2 s) */
#define P4_TRN 5120             /* ... of Phase 4 TRN, the first P4_TRN4 on 4 points */
#define P4_TRN4 512
#define J_STOP 1200             /* J until the server's preamble has been there 150 ms */
#define MP_BITS 104             /* a record of 4 words and 2 zeros: 26 symbols on 16 points */
#define START_FRAMES 681       /* the start segment: one frame in levels A, 680 in the data levels */

/* the server's script, octets from its preamble's first (x2-facts.md "Phase 3 script") */
#define K_TRN   363
#define K_JW    (K_TRN + 20004)
#define K_ONES  (K_JW + 1200)
#define K_ALIGN (K_ONES + 6000)
#define K_SYNC  (K_ALIGN + 1152)

static const char pre_text[] = "ab bd ab 2b 3d 2b ab 2b 3d bd 3d bd ab bd 3d 2b 3d bd 3d 2b ab 2b";
static const uint8_t align_tab[6] = { 0x9e, 0x28, 0xa1, 0x21, 0xa8, 0x1e };

enum { SIG_SIL, SIG_S, SIG_SBAR, SIG_PP, SIG_TRN, SIG_J, SIG_TRN4, SIG_MP, SIG_E, SIG_DATA };

uint8_t x2call_pad(uint8_t o, int m)
{
    static const int32_t gain[3] = { 32768, 23198, 16423 };     /* x2-facts.md "Pads" */
    int32_t v, a;
    if (m <= 0 || m > 2) return o;
    v = ulaw_to_linear(o);
    a = v < 0 ? -v : v;
    a = (int32_t)(((int64_t)a * gain[m] + 16384) >> 15);
    return linear_to_ulaw(v < 0 ? -a : a);
}

uint8_t x2call_level(const x2map_t *m, int pos, uint8_t o)
{
    const uint8_t *rv, *sn;
    int n = m->levels_a ? 0 : x2map_list(m, pos, &rv, &sn), j;
    for (j = 0; j < n; j++)
        if (x2call_pad(sn[j], m->pad_mode) == (o | 0x80))
            return (uint8_t)((o & 0x80) ? rv[j] : (rv[j] & 0x7f));
    return o;
}

static int pad_of(const x2call_t *c) { return c->cfg.pad_mode >= 0 && c->cfg.pad_mode <= 2 ? c->cfg.pad_mode : 0; }

static uint8_t pre_octet(int k)
{
    int i = k < 352 ? k % 22 : k - 352;
    unsigned v = 0, d;
    const char *p = pre_text + 3 * i;
    for (d = 0; d < 2; d++) v = v * 16 + (unsigned)(p[d] <= '9' ? p[d] - '0' : p[d] - 'a' + 10);
    return (uint8_t)(k < 352 ? v : v ^ 0x80);
}

/* the MP: the start-up's, or the rate change's */
static void build_mp(x2call_t *c, int ack)
{
    uint16_t w[4];
    int n, index = c->renegs == 2 ? c->cfg.reneg_index2 : c->renegs ? c->cfg.reneg_index : c->cfg.max_index;
    int md = c->renegs ? c->cfg.reneg_md : c->cfg.md;
    w[0] = (uint16_t)(((index & 15) << 2) | (13 << 6) | ((c->cfg.pad_mode & 15) << 11));
    w[1] = c->renegs ? c->cfg.reneg_mask : c->cfg.up_mask;
    /* words 2 and 3 as the Courier's, MD (5) in word 3's high byte */
    w[2] = (uint16_t)(c->cfg.pad_mode ? 0x8888 : 0x0000);
    w[3] = (uint16_t)(((md ? md : 5) & 0xff) << 8 | (c->cfg.pad_mode ? 0x88 : 0x00));
    n = x2rec_build(w, 4, ack, c->mpbits);
    memset(c->mpbits + n, 0, (size_t)(MP_BITS - n));
    c->mp_ack = ack;
}

/* the upstream rate: the highest of mask, at most 28800 */
static void pick_up(x2call_t *c, uint16_t mask)
{
    int n;
    c->up_rate = 0;
    for (n = 12; n >= 1; n--) {
        v34_cfg_t vc;
        if (!((mask >> (n - 1)) & 1)) continue;
        memset(&vc, 0, sizeof vc);
        vc.symbol_rate = 3200; vc.data_rate = (uint16_t)(n * 2400);
        vc.trellis = V34_TRELLIS_64; vc.expanded = 1; vc.nonlinear = 1; vc.scrambler = V34_GPA;
        if (v34_params_init(&c->tpar, &vc) == 0) { c->up_rate = n * 2400; break; }
    }
    c->tx_dg = (int32_t)v34_gain_q14(c->tpar.energy_tx_q8);
}

void x2call_init(x2call_t *c, const x2call_cfg *cfg)
{
    v8_menu_t cm;
    int k, s;
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    if (!c->cfg.marker_ms) c->cfg.marker_ms = 170;
    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA;
    cm.modes = V8_MOD_V34;
    cm.n_mod_octets = 3;
    cm.has_access = 1;
    v8call_init(&c->v8, 8000, (int16_t)cfg->level, &cm);
    c->state = X2C_V8;
    c->mark_at = -1; c->mk_off = 0;
    c->k = -1; c->j_from = -1; c->data_at = -1; c->up_at = -1;
    dpsktx_init(&c->mk, DPSK_FC_B, cfg->level, 0);
    c->nmk = p2ans_x2_info_frame(MARKER, c->mkbits);
    /* what each levels A octet arrives as behind our pad */
    for (k = 0; k < 256; k++) c->ra[k] = -1;
    for (k = 0; k < X2_NLEV1; k++)
        for (s = 0; s < 2; s++) {
            uint8_t o = (uint8_t)(s ? x2_levels_a[k] : (x2_levels_a[k] & 0x7f));
            c->ra[x2call_pad(o, pad_of(c))] = o;
        }
    v90_scr_init(&c->dscr);
    pick_up(c, cfg->up_mask);
    c->g16 = (int32_t)v34_gain_q14(V34_ENERGY_16PT_Q8);
}

x2c_state x2call_state(const x2call_t *c) { return c->state; }

static void go_sig(x2call_t *c, int sig) { c->sig = sig; c->sn = 0; }

/* ---- upstream ---- */

static void tx_symbol(x2call_t *c, int32_t *re, int32_t *im)
{
    v34_pt_t pt;
    int32_t g = 16384;
    int k;
    *re = *im = 0;
    if (c->reneg_due && c->sig == SIG_DATA && (!c->cfg.reneg_whole || c->vtx.pos >= 8)) {
        /* the rate change: S in data mode, then as Phase 4 with a short TRN */
        c->reneg_due = 0; c->renegs++; c->reneg_mp = 0; c->rr_acked = 0;
        c->rr_base = c->seen.rr_nak + c->seen.rr_ack;
        pick_up(c, c->cfg.reneg_mask);
        go_sig(c, SIG_S); c->state = X2C_RENEG;
    }
    switch (c->sig) {
    case SIG_SIL:
        return;
    case SIG_S:
        v34_s(c->sn, V34_S_LEN, re, im);
        if (++c->sn == V34_S_LEN) go_sig(c, SIG_SBAR);
        return;
    case SIG_SBAR:
        v34_sbar(c->sn, re, im);
        if (++c->sn == V34_SBAR_LEN) {
            v34_tmod_init(&c->tmod, V34_GPA, 4);
            go_sig(c, c->state == X2C_P4 || c->state == X2C_RENEG ? SIG_TRN4 : SIG_PP);
        }
        return;
    case SIG_PP:
        v34_pp(c->sn, re, im);
        if (++c->sn == V34_PP_LEN) { v34_tmod_init(&c->tmod, V34_GPA, 4); go_sig(c, SIG_TRN); }
        return;
    case SIG_TRN:
        pt = v34_tmod_trn(&c->tmod);
        if (++c->sn == P3_TRN) go_sig(c, SIG_J);
        break;
    case SIG_J:
        k = (int)(c->sn & 7) * 2;
        pt = v34_tmod_put(&c->tmod, (unsigned)(v34_j16[k] | (v34_j16[k + 1] << 1)));
        if ((++c->sn & 7) == 0 && c->k >= J_STOP) { go_sig(c, SIG_SIL); c->state = X2C_QUIET; }
        break;
    case SIG_TRN4:
        if (c->state == X2C_RENEG) {
            /* the rate change's TRN, MP and E: 4 points only (x2-facts.md
             * "Rate change": the Courier's) */
            pt = v34_tmod_trn(&c->tmod);
            if (++c->sn == P4_TRN4) {
                build_mp(c, 0); c->mpn = 0; c->reneg_mp = 1; go_sig(c, SIG_MP);
            }
            break;
        }
        if (c->sn == P4_TRN4) v34_tmod_set_points(&c->tmod, 16);
        pt = v34_tmod_trn(&c->tmod);
        if (c->sn >= P4_TRN4) g = c->g16;
        if (++c->sn == P4_TRN) { build_mp(c, 0); c->mpn = 0; go_sig(c, SIG_MP); c->state = X2C_MP; }
        break;
    case SIG_MP: {
        /* 16 points in the start-up, 4 in a rate change (the Courier's) */
        unsigned b = 0;
        int bps = c->renegs ? 2 : 4;
        for (k = 0; k < bps; k++) b |= (unsigned)c->mpbits[c->mpn + k] << k;
        pt = v34_tmod_put(&c->tmod, b);
        if (!c->renegs) g = c->g16;
        c->mpn += bps;
        if (c->mpn >= MP_BITS) {
            c->mpn = 0; c->mp_count++;
            if (c->cfg.silent == X2C_SIL_MP && c->mp_count >= 4) c->quiet = 1;
            if (c->mp_ack) { go_sig(c, SIG_E); c->state = X2C_E; c->e_sent = 1; }
            else if (c->renegs ? c->seen.rr_nak + c->seen.rr_ack > c->rr_base : c->seen.rec_good > 0)
                build_mp(c, 1);                                 /* one whole MP', then E */
        }
        break;
    }
    case SIG_E:
        pt = v34_tmod_put(&c->tmod, c->renegs ? 3 : 15);
        if (!c->renegs) g = c->g16;
        if (++c->sn * (c->renegs ? 2 : 4) >= 20) { v34_tx_init(&c->vtx, &c->tpar, c->cfg.getbit, c->cfg.user); go_sig(c, SIG_DATA); c->up_at = c->now; }
        break;
    default:    /* SIG_DATA: B1, then data */
        if (!v34_tx_in_b1(&c->vtx)) c->seen.data_up = 1;
        v34_tx_symbol(&c->vtx, &pt, NULL); g = c->tx_dg;
        break;
    }
    *re = (pt.re * g) >> 9; *im = (pt.im * g) >> 9;
}

static int16_t tx_sample(x2call_t *c)
{
    if (v34mod_due(&c->mod)) {
        int32_t re, im;
        tx_symbol(c, &re, &im);
        v34mod_put(&c->mod, re, im);
    }
    return v34mod_tick(&c->mod);
}

/* ---- downstream: the server's script ---- */

static int dbit(x2call_t *c, uint8_t o) { return v90_descramble_gpc(&c->dscr, (o & 0x80) ? 0 : 1); }

/* our record, F33C (733C) 7FFF 0000 (word 1: all 15 indices, x2-facts.md
 * "Records"): in the start-up, F33C alone; in a rate change 733C, then
 * F33C */
static void rec_bit(x2call_t *c, int bit)
{
    if (c->rn > 0) {
        c->rb[c->rn++] = (uint8_t)bit;
        if (c->rn == X2REC_BITS(3)) {
            uint16_t w[3];
            int ok = x2rec_parse(c->rb, c->rn, w, 3) == 3 && (w[0] & 0x7fff) == 0x733C && w[1] == 0x7FFF && w[2] == 0;
            if (!c->rr) {
                if (ok && (w[0] >> 15)) c->seen.rec_good++; else c->seen.rec_bad++;
            } else if (!ok) {
                c->seen.rr_bad++;
            } else if (w[0] >> 15) {
                c->seen.rr_ack++; c->rr_acked = 1;
            } else {
                c->seen.rr_nak++;
                if (c->rr_acked) c->seen.rr_order++;
            }
            c->rn = 0;
        }
    }
    if (bit) { c->rones++; return; }
    if (c->rones >= 17) { memset(c->rb, 1, 17); c->rb[17] = 0; c->rn = 18; }
    c->rones = 0;
}

/* the server's marker frame of a rate change, as received: +++--- (last
 * 0) or ---+++ (last 1), each position at its largest level in the data
 * levels in use */
static int marker_frame(const x2call_t *c, int last)
{
    const uint8_t *rv, *sn;
    int i;
    for (i = 0; i < X2MAP_N; i++) {
        uint8_t hi;
        if (!x2map_list(&c->map, i, &rv, &sn)) return 0;
        hi = rv[0];
        if (x2call_level(&c->map, i, c->fr[i]) != (((i < 3) != last) ? hi : (uint8_t)(hi & 0x7f))) return 0;
    }
    return 1;
}

/* the frame's octets as the levels the client takes them for */
static void levels_in(const x2call_t *c, uint8_t t[X2MAP_N])
{
    int i;
    for (i = 0; i < X2MAP_N; i++) t[i] = x2call_level(&c->map, i, c->fr[i]);
}

static void frame_in(x2call_t *c)
{
    uint8_t bits[64], t[X2MAP_N];
    int n, i, ones = 0;
    if (c->in_data && marker_frame(c, 0)) {
        if (!c->reneg_mp) {
            c->seen.mark_early++;           /* (and taken as data, below) */
        } else {
            c->in_data = 0; c->rr = 1; c->seen.mark_frames++;
            return;
        }
    }
    if (c->rr == 1) {
        if (marker_frame(c, 0)) {
            c->seen.mark_frames++;
        } else if (marker_frame(c, 1)) {
            /* the records from here: GPC from zero, the sign positive, levels B */
            c->seen.mark_last = 1; c->rr = 2;
            v90_scr_init(&c->dscr); x2map_init(&c->map, 1, pad_of(c), 0);
            c->rn = c->rones = 0;
        } else {
            c->seen.mark_bad++;
        }
        return;
    }
    if (c->rr == 2) {
        levels_in(c, t);
        n = x2map_unframe(&c->map, t, bits);
        if (n < 0) { c->seen.rr_bad++; return; }
        for (i = 0; i < n; i++) { bits[i] = (uint8_t)v90_descramble_gpc(&c->dscr, bits[i]); ones += bits[i]; }
        if (ones == n && c->rn == 0 && c->seen.rr_nak + c->seen.rr_ack > c->rr_base) {
            /* the frame of ones after the records; then the rate asked for */
            c->seen.rr_ones++; c->rr = 0;
            c->in_start = 1; c->start_n = 0; c->start_len = START_FRAMES;
            c->map.index = c->renegs == 2 ? c->cfg.reneg_index2 : c->cfg.reneg_index;
            return;
        }
        for (i = 0; i < n; i++) rec_bit(c, bits[i]);
        return;
    }
    if (!c->in_start && !c->in_data) {
        /* levels A, sent whatever the pad: back to the octets sent */
        int before = c->seen.rec_good + c->seen.rec_bad == 0 && c->rn == 0;
        for (i = 0; i < X2MAP_N; i++) {
            if (c->ra[c->fr[i]] < 0) { if (before) c->seen.sync_bad++; else c->seen.rec_bad++; return; }
            t[i] = (uint8_t)c->ra[c->fr[i]];
        }
        n = x2map_unframe(&c->map, t, bits);
        for (i = 0; i < n; i++) { bits[i] = (uint8_t)v90_descramble_gpc(&c->dscr, bits[i]); ones += bits[i]; }
        if (c->e_sent && ones == n) {
            /* the start: its first frame of ones in levels A, then the
             * data levels of the index we asked for, behind our pad */
            c->in_start = 1; c->start_n = 0; c->start_len = START_FRAMES - 1;
            c->map.levels_a = 0; c->map.index = c->cfg.max_index; c->map.pad_mode = pad_of(c);
            return;
        }
        for (i = 0; i < n; i++) rec_bit(c, bits[i]);
        if (before) { if (ones == n) c->seen.sync_frames++; else if (c->rn == 0) c->seen.sync_bad++; }
        return;
    }
    levels_in(c, t);
    n = x2map_unframe(&c->map, t, bits);
    if (n < 0) {
        if (!c->in_start) c->seen.data_lost++;
        else if (c->renegs) c->seen.rstart_bad++;
        else c->seen.start_bad++;
        return;
    }
    for (i = 0; i < n; i++) { bits[i] = (uint8_t)v90_descramble_gpc(&c->dscr, bits[i]); ones += bits[i]; }
    if (c->in_start) {
        if (c->renegs) { if (ones == n) c->seen.rstart_ones++; else c->seen.rstart_bad++; }
        else if (ones == n) c->seen.start_ones++; else c->seen.start_bad++;
        if (++c->start_n == c->start_len) { c->in_start = 0; c->in_data = 1; }
        return;
    }
    c->seen.data_frames++;
    if (c->cfg.putbit) for (i = 0; i < n; i++) c->cfg.putbit(c->cfg.user, bits[i]);
}

static void octet_in(x2call_t *c, uint8_t o, int32_t lin)
{
    int32_t k;
    int pad = pad_of(c);
    if (c->k < 0) {
        if (c->j_from < 0 || lin == 0) return;
        c->k = 0;
    }
    k = c->k++;
    if (k < K_TRN) {
        if (o != x2call_pad(pre_octet(k), pad)) c->seen.pre_bad++;
    } else if (k < K_JW) {
        if ((o & 0x7f) != (x2call_pad(0xb1, pad) & 0x7f) || !dbit(c, o)) c->seen.trn_bad++;
    } else if (k < K_ONES) {
        int j = k - K_JW;
        if (j == 0 && c->state == X2C_QUIET) {
            /* the J words: our Phase 4 */
            if (c->cfg.silent == X2C_SIL_J) c->quiet = 1;
            go_sig(c, SIG_S); c->state = X2C_P4;
        }
        if ((o & 0x7f) != (x2call_pad(0xb1, pad) & 0x7f)) c->seen.trn_bad++;
        c->jw |= (unsigned)dbit(c, o) << (j % 12);
        if (j % 12 == 11) {
            if (j / 12 < 99) { if (c->jw == 0x09B0) c->seen.jw_good++; }
            else c->seen.jw_last = c->jw == 0x099F;
            c->jw = 0;
        }
    } else if (k < K_ALIGN) {
        if ((o & 0x7f) != (x2call_pad(0xc1, pad) & 0x7f) || !dbit(c, o)) c->seen.ones_bad++;
    } else if (k < K_SYNC) {
        int j = k - K_ALIGN;
        if (o != x2call_pad(align_tab[(j % 6 + j / 6) % 6], pad)) c->seen.align_bad++;
    } else {
        if (k == K_SYNC) { x2map_init(&c->map, 1, 0, 1); c->fpos = 0; }
        c->fr[c->fpos++] = o;
        if (c->fpos == X2MAP_N) { c->fpos = 0; frame_in(c); }
    }
}

int16_t x2call_tick(x2call_t *c, uint8_t octet)
{
    uint8_t o = x2call_pad(octet, pad_of(c));
    int32_t lin = ulaw_to_linear(o);
    int16_t out = 0;
    c->now++; c->t++;
    switch (c->state) {
    case X2C_V8:
        if (v8call_tick(&c->v8, (int16_t)lin, &out) == V8C_DONE) {
            p2c_cfg pc;
            int k;
            memset(&pc, 0, sizeof pc);
            pc.level = c->cfg.level;
            pc.info0c.lo3000 = pc.info0c.hi3000 = pc.info0c.lo3200 = pc.info0c.hi3200 = 1;
            pc.info0c.pwr_reduce = 1; pc.info0c.c1664 = 1;
            pc.hi_carrier_mask = 1 << SYM_IDX;
            for (k = 0; k < 6; k++) pc.max_rate[k] = -1;
            p2c_init(&c->p2, &pc);
            c->state = X2C_P2;
        }
        break;
    case X2C_P2:
        out = p2c_tick(&c->p2, (int16_t)lin);
        if (c->p2.state == P2C_FAILED) { c->state = X2C_ENDED; break; }
        if (c->mark_at < 0 && c->p2.state == P2C_PROBE_RX) {
            /* the server's reversal, 10 ms of Tone A, then its L1 */
            if (c->p2.info0a.max_sr_diff < 4 && !c->p2.info0a.cme) { c->seen.not_x2 = 1; c->state = X2C_ENDED; break; }
            c->mark_at = c->now - (c->p2.now - c->p2.t_rev2) + 80 + MS(c->cfg.marker_ms);
        }
        if (c->mark_at >= 0 && c->now >= c->mark_at) {
            dpsktx_send(&c->mk, c->mkbits, c->nmk);
            c->mk_off = -1;
            c->state = X2C_MARKER;
            out = dpsktx_tick(&c->mk);
        }
        break;
    case X2C_MARKER:
        out = dpsktx_tick(&c->mk);
        if (c->mk_off < 0 && !dpsktx_busy(&c->mk)) c->mk_off = c->now + 60;   /* the last bit out */
        if (c->now == c->mk_off) dpsktx_off(&c->mk);
        if (c->mk_off > 0 && c->now >= c->mk_off + MS(70)) {
            if (c->cfg.silent == X2C_SIL_MARKER) c->quiet = 1;
            (void)v34mod_init(&c->mod, 3200, 3, 5, 0, c->cfg.level, 0);    /* 1920 Hz */
            go_sig(c, SIG_S);
            c->state = X2C_P3;
        }
        break;
    case X2C_ENDED:
        break;
    default:
        if (c->sig == SIG_J && c->j_from < 0) c->j_from = c->now;
        octet_in(c, o, lin);
        if (c->state == X2C_E && c->in_data && c->sig == SIG_DATA) {
            c->state = X2C_DATA;
            c->data_at = c->now;        /* (and again after each rate change) */
        }
        if (c->cfg.reneg_ms && c->renegs < (c->cfg.reneg_index2 ? 2 : 1) && !c->reneg_due && c->state == X2C_DATA
            && !c->cfg.reneg_up && c->now - c->data_at >= MS(c->cfg.reneg_ms))
            c->reneg_due = 1;
        if (c->cfg.reneg_ms && c->cfg.reneg_up && !c->renegs && !c->reneg_due && c->up_at >= 0
            && c->now - c->up_at >= MS(c->cfg.reneg_ms))
            c->reneg_due = 1;
        if (c->cfg.silent == X2C_SIL_DATA && c->state == X2C_DATA && c->seen.data_frames >= 1334) c->quiet = 1;
        out = tx_sample(c);
        break;
    }
    return c->quiet ? 0 : out;
}
