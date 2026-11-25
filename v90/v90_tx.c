/* V.90 digital modem transmitter. See v90_tx.h for the interface, the signal
 * table and the spec readings.
 *
 * Structure. Every signal is a whole number of 6-symbol data frames, so the
 * transmitter works a data frame at a time:
 *
 *   front (build_frame)  decides which signal the next data frame belongs to,
 *                        and builds it as six Ucodes and six sign bits. All
 *                        state that decides future frames lives in
 *                        v90tx_front_t.
 *   queue                the frame being sent plus the frames built ahead for
 *                        the shaper's lookahead (none unless Sr > 0, ld > 0).
 *                        Each entry keeps a copy of the front state from
 *                        before it was built, so a selection can drop
 *                        built-ahead frames and rebuild them.
 *   back (emit_head)     runs the spectral shaper over the frame at the head
 *                        of the queue, looking ahead into the queue, and
 *                        turns it into six octets.
 */
#include <string.h>
#include "v90_tx.h"
#include "v90_pcm.h"

/* frame flags */
#define FL_FIRST   0x01   /* first data frame of a signal */
#define FL_UEND    0x02   /* last data frame of a unit */
#define FL_SHAPED  0x04   /* t[] is the initial assignment; the shaper decides */
#define FL_RESET   0x08   /* zero the shaper before this frame */
#define FL_SWITCH  0x10   /* a selection would have taken effect at this frame */
#define FL_USER    0x20   /* carries bits taken from getbit: cannot be rebuilt */
#define FL_DMAP    0x40   /* encoded with the data map (else the training map) */

/* parameter presence */
#define HAVE_JD    0x01
#define HAVE_MP    0x02
#define HAVE_DIL   0x04
#define HAVE_TRAIN 0x08
#define HAVE_DATA  0x10

/* ---- signal properties ----------------------------------------------------- */

/* number of units of a fixed-length signal, 0 if it runs until told */
static unsigned fixed_units(int sig)
{
    switch (sig) {
    case V90TX_SD:       return 64;
    case V90TX_SD_BAR:   return 8;
    case V90TX_JD_PRIME: return 1;
    case V90TX_RI_BAR:
    case V90TX_RT_BAR:
    case V90TX_RD_BAR:   return 4;
    case V90TX_ED:       return 2;
    case V90TX_B1D:      return 48;
    case V90TX_JP_PRIME: return 1;
    case V90TX_RF_BAR:   return 2;
    default:             return 0;
    }
}

/* the sequence a V.92 signal carries, -1 if none */
static int xseq_of(int sig)
{
    switch (sig) {
    case V90TX_JP:       return V90TX_XSEQ_JP;
    case V90TX_SUVD:     return V90TX_XSEQ_SUVD;
    case V90TX_SUVD_ACK: return V90TX_XSEQ_SUVD_ACK;
    case V90TX_CPD:      return V90TX_XSEQ_CPD;
    case V90TX_CPD_ACK:  return V90TX_XSEQ_CPD_ACK;
    default:             return -1;
    }
}

static int available(const v90tx_t *t, int sig)
{
    switch (sig) {
    case V90TX_JP_PRIME: case V90TX_SCR:
        return 1;
    case V90TX_JP:
        return t->xseq[V90TX_XSEQ_JP] != 0;
    case V90TX_SUVD: case V90TX_SUVD_ACK: case V90TX_CPD: case V90TX_CPD_ACK:
        return (t->have & HAVE_TRAIN) && t->xseq[xseq_of(sig)] != 0;
    case V90TX_RF: case V90TX_RF_BAR:
        return (t->have & HAVE_DATA) != 0;
    case V90TX_SILENCE: case V90TX_SD: case V90TX_SD_BAR: case V90TX_TRN1D:
    case V90TX_JD_PRIME: case V90TX_RI: case V90TX_RI_BAR:
        return 1;
    case V90TX_JD:
        return (t->have & HAVE_JD) != 0;
    case V90TX_DIL:
        return (t->have & HAVE_DIL) && t->dil.n > 0;
    case V90TX_RT: case V90TX_RT_BAR: case V90TX_TRN2D: case V90TX_ED:
        return (t->have & HAVE_TRAIN) != 0;
    case V90TX_MP: case V90TX_MP_ACK:
        return (t->have & HAVE_TRAIN) && (t->have & HAVE_MP);
    case V90TX_RD: case V90TX_RD_BAR: case V90TX_B1D:
        return (t->have & HAVE_DATA) != 0;
    case V90TX_DATA:
        return (t->have & HAVE_DATA) && t->getbit != 0;
    default:
        return 0;
    }
}

