/* The digital modem's Phase 4 sequences, word by word. See v92aseq.h. */
#include <string.h>
#include "v92aseq.h"

#define NONE_YET 0x7fffffff     /* crc_at while the layout is not known */

void v92aseq_init(v92aseq_t *s)
{
    memset(s, 0, sizeof *s);
    s->after = -1;
    s->crc_at = NONE_YET;
}

void v92aseq_phase4(v92aseq_t *s)
{
    v92aseq_init(s);            /* nothing stored */
}

void v92aseq_restart(v92aseq_t *s)
{
    uint8_t has = s->has;
    v92aseq_init(s);
    s->has = has;
}

int v92aseq_cpd_complete(const v92aseq_t *s)
{
    return s->has == (V92AS_HAS_MOD | V92AS_HAS_FILT | V92AS_HAS_CONST);
}

static void hunt(v92aseq_t *s)
{
    s->in = 0; s->sh.ones = 0;
}

static void bad(v92aseq_t *s)
{
    s->ev |= V92AS_BAD;
    hunt(s);
}

/* Table 30, word k of a CPd (word 0, the header, is taken in begin()) */
static void cpd_word(v92aseq_t *s, int k, unsigned v)
{
    v92_cpd_t *c = &s->cpd;
    int i_mod = 2, i_len = i_mod + (c->has_mod ? 6 : 0), i_coef = i_len + (c->has_filt ? 4 : 0), i;
    if (k == 1) { c->filt.gain4 = (uint16_t)v; return; }
    if (k < i_len) {
        i = 2 * (k - i_mod);
        c->map.M[i] = (uint8_t)(v & 0xFF); c->map.M[i + 1] = (uint8_t)(v >> 8);
        return;
    }
    if (k < i_coef) {
        uint16_t l = (uint16_t)(v & 0x1FF);
        switch (k - i_len) {
        case 0: c->filt.lz1 = l; break;
        case 1: c->filt.lp1 = l; break;
        case 2: c->filt.lz2 = l; break;
        default:
            c->filt.lp2 = l;
            /* the lengths decide the layout: checked before the CRC */
            if (c->filt.lz1 > V92_MAX_LSEC || c->filt.lp1 > V92_MAX_LSEC || c->filt.lz2 > V92_MAX_LSEC
                || c->filt.lp2 > V92_MAX_LSEC || c->filt.lz1 + c->filt.lp1 + c->filt.lz2 + c->filt.lp2 > V92_MAX_LTOT) { bad(s); return; }
            s->ncoef = c->filt.lz1 + c->filt.lp1 + c->filt.lz2 + c->filt.lp2;
            s->i_lc = i_coef + s->ncoef + 2;
            if (!c->has_const) s->crc_at = i_coef + s->ncoef;
            break;
        }
        return;
    }
    if (k < i_coef + s->ncoef) { c->filt.coef[k - i_coef] = (int16_t)(uint16_t)v; return; }
    if (k < s->i_lc) {
        if (k == s->i_lc - 2) for (i = 0; i < 4; i++) c->map.cidx[i] = (uint8_t)((v >> (4 * i)) & 0xF);
        else { c->map.cidx[4] = (uint8_t)(v & 0xF); c->map.cidx[5] = (uint8_t)((v >> 4) & 0xF); }
        return;
    }
    if (k < s->i_lc + 3) {
        i = 2 * (k - s->i_lc);
        c->map.lc[i] = (uint8_t)(v & 0xFF); c->map.lc[i + 1] = (uint8_t)(v >> 8);
        if (c->map.lc[i] > V92_MAX_LC || c->map.lc[i + 1] > V92_MAX_LC) { bad(s); return; }
        if (k == s->i_lc + 2) {
            s->npts = 0;
            for (i = 0; i < V92_NCONST; i++) s->npts += c->map.lc[i];
            s->i_pts = s->i_lc + 3;
            s->crc_at = s->i_pts + s->npts;
        }
        return;
    }
    /* the points, set after set */
    {
        int j = k - s->i_pts, set = 0;
        while (set < V92_NCONST - 1 && j >= c->map.lc[set]) { j -= c->map.lc[set]; set++; }
        c->map.pt[set][j] = (uint16_t)v;
    }
}

