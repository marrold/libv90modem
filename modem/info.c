/* V.90 / V.34 Phase 2 INFO frame codec. See info.h.
 *
 * Spec reading (flagged in v90d's PROGRESS.md): Figure 14/V.34 draws the CRC register
 * with cells 15..0, the data entering at the bit-0 end and gaps after cells 11
 * and 4. Read as: feedback = data XOR bit 0; the register shifts towards bit
 * 0; the feedback enters cells 15, 10 and 3. That is x^16+x^12+x^5+1 in the
 * bit-reversed arrangement, and "output starting with bit 0" then sends the
 * remainder so that a receiver running the same register over information +
 * CRC ends at zero. */
#include "info.h"
#include "../v90/v90_seq.h"     /* v90_crc: the one copy of the register */

uint16_t info_crc(const uint8_t *bits, int n) { return v90_crc(bits, n); }

int info_check(const uint8_t *payload, int npayload)
{
    return info_crc(payload, npayload) == 0;
}

/* ---- bit cursor ---- */
typedef struct { uint8_t *b; int n; } wr_t;
typedef struct { const uint8_t *b; int n; } rd_t;

static void put(wr_t *w, unsigned v, int nbits)
{
    int k;
    for (k = 0; k < nbits; k++) w->b[w->n++] = (uint8_t)((v >> k) & 1);
}

static unsigned get(rd_t *r, int nbits)
{
    unsigned v = 0; int k;
    for (k = 0; k < nbits; k++) v |= (unsigned)(r->b[r->n++] & 1) << k;
    return v;
}

static void head(wr_t *w, uint8_t *bits)
{
    w->b = bits; w->n = 0;
    put(w, 0xF, 4);
    put(w, 0x4E, 8);        /* 01110010, left-most first = 0x4E LSB first */
}

static int tail(wr_t *w)
{
    uint16_t c = info_crc(w->b + INFO_HDR_BITS, w->n - INFO_HDR_BITS);
    put(w, c, 16);
    put(w, 0xF, 4);
    return w->n;
}

static void put_info0(wr_t *w, const info0_t *i, int digital)
{
    put(w, i->sr2743, 1); put(w, i->sr2800, 1); put(w, i->sr3429, 1);
    put(w, i->lo3000, 1); put(w, i->hi3000, 1); put(w, i->lo3200, 1); put(w, i->hi3200, 1);
    put(w, i->allow3429, 1); put(w, i->pwr_reduce, 1);
    put(w, i->max_sr_diff, 3);
    put(w, i->cme, 1); put(w, i->c1664, 1);
    /* bits 26, 27: reserved in V.90; Tables 15 and 16 of V.92 */
    put(w, digital ? i->short_p2 : i->v92, 1);
    put(w, digital ? i->v92 : i->short_p2, 1);
    put(w, i->ack, 1);
}

static void get_info0(rd_t *r, info0_t *i, int digital)
{
    unsigned b26, b27;
    i->sr2743 = (uint8_t)get(r, 1); i->sr2800 = (uint8_t)get(r, 1); i->sr3429 = (uint8_t)get(r, 1);
    i->lo3000 = (uint8_t)get(r, 1); i->hi3000 = (uint8_t)get(r, 1);
    i->lo3200 = (uint8_t)get(r, 1); i->hi3200 = (uint8_t)get(r, 1);
    i->allow3429 = (uint8_t)get(r, 1); i->pwr_reduce = (uint8_t)get(r, 1);
    i->max_sr_diff = (uint8_t)get(r, 3);
    i->cme = (uint8_t)get(r, 1); i->c1664 = (uint8_t)get(r, 1);
    b26 = get(r, 1); b27 = get(r, 1);
    i->short_p2 = (uint8_t)(digital ? b26 : b27);
    i->v92 = (uint8_t)(digital ? b27 : b26);
    i->ack = (uint8_t)get(r, 1);
    i->tx_power = i->max_tx_power = i->pwr_at_codec = i->alaw = i->up3429 = 0;
}

int info0d_build(const info0_t *i, uint8_t *bits)
{
    wr_t w;
    head(&w, bits);
    put_info0(&w, i, 1);
    put(&w, i->tx_power, 4);
    put(&w, i->max_tx_power, 5);
    put(&w, i->pwr_at_codec, 1); put(&w, i->alaw, 1); put(&w, i->up3429, 1);
    put(&w, 0, 1);
    return tail(&w);
}

void info0d_parse(const uint8_t *payload, info0_t *i)
{
    rd_t r; r.b = payload; r.n = 0;
    get_info0(&r, i, 1);
    i->tx_power = (uint8_t)get(&r, 4);
    i->max_tx_power = (uint8_t)get(&r, 5);
    i->pwr_at_codec = (uint8_t)get(&r, 1);
    i->alaw = (uint8_t)get(&r, 1);
    i->up3429 = (uint8_t)get(&r, 1);
}

int info0a_build(const info0_t *i, uint8_t *bits)
{
    wr_t w;
    head(&w, bits);
    put_info0(&w, i, 0);
    return tail(&w);
}

void info0a_parse(const uint8_t *payload, info0_t *i)
{
    rd_t r; r.b = payload; r.n = 0;
    get_info0(&r, i, 0);
}