/* what follows a fixed-length signal when nothing was selected */
static int successor(const v90tx_t *t, int sig)
{
    int next;
    switch (sig) {
    case V90TX_SD:       next = V90TX_SD_BAR; break;
    case V90TX_SD_BAR:   next = V90TX_TRN1D; break;
    case V90TX_JD_PRIME: next = ((t->have & HAVE_DIL) && t->dil.n > 0) ? V90TX_DIL : V90TX_RI; break;
    case V90TX_JP_PRIME: next = ((t->have & HAVE_DIL) && t->dil.n > 0) ? V90TX_DIL : V90TX_SCR; break;   /* 9.5.1.1.11/V.92 */
    case V90TX_RI_BAR:
    case V90TX_RD_BAR:   next = V90TX_TRN2D; break;
    case V90TX_RT_BAR:   next = V90TX_MP; break;
    case V90TX_ED:       next = V90TX_B1D; break;
    case V90TX_B1D:      next = V90TX_DATA; break;
    default:             next = V90TX_SILENCE; break;
    }
    return available(t, next) ? next : V90TX_SILENCE;
}

static int uses_data_map(int sig)
{
    return sig == V90TX_B1D || sig == V90TX_DATA || sig == V90TX_RD || sig == V90TX_RD_BAR
        || sig == V90TX_RF || sig == V90TX_RF_BAR;
}

/* ---- front ----------------------------------------------------------------- */

static void seq_load(v90tx_t *t, const uint8_t *bits, int n)
{
    int i;
    memset(t->seq, 0, sizeof t->seq);
    for (i = 0; i < n; i++)
        if (bits[i]) t->seq[i >> 3] |= (uint8_t)(1u << (i & 7));
    t->seqlen = (uint16_t)n;
    t->seqp = t->seq;
}

/* the unit is one of the caller's packed sequences */
static void seq_ext(v90tx_t *t, int which)
{
    t->seqp = t->xseq[which];
    t->seqlen = t->xlen[which];
}

/* bit i of the Jd / MP unit in progress; zero fill beyond its end */
static int seq_bit(const v90tx_t *t, int i)
{
    if (i >= t->seqlen) return 0;
    return (t->seqp[i >> 3] >> (i & 7)) & 1;
}

static int pat_bit(const uint8_t *p, int m)
{
    return (p[m >> 3] >> (m & 7)) & 1;
}

static void start_signal(v90tx_t *t, int sig, uint8_t *flags)
{
    v90tx_front_t *f = &t->front;
    f->sig = (int8_t)sig;
    f->fr = 0;
    f->seg = 0;
    f->units = 0;
    *flags |= FL_FIRST;
    switch (sig) {
    case V90TX_TRN1D:                                    /* 8.4.5 */
        v90_scr_init(&f->scr);
        break;
    case V90TX_TRN2D:                                    /* 8.6.5 */
    case V90TX_B1D:                                      /* 8.6.1 */
        v90_scr_init(&f->scr);
        v90_diff_init(&f->diff);
        *flags |= FL_RESET;
        break;
    default:
        break;
    }
}