/* the header word: what kind, and how long */
static void begin(v92aseq_t *s, unsigned v)
{
    v92_cpd_t *c = &s->cpd;
    s->w0 = (uint16_t)v;
    s->kind = (uint8_t)((v & 1) ? V92_DN_SUVD : V92_DN_CPD);
    if (s->kind == V92_DN_SUVD) { s->crc_at = 1; return; }
    memset(c, 0, sizeof *c);
    c->has_mod = (uint8_t)((v >> 1) & 1);
    c->has_filt = (uint8_t)((v >> 2) & 1);
    c->has_const = (uint8_t)((v >> 3) & 1);
    c->drn = (uint8_t)((v >> 4) & 0x1F);
    c->map.trellis = (uint8_t)((v >> 9) & 3);
    c->e2u_extend = (uint8_t)((v >> 11) & 1);
    c->ack = (uint8_t)((v >> 15) & 1);
    s->ncoef = 0; s->npts = 0;
    {
        int i_coef = 2 + (c->has_mod ? 6 : 0) + (c->has_filt ? 4 : 0);
        s->i_lc = i_coef + 2;           /* (moved on once the filter lengths are in) */
        s->crc_at = c->has_filt || c->has_const ? NONE_YET : i_coef;
    }
}

/* A CPd whose CRC checked out, into a copy *o of the stored one: what is
 * always carried and each part it carries replace what was there; a part
 * it leaves out is kept, or zero (as v92_cpd_parse() leaves it) when none
 * is stored. The CPd being received (s->cpd) has its absent parts zero
 * (begin()). Returns the parts *o then holds (V92AS_HAS_*); s->has is the
 * caller's to set. */
static uint8_t merge(const v92aseq_t *s, v92_cpd_t *o)
{
    uint8_t has;
    const v92_cpd_t *c = &s->cpd;
    o->drn = c->drn;
    o->e2u_extend = c->e2u_extend;
    o->ack = c->ack;
    o->map.K = c->map.K;
    o->map.trellis = c->map.trellis;
    o->filt.gain4 = c->filt.gain4;
    if (c->has_mod || !(s->has & V92AS_HAS_MOD)) memcpy(o->map.M, c->map.M, sizeof o->map.M);
    if (c->has_filt || !(s->has & V92AS_HAS_FILT)) {
        o->filt.lz1 = c->filt.lz1; o->filt.lp1 = c->filt.lp1;
        o->filt.lz2 = c->filt.lz2; o->filt.lp2 = c->filt.lp2;
        memcpy(o->filt.coef, c->filt.coef, sizeof o->filt.coef);
    }
    if (c->has_const || !(s->has & V92AS_HAS_CONST)) {
        memcpy(o->map.cidx, c->map.cidx, sizeof o->map.cidx);
        memcpy(o->map.lc, c->map.lc, sizeof o->map.lc);
        memcpy(o->map.pt, c->map.pt, sizeof o->map.pt);
    }
    has = (uint8_t)(s->has | (c->has_mod ? V92AS_HAS_MOD : 0) | (c->has_filt ? V92AS_HAS_FILT : 0)
                    | (c->has_const ? V92AS_HAS_CONST : 0));
    o->has_mod = (uint8_t)((has & V92AS_HAS_MOD) != 0);
    o->has_filt = (uint8_t)((has & V92AS_HAS_FILT) != 0);
    o->has_const = (uint8_t)((has & V92AS_HAS_CONST) != 0);
    return has;
}

/* 1 if the merged CPd *o (holding the parts has) may be stored: one that
 * names an upstream rate must have a usable map once it holds the moduli
 * and the constellations (v92_map_check: Mi >= 1, the indices name sets of
 * points, trellis 0..2, ...), and a usable filter once it holds one
 * (v92_filt_check). A CRC protects against the line, not against the
 * digital modem: an encoder on a map that fails these divides by zero or
 * reads past the map. A CPd that names no upstream rate (drn 0, the
 * cleardown, or 20..31, which Table 30 does not define) is stored
 * unchecked, as before: nothing is encoded from it, since start_b1u
 * refuses a drn outside 1..19. */