static int16_t sext10(unsigned v)
{
    return (int16_t)((v & 0x200) ? (int)v - 1024 : (int)v);
}

int info1d_build(const info1d_t *i, uint8_t *bits)
{
    wr_t w; int k;
    head(&w, bits);
    put(&w, i->min_pwr_red, 3);
    put(&w, i->add_pwr_red, 3);
    put(&w, i->md_len, 7);
    for (k = 0; k < 6; k++) {
        put(&w, i->sr[k].hi_carrier, 1);
        put(&w, i->sr[k].preemph, 4);
        put(&w, i->sr[k].max_rate, 4);
    }
    put(&w, (unsigned)i->freq_off & 0x3ff, 10);
    return tail(&w);
}

int info1d_build_v92(const info1d_t *i, int pcm_up, uint8_t *bits)
{
    int n = info1d_build(i, bits);
    bits[70] = (uint8_t)(pcm_up != 0);      /* Table 17/V.92 */
    /* the CRC again, over the changed bit */
    {
        wr_t w; w.b = bits; w.n = n - 20;
        return tail(&w);
    }
}

void info1d_parse(const uint8_t *payload, info1d_t *i)
{
    rd_t r; int k;
    r.b = payload; r.n = 0;
    i->min_pwr_red = (uint8_t)get(&r, 3);
    i->add_pwr_red = (uint8_t)get(&r, 3);
    i->md_len = (uint8_t)get(&r, 7);
    for (k = 0; k < 6; k++) {
        i->sr[k].hi_carrier = (uint8_t)get(&r, 1);
        i->sr[k].preemph = (uint8_t)get(&r, 4);
        i->sr[k].max_rate = (uint8_t)get(&r, 4);
    }
    i->freq_off = sext10(get(&r, 10));
}

int info1a_build(const info1a_t *i, uint8_t *bits)
{
    wr_t w;
    head(&w, bits);
    if (i->pcm) {                           /* Table 18/V.92 */
        put(&w, i->sections, 2);
        put(&w, i->ltot_code, 2);
        put(&w, i->lmax_code, 2);
        put(&w, i->md_len, 7);
        put(&w, i->uinfo, 7);
        put(&w, 0, 2);
        put(&w, 6, 3);
        put(&w, 6, 3);
        put(&w, 0x3ff, 10);                 /* "set to 1 to avoid generating a tone" */
        return tail(&w);
    }
    if (i->v90) {
        put(&w, 0, 6);
        put(&w, i->md_len, 7);
        put(&w, i->uinfo, 7);
        put(&w, 0, 2);
        put(&w, i->up_sr, 3);
        put(&w, 6, 3);
    } else {
        put(&w, i->min_pwr_red, 3);
        put(&w, i->add_pwr_red, 3);
        put(&w, i->md_len, 7);
        put(&w, i->hi_carrier, 1);
        put(&w, i->preemph, 4);
        put(&w, i->max_rate, 4);
        put(&w, i->up_sr, 3);
        put(&w, i->down_sr, 3);
    }
    put(&w, (unsigned)i->freq_off & 0x3ff, 10);
    return tail(&w);
}

void info1a_parse(const uint8_t *payload, info1a_t *i)
{
    rd_t r;
    unsigned sel = (unsigned)(payload[25] | (payload[26] << 1) | (payload[27] << 2)); /* bits 37:39 */
    r.b = payload; r.n = 0;
    i->v90 = (sel == 6);
    i->uinfo = 0; i->min_pwr_red = i->add_pwr_red = 0;
    i->hi_carrier = i->preemph = i->max_rate = 0; i->down_sr = 0;
    i->pcm = 0; i->sections = i->ltot_code = i->lmax_code = 0;
    if (i->v90) {
        /* bits 12:17 are reserved (zero) in Table 10/V.90 and carry the
         * filter capabilities in Table 18/V.92 */
        i->sections = (uint8_t)get(&r, 2);
        i->ltot_code = (uint8_t)get(&r, 2);
        i->lmax_code = (uint8_t)get(&r, 2);
        i->md_len = (uint8_t)get(&r, 7);
        i->uinfo = (uint8_t)get(&r, 7);
        (void)get(&r, 2);
        i->up_sr = (uint8_t)get(&r, 3);
        (void)get(&r, 3);
        i->pcm = (uint8_t)(i->up_sr == 6);
        if (!i->pcm) i->sections = i->ltot_code = i->lmax_code = 0;
    } else {
        i->min_pwr_red = (uint8_t)get(&r, 3);
        i->add_pwr_red = (uint8_t)get(&r, 3);
        i->md_len = (uint8_t)get(&r, 7);
        i->hi_carrier = (uint8_t)get(&r, 1);
        i->preemph = (uint8_t)get(&r, 4);
        i->max_rate = (uint8_t)get(&r, 4);
        i->up_sr = (uint8_t)get(&r, 3);
        i->down_sr = (uint8_t)get(&r, 3);
    }
    i->freq_off = sext10(get(&r, 10));
}

int info1a_kind(const info1a_t *i)
{
    return !i->v90 ? INFO1A_V34 : i->pcm ? INFO1A_PCM : INFO1A_V90;
}