/* set up the unit that starts with the next data frame */
static void begin_unit(v90tx_t *t)
{
    v90tx_front_t *f = &t->front;
    uint8_t bits[V90_MP_MAX_BITS];
    int n, D;
    f->nfr = 1;
    switch (f->sig) {
    case V90TX_JD:
        n = v90_jd_build(&t->jd, bits);
        seq_load(t, bits, n);
        f->nfr = V90_JD_BITS / 6;
        break;
    case V90TX_JD_PRIME:
    case V90TX_JP_PRIME:
    case V90TX_RF: case V90TX_RF_BAR:
        f->nfr = 2;                                      /* 12 zeros; the 12 symbols of Rf */
        break;
    case V90TX_JP:
        seq_ext(t, V90TX_XSEQ_JP);
        f->nfr = (uint16_t)((t->seqlen + 5) / 6);
        break;
    case V90TX_SUVD: case V90TX_SUVD_ACK: case V90TX_CPD: case V90TX_CPD_ACK:
        seq_ext(t, xseq_of(f->sig));
        D = V90_MAP_D(&t->map[V90TX_MAP_TRAIN]);
        f->nfr = (uint16_t)((t->seqlen + D - 1) / D);    /* zero fill to whole data frames */
        break;
    case V90TX_DIL:
        /* Lc = (Hc + 1) * 6 symbols, c the Uchord of the training Ucode */
        f->nfr = (uint16_t)(t->dil.h[t->dil.train[f->seg] >> 4] + 1);
        break;
    case V90TX_MP:
    case V90TX_MP_ACK: {
        v90_mp_t mp = t->mp;
        mp.ack = (f->sig == V90TX_MP_ACK);
        n = v90_mp_build(&mp, bits);
        seq_load(t, bits, n);
        D = V90_MAP_D(&t->map[V90TX_MAP_TRAIN]);
        f->nfr = (uint16_t)((n + D - 1) / D);            /* zero fill to whole data frames */
        break;
    }
    default:
        break;
    }
}