static int storable(const v92_cpd_t *o, uint8_t has)
{
    if (o->drn < 1 || o->drn > V92_UP_NUM_RATES) return 1;
    if ((has & V92AS_HAS_MOD) && (has & V92AS_HAS_CONST) && v92_map_check(&o->map) != 0) return 0;
    if ((has & V92AS_HAS_FILT) && v92_filt_check(&o->filt) != 0) return 0;
    return 1;
}

/* the CRC word is in: the sequence, if it checks out */
static void done(v92aseq_t *s, unsigned v, v92_suvd_t *suvd, v92_cpd_t *cpd)
{
    if ((uint16_t)v != s->crc_want) { bad(s); return; }
    if (s->kind == V92_DN_SUVD) {
        suvd->silence = (uint8_t)((s->w0 >> 14) & 1);
        suvd->ack = (uint8_t)((s->w0 >> 15) & 1);
        s->ev |= V92AS_SUVD;
    } else {
        v92_cpd_t *c = &s->cpd;
        uint8_t has;
        c->map.K = (uint8_t)(c->drn >= 1 && c->drn <= V92_UP_NUM_RATES ? V92_UP_K(c->drn) : 0);
        s->out = *cpd;
        has = merge(s, &s->out);
        if (!storable(&s->out, has)) { bad(s); return; }    /* refused: as a CPd that did not check out */
        s->has = has;
        *cpd = s->out;
        s->ev |= V92AS_CPD;
    }
    hunt(s);
    s->fill = 1;                /* the fill bit and zeros to the end of the frame */
}

static void bit_in(v92aseq_t *s, int bit, v92_suvd_t *suvd, v92_cpd_t *cpd)
{
    if (!s->in) {
        if (seqhunt_bit(&s->sh, bit)) {
            /* the frame sync and the start bit of the header word */
            s->in = 1; s->nb = 1; s->cur = 0; s->nw = 0;
            s->crc = 0xFFFF; s->crc_at = NONE_YET;
        }
        return;
    }
    if (s->nb == 0) {               /* a start bit */
        if (bit) { bad(s); s->sh.ones = 1; return; }
        if (s->nw == s->crc_at) s->crc_want = s->crc;
        s->nb = 1; s->cur = 0;
        return;
    }
    s->cur |= (uint32_t)(bit & 1) << (s->nb - 1);
    if (s->nw != s->crc_at) s->crc = v90_crc_step(s->crc, bit & 1);
    if (++s->nb <= 16) return;
    s->nb = 0;
    if (s->nw == s->crc_at) { done(s, s->cur, suvd, cpd); return; }
    if (s->nw == 0) begin(s, s->cur);
    else cpd_word(s, s->nw, s->cur);
    if (++s->nw > V92AS_MAX_WORDS && s->in) bad(s);
}

int v92aseq_frame(v92aseq_t *s, const uint8_t *bits, int n, v92_suvd_t *suvd, v92_cpd_t *cpd)
{
    int i, zeros = 1;
    s->ev = 0;
    if (s->skip) {                  /* the CRC ended a frame: this one is fill */
        s->skip = 0; s->after = 0;
        return 0;
    }
    for (i = 0; i < n; i++) if (bits[i] & 1) { zeros = 0; break; }
    if (s->after == 0) {
        /* Ed: a frame of zeros straight after a sequence */
        s->after = -1;
        if (!s->in && zeros) { s->ev |= V92AS_ED; return s->ev; }
    }
    for (i = 0; i < n; i++) {
        bit_in(s, bits[i] & 1, suvd, cpd);
        if (s->fill) {              /* a sequence ended: its fill bit is next */
            s->fill = 0;
            if (i == n - 1) s->skip = 1; else s->after = 0;
            break;
        }
    }
    return s->ev;
}
