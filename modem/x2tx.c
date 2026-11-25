/* x2's training script. See x2tx.h.
 *
 * The segments, their lengths, levels and order are docs/history/x2-facts.md
 * "Phase 3 script": our disassembly of the I-modem's DSP (Ie030002, the
 * script at c915-c935), which D-Modem (https://github.com/D-Modem/D-Modem,
 * docs/dsp/x2-details.md "Phase 3") and courier-emu
 * (https://github.com/cryan209/courier-emu, its server script) agree with.
 * Facts only; no code of either. */
#include <string.h>
#include "x2tx.h"
#include "clock.h"
#include "x2rec.h"

#define POS   0x80              /* bit 7 of a mu-law octet: positive */
#define SIL   0x7f
#define LV_TRN  0xb1            /* TRN and J words: 3772 linear */
#define LV_ONES 0xc1            /* ones: 1820 linear, 6.3 dB below */
#define LV_PRE0 0xab            /* the preamble's two levels */
#define LV_PRE1 0xbd
#define PRE_SIGNS 0x05b8        /* 11 bits LSB first, 1 negative (Barker 11) */
#define PRE_MAGS  0x0712        /* 11 bits LSB first, 0 LV_PRE0, complemented each other 11 */
#define JW_WORD   0x09B0
#define JW_LAST   0x099F
#define JW_BITS   12

static const uint8_t align_tab[X2MAP_N] = { 0x9e, 0x28, 0xa1, 0x21, 0xa8, 0x1e };

/* octet k (0..351) of the 16 periods of the preamble */
static uint8_t pre_octet(int k)
{
    int j = k % 11, odd = (k / 11) & 1;
    uint8_t lv = (((PRE_MAGS >> j) & 1) ^ odd) ? LV_PRE1 : LV_PRE0;
    return ((PRE_SIGNS >> j) & 1) ? (uint8_t)(lv & 0x7f) : lv;
}

/* a scrambled bit sent at a level: positive for a scrambled 0 */
static uint8_t sbit(x2tx_t *t, int bit, uint8_t lv)
{
    return (uint8_t)(lv ^ (v90_scramble_gpc(&t->scr, bit) << 7));
}

static void enter(x2tx_t *t, x2tx_seg s) { t->seg = s; t->n = 0; t->fpos = X2MAP_N; }

void x2tx_init(x2tx_t *t)
{
    static const uint16_t w[3] = { X2TX_REC_W0, X2TX_REC_W1, X2TX_REC_W2 };
    memset(t, 0, sizeof *t);
    v90_scr_init(&t->scr);
    x2map_init(&t->map, 1, 0, 1);
    /* our record, the acknowledge set (we send it once the client's MP is
     * in), padded with zeros to 4 frames; without it for a rate change's
     * first ten */
    (void)x2rec_build(w, 3, 1, t->rec);
    (void)x2rec_build(w, 3, 0, t->rec0);
    t->index = 1; t->pad_mode = 0;
    enter(t, X2TX_SILENCE);
}

void x2tx_start(x2tx_t *t)
{
    if (t->seg == X2TX_SILENCE) enter(t, X2TX_LEAD);
}

void x2tx_data(x2tx_t *t, int index, int pad_mode, x2tx_getbit_fn getbit, void *user)
{
    t->index = index; t->pad_mode = pad_mode;
    t->getbit = getbit; t->user = user;
}

void x2tx_go(x2tx_t *t) { t->go = 1; }

int x2tx_rate(const x2tx_t *t)
{
    x2map_t m;
    x2map_init(&m, t->index, t->pad_mode, 0);
    return x2map_bits(&m) * 8000 / X2MAP_N;
}

int x2tx_data_next(const x2tx_t *t)
{
    if (t->seg == X2TX_DATA) return !t->go;
    /* (a rate change pending: its marker follows this start, not data) */
    return !t->go && t->seg == X2TX_START && t->n > (int32_t)(X2TX_START_FRAMES + t->again - 1) * X2MAP_N;
}

/* the next frame of the mapped segments; returns 1, or 0 when the segment
 * is over before it */