static void build_frame(v90tx_t *t, v90tx_frame_t *q)
{
    v90tx_front_t *f = &t->front;
    uint8_t flags = 0;
    int i;

    q->snap = *f;
    if (f->fr == 0) {
        unsigned fixed = fixed_units(f->sig);
        if (fixed == 0 || f->units >= fixed) {
            int next = V90TX_NONE;
            flags |= FL_SWITCH;
            if (t->pending != V90TX_NONE) { next = t->pending; t->pending = V90TX_NONE; }
            else if (fixed) next = successor(t, f->sig);
            if (next != V90TX_NONE) {
                if (!available(t, next)) next = V90TX_SILENCE;
                start_signal(t, next, &flags);
            }
        }
        begin_unit(t);
    }

    switch (f->sig) {
    case V90TX_SD:
    case V90TX_SD_BAR: {
        /* {+W, +0, +W, -W, -0, -W}, W = Ucode 16 + UINFO; inverted for Sd-bar */
        uint8_t w = (uint8_t)(16 + t->uinfo);
        for (i = 0; i < 6; i++) {
            q->u[i] = (i % 3 == 1) ? 0 : w;
            q->t[i] = (uint8_t)((i < 3) ^ (f->sig == V90TX_SD_BAR));
        }
        break;
    }
    case V90TX_TRN1D:
        for (i = 0; i < 6; i++) {
            q->u[i] = t->uinfo;
            f->dsign = (uint8_t)v90_scramble_gpc(&f->scr, 1);
            q->t[i] = f->dsign;
        }
        break;
    case V90TX_SCR:                                      /* 8.6.6/V.92: the scrambler runs on */
        for (i = 0; i < 6; i++) {
            q->u[i] = t->uinfo;
            f->dsign = (uint8_t)v90_scramble_gpc(&f->scr, 1);
            q->t[i] = f->dsign;
        }
        break;
    case V90TX_RF: case V90TX_RF_BAR: {                  /* 8.8.4/V.92 */
        const v90_map_t *m = &t->map[V90TX_MAP_DATA];
        for (i = 0; i < 6; i++) {
            int n = f->fr * 6 + i;                       /* position in the 12-symbol pattern */
            q->u[i] = m->C[i][0];
            q->t[i] = (uint8_t)(((n & 3) < 2) ^ (f->sig == V90TX_RF_BAR));
        }
        break;
    }
    case V90TX_JD:
    case V90TX_JD_PRIME:
    case V90TX_JP:
    case V90TX_JP_PRIME:
        for (i = 0; i < 6; i++) {
            int bit = (f->sig == V90TX_JD || f->sig == V90TX_JP) ? seq_bit(t, f->fr * 6 + i) : 0;
            q->u[i] = t->uinfo;
            f->dsign ^= (uint8_t)v90_scramble_gpc(&f->scr, bit);
            q->t[i] = f->dsign;
        }
        break;
    case V90TX_DIL: {
        const v90_dil_t *d = &t->dil;
        uint8_t tr = d->train[f->seg];
        for (i = 0; i < 6; i++) {
            int m = f->fr * 6 + i;                       /* symbol index in the segment */
            q->u[i] = pat_bit(d->tp, m % d->ltp) ? tr : d->ref[tr >> 4];
            q->t[i] = (uint8_t)pat_bit(d->sp, m % d->lsp);
        }
        break;
    }
    case V90TX_RI: case V90TX_RI_BAR:
    case V90TX_RT: case V90TX_RT_BAR:
    case V90TX_RD: case V90TX_RD_BAR: {
        /* + + + - - -, inverted for the barred signals */
        int bar = f->sig == V90TX_RI_BAR || f->sig == V90TX_RT_BAR || f->sig == V90TX_RD_BAR;
        const v90_map_t *m = &t->map[uses_data_map(f->sig) ? V90TX_MAP_DATA : V90TX_MAP_TRAIN];
        for (i = 0; i < 6; i++) {
            q->u[i] = (f->sig == V90TX_RI || f->sig == V90TX_RI_BAR) ? t->uinfo : m->C[i][0];
            q->t[i] = (uint8_t)((i < 3) ^ bar);
        }
        break;
    }
    case V90TX_TRN2D: case V90TX_MP: case V90TX_MP_ACK:
    case V90TX_SUVD: case V90TX_SUVD_ACK: case V90TX_CPD: case V90TX_CPD_ACK:
    case V90TX_ED: case V90TX_B1D: case V90TX_DATA: {
        int dm = uses_data_map(f->sig);
        const v90_map_t *m = &t->map[dm ? V90TX_MAP_DATA : V90TX_MAP_TRAIN];
        uint8_t bits[V90_MAX_D];
        int D = V90_MAP_D(m);
        for (i = 0; i < D; i++) {
            int bit;
            switch (f->sig) {
            case V90TX_TRN2D: case V90TX_B1D: bit = 1; break;
            case V90TX_ED:                    bit = 0; break;
            case V90TX_DATA:                  bit = t->getbit(t->getbit_ctx) & 1; break;
            default:                          bit = seq_bit(t, f->fr * D + i); break;
            }
            bits[i] = (uint8_t)bit;
        }
        v90_scramble_gpc_bits(&f->scr, bits, D);
        v90_frame_encode(m, &f->diff, bits, q->u, q->t);
        if (m->Sr > 0) flags |= FL_SHAPED;
        if (dm) flags |= FL_DMAP;
        if (f->sig == V90TX_DATA) flags |= FL_USER;
        break;
    }
    default:                                             /* SILENCE */
        for (i = 0; i < 6; i++) { q->u[i] = 0; q->t[i] = 1; }
        break;
    }

    q->sig = f->sig;
    if (++f->fr >= f->nfr) {
        f->fr = 0;
        f->units++;
        flags |= FL_UEND;
        if (f->sig == V90TX_DIL && ++f->seg >= t->dil.n) f->seg = 0;
    }
    q->flags = flags;
}

/* ---- back ------------------------------------------------------------------ */

static const v90_map_t *frame_map(const v90tx_t *t, const v90tx_frame_t *q)
{
    return &t->map[(q->flags & FL_DMAP) ? V90TX_MAP_DATA : V90TX_MAP_TRAIN];
}

/* data frames the queue must hold to shape the head frame */
static int frames_needed(const v90tx_t *t)
{
    const v90tx_frame_t *q = &t->q[0];
    const v90_map_t *m;
    int F;
    if (!(q->flags & FL_SHAPED)) return 1;
    m = frame_map(t, q);
    if (m->Sr == 0) return 1;                            /* map replaced under us */
    F = 6 / m->Sr;
    /* the last shaping frame of the head starts at symbol 6 - F and looks
     * ld shaping frames beyond itself */
    return (6 + m->ld * F + 5) / 6;
}

static void emit_head(v90tx_t *t)
{
    const v90tx_frame_t *q = &t->q[0];
    uint8_t sign[6];
    int i;
    const v90_map_t *m = frame_map(t, q);

    if (q->flags & FL_RESET) v90_shaper_init(&t->sh);
    if (!(q->flags & FL_SHAPED) || m->Sr == 0) {
        memcpy(sign, q->t, 6);
    } else {
        int F = 6 / m->Sr, j, k, nf;
        for (j = 0; j < m->Sr; j++) {
            int32_t lin[24];
            uint8_t tt[24];
            /* collect this shaping frame and up to ld following ones, as far
             * as they are queued and belong to the same shaped stream */
            for (nf = 0; nf <= m->ld; nf++) {
                int pos = (j + nf) * F, qi = pos / 6, off = pos % 6;
                const v90tx_frame_t *g = &t->q[qi];
                if (qi >= t->qn) break;
                /* F divides 6, so a new data frame is always entered at its
                 * interval 0 and this test sees every frame boundary */
                if (qi > 0 && (!(g->flags & FL_SHAPED) || (g->flags & FL_RESET) ||
                               ((g->flags ^ q->flags) & FL_DMAP)))
                    break;
                for (k = 0; k < F; k++) {
                    lin[nf * F + k] = v90_ucode_linear(t->alaw, g->u[off + k]);
                    tt[nf * F + k] = g->t[off + k];
                }
            }
            (void)v90_shape(&t->sh, m, lin, tt, nf, sign + j * F);
        }
    }
    for (i = 0; i < 6; i++) t->out[i] = v90_pcm_octet(t->alaw, q->u[i], sign[i]);
}

/* move on to the next data frame */
static void advance(v90tx_t *t)
{
    int need;
    if (t->qn > 0) {
        t->qn--;
        memmove(&t->q[0], &t->q[1], t->qn * sizeof t->q[0]);
    }
    if (t->qn == 0) build_frame(t, &t->q[t->qn++]);
    need = frames_needed(t);
    while (t->qn < need) build_frame(t, &t->q[t->qn++]);
    emit_head(t);
}

/* ---- API ------------------------------------------------------------------- */

int v90tx_init(v90tx_t *t, int alaw, int uinfo)
{
    memset(t, 0, sizeof *t);
    t->alaw = alaw ? 1 : 0;
    t->pending = V90TX_NONE;
    t->cur_sig = V90TX_SILENCE;
    t->front.sig = V90TX_SILENCE;
    t->front.nfr = 1;
    t->seqp = t->seq;
    if (uinfo < 0 || uinfo > 111) return -1;
    t->uinfo = (uint8_t)uinfo;
    return 0;
}

void v90tx_set_getbit(v90tx_t *t, v90tx_getbit_fn fn, void *ctx)
{
    t->getbit = fn;
    t->getbit_ctx = ctx;
}

int v90tx_set_jd(v90tx_t *t, const v90_jd_t *jd)
{
    uint8_t bits[V90_JD_BITS];
    int r = v90_jd_build(jd, bits);
    if (r < 0) return r;
    t->jd = *jd;
    t->have |= HAVE_JD;
    return 0;
}

int v90tx_set_mp(v90tx_t *t, const v90_mp_t *mp)
{
    uint8_t bits[V90_MP_MAX_BITS];
    int r = v90_mp_build(mp, bits);
    if (r < 0) return r;
    t->mp = *mp;
    t->have |= HAVE_MP;
    return 0;
}