static int next_frame(x2tx_t *t)
{
    uint8_t bits[64];
    int nb, k;
    switch (t->seg) {
    case X2TX_SYNC:
        if (t->go && t->n >= X2TX_SYNC_MIN) {
            t->go = 0; t->rec_frame = 0; t->records = 0;
            enter(t, X2TX_REC);
            return next_frame(t);
        }
        nb = x2map_bits(&t->map);
        for (k = 0; k < nb; k++) bits[k] = 1;
        break;
    case X2TX_REC:
        if (t->rec_frame == 0) {
            if (t->go) {
                t->go = 0;
                enter(t, X2TX_START);
                return next_frame(t);
            }
            t->records++;
        }
        nb = x2map_bits(&t->map);
        for (k = 0; k < nb; k++) bits[k] = t->rec[t->rec_frame * nb + k];
        t->rec_frame = (t->rec_frame + 1) & 3;
        break;
    case X2TX_START:
        /* the start-up's: one frame in levels A, 680 in the data levels; a
         * rate change's: one in levels B (the map is there already), 681 */
        if (t->n >= (X2TX_START_FRAMES + t->again) * X2MAP_N) {
            t->again = 0;
            enter(t, X2TX_DATA);
            return next_frame(t);
        }
        if (t->n == 0) {
            if (!t->again) t->map.levels_a = 1;
        } else if (t->n == X2MAP_N) {
            /* the data levels from the start's second frame; the signs and
             * the running sum carry on */
            t->map.levels_a = 0; t->map.index = t->index; t->map.pad_mode = t->pad_mode;
        }
        nb = x2map_bits(&t->map);
        for (k = 0; k < nb; k++) bits[k] = 1;
        break;
    case X2TX_MARK: {
        /* each position at its largest level in the rate in use (t->map's
         * index), sent for the pad the client's MP reports (t->pad_mode:
         * from its MP on all we send is for it); not scrambled, no
         * differential signs */
        x2map_t cur;
        const uint8_t *rv, *sn;
        int f = (int)(t->n / X2MAP_N);
        if (f == X2TX_MARK_FRAMES) {
            v90_scr_init(&t->scr);
            x2map_init(&t->map, 1, t->pad_mode, 0);     /* levels B, the sign positive */
            t->rec_frame = 0; t->records = 0;
            enter(t, X2TX_RREC);
            return next_frame(t);
        }
        x2map_init(&cur, t->map.index, t->pad_mode, 0);
        for (k = 0; k < X2MAP_N; k++) {
            /* the 0xff cannot come: the index is 1..15 and the pad passed on_mp */
            uint8_t hi = x2map_list(&cur, k, &rv, &sn) ? sn[0] : 0xff;
            t->frame[k] = ((k < 3) == (f < X2TX_MARK_FRAMES - 1)) ? hi : (uint8_t)(hi & 0x7f);
        }
        t->fpos = 0;
        return 1;
    }
    case X2TX_RREC: {
        const uint8_t *rec;
        if (t->rec_frame == 0) {
            if (t->records == X2TX_RREC_N) {
                t->again = 1;
                enter(t, X2TX_START);
                return next_frame(t);
            }
            t->records++;
        }
        rec = t->records <= X2TX_RREC_NAK ? t->rec0 : t->rec;
        nb = x2map_bits(&t->map);
        for (k = 0; k < nb; k++) bits[k] = rec[t->rec_frame * nb + k];
        t->rec_frame = (t->rec_frame + 1) & 3;
        break;
    }
    default:    /* X2TX_DATA */
        if (t->go) {
            /* the client's rate change: its MP is in */
            t->go = 0;
            enter(t, X2TX_MARK);
            return next_frame(t);
        }
        nb = x2map_bits(&t->map);
        for (k = 0; k < nb; k++) bits[k] = t->getbit ? (uint8_t)(t->getbit(t->user) & 1) : 1;
        break;
    }
    v90_scramble_gpc_bits(&t->scr, bits, nb);
    x2map_frame(&t->map, bits, t->frame);
    t->fpos = 0;
    return 1;
}

uint8_t x2tx_tick(x2tx_t *t)
{
    uint8_t o;
    switch (t->seg) {
    case X2TX_SILENCE:
        return SIL;
    case X2TX_LEAD:
        o = SIL;
        if (++t->n == X2TX_LEAD_N) enter(t, X2TX_PRE);
        return o;
    case X2TX_PRE:
        /* 16 periods, then the first 11 octets with the sign turned */
        o = t->n < 352 ? pre_octet(t->n) : (uint8_t)(pre_octet(t->n - 352) ^ POS);
        if (++t->n == X2TX_PRE_N) enter(t, X2TX_TRN);
        return o;
    case X2TX_TRN:
        o = sbit(t, 1, LV_TRN);
        if (++t->n == X2TX_TRN_N) enter(t, X2TX_JW);
        return o;
    case X2TX_JW: {
        int w = t->n / JW_BITS;
        o = sbit(t, (((w < 99 ? JW_WORD : JW_LAST) >> (t->n % JW_BITS)) & 1), LV_TRN);
        if (++t->n == X2TX_JW_N) enter(t, X2TX_ONES);
        return o;
    }
    case X2TX_ONES:
        o = sbit(t, 1, LV_ONES);
        if (++t->n == X2TX_ONES_N) enter(t, X2TX_ALIGN);
        return o;
    case X2TX_ALIGN:
        o = align_tab[(t->n % X2MAP_N + t->n / X2MAP_N) % X2MAP_N];
        if (++t->n == X2TX_ALIGN_N) {
            /* the first sync frame: the differential sign positive, the
             * running sum from zero */
            x2map_init(&t->map, 1, 0, 1);
            enter(t, X2TX_SYNC);
        }
        return o;
    default:
        if (t->fpos == X2MAP_N) (void)next_frame(t);
        t->n = clk_sat_inc(t->n);          /* (not read in data) */
        return t->frame[t->fpos++];
    }
}