int v90tx_load_dil(v90tx_t *t, const v90_dil_t *d)
{
    int i;
    if (d->lsp < 1 || d->lsp > 128 || d->ltp < 1 || d->ltp > 128) return V90_ERR_RANGE;
    for (i = 0; i < 8; i++)
        if (d->h[i] > 127 || d->ref[i] > 127) return V90_ERR_RANGE;
    for (i = 0; i < d->n; i++)
        if (d->train[i] > 127) return V90_ERR_RANGE;
    t->dil = *d;
    t->have |= HAVE_DIL;
    return 0;
}

int v90tx_set_xseq(v90tx_t *t, int which, const uint8_t *packed, int nbits)
{
    if (which < 0 || which >= V90TX_NXSEQ || !packed || nbits < 1 || nbits > 65535) return V90_ERR_RANGE;
    t->xseq[which] = packed;
    t->xlen[which] = (uint16_t)nbits;
    return 0;
}

void v90_pack_bits(const uint8_t *bits, int n, uint8_t *packed)
{
    int i;
    memset(packed, 0, (size_t)((n + 7) / 8));
    for (i = 0; i < n; i++)
        if (bits[i]) packed[i >> 3] |= (uint8_t)(1u << (i & 7));
}

int v90tx_set_map(v90tx_t *t, int which, const v90_map_t *m)
{
    int r;
    if (which != V90TX_MAP_TRAIN && which != V90TX_MAP_DATA) return V90_ERR_RANGE;
    r = v90_map_check(m);
    if (r < 0) return r;
    t->map[which] = *m;
    t->have |= (which == V90TX_MAP_DATA) ? HAVE_DATA : HAVE_TRAIN;
    return 0;
}

int v90tx_load_cp(v90tx_t *t, const v90_cp_t *cp)
{
    /* built on the stack (about 780 bytes) so that a refused CP leaves the
     * installed map untouched */
    v90_map_t m;
    int r = v90_map_from_cp(&m, cp);
    if (r != 0) return r;
    return v90tx_set_map(t, cp->is_cp ? V90TX_MAP_DATA : V90TX_MAP_TRAIN, &m);
}

int v90tx_select(v90tx_t *t, v90tx_sig_t sig)
{
    int i, cut = -1;
    if (sig < 0 || sig >= V90TX_NSIG || !available(t, sig)) return -1;
    t->pending = (int8_t)sig;
    /* Drop frames built ahead from the earliest point at which this
     * selection would have been honoured, unless that would lose user bits. */
    for (i = t->qn - 1; i >= 1; i--) {
        if (t->q[i].flags & FL_USER) break;
        if (t->q[i].flags & FL_SWITCH) cut = i;
    }
    if (cut >= 0) {
        t->front = t->q[cut].snap;
        t->qn = (uint8_t)cut;
    }
    return 0;
}

uint8_t v90tx_tick(v90tx_t *t)
{
    const v90tx_frame_t *q;
    uint8_t o;
    if (!t->aligned && t->pending != V90TX_NONE) {
        /* first signal after init: start now, this symbol is interval 0 */
        t->aligned = 1;
        t->phase = 0;
        t->qn = 0;
    }
    if (t->phase == 0) {
        advance(t);
        if (t->q[0].flags & FL_FIRST) {
            t->cur_sig = t->q[0].sig;
            t->nsym = 0;
            t->nunits = 0;
        }
    }
    q = &t->q[0];
    o = t->out[t->phase];
    t->nsym++;
    if (++t->phase == 6) {
        t->phase = 0;
        if (q->flags & FL_UEND) t->nunits++;
    }
    return o;
}

v90tx_sig_t v90tx_signal(const v90tx_t *t) { return (v90tx_sig_t)t->cur_sig; }
uint32_t v90tx_symbols(const v90tx_t *t)   { return t->nsym; }
uint32_t v90tx_units(const v90tx_t *t)     { return t->nunits; }
v90tx_sig_t v90tx_pending(const v90tx_t *t) { return (v90tx_sig_t)t->pending; }
int v90tx_phase(const v90tx_t *t)          { return t->phase; }
