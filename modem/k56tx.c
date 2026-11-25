/* K56flex's training transmitter. See k56tx.h.
 *
 * The segments, levels and sign rules are docs/history/k56-facts.md's
 * "Training down", "Phase 4" and "Rate renegotiation", as D-Modem
 * (https://github.com/D-Modem/D-Modem at c06a380d, its notes on K56flex)
 * describes the training SM56 accepts. Facts only; no code of D-Modem. MP
 * and MP' are V.34's type 0 (modem/v34mp), J is V.34's (v34_j16), the
 * scrambler V.34's GPA (v90/v90_scr). */
#include <string.h>
#include "k56tx.h"
#include "clock.h"
#include "k56tab.h"
#include "../v90/v90_pcm.h"

#define R_MAG       0x54
#define R_BAR_FROM  264     /* R's last 24 symbols are R-bar */
#define TRN1A_MAG0  0x4e    /* hist bit 24 clear */
#define TRN1A_MAG1  0x4f
#define ONE_MAG     0x4f    /* TRN1C, Js, TRN2A, MARK */
#define B1_FRAMES   6
#define GRID        24
/* a multiple of 24 that keeps the symbol count in range over a long call */
#define SYM_WRAP    0x18000000

static const uint8_t trn1b_mag[8] = { 0x40, 0x41, 0x4b, 0x4c, 0x50, 0x51, 0x50, 0x51 };
/* pad 0..2: 2 bits a symbol (d0 + 2 d1), 1 bit a symbol */
static const uint8_t pad2_mag[3][4] = {
    { 0x2f, 0x45, 0x51, 0x57 }, { 0x35, 0x4d, 0x58, 0x60 }, { 0x3d, 0x54, 0x61, 0x67 }
};
static const uint8_t pad1_mag[3][2] = { { 0x3d, 0x55 }, { 0x44, 0x5d }, { 0x4c, 0x65 } };

static int32_t sil2_len(int32_t n)
{
    int32_t l = (n + 7) & ~(int32_t)7;
    return l < K56TX_SIL2_MIN ? K56TX_SIL2_MIN : l;
}

static void enter(k56tx_t *t, int seg)
{
    t->seg = (uint8_t)seg;
    t->n = 0;
    t->nbit = 0;
    switch (seg) {
    case K56TX_SIL1:  t->len = K56TX_SIL1_N; break;
    case K56TX_R1:
    case K56TX_R2:    t->len = K56TX_R_N; break;
    case K56TX_TRN1A: t->len = K56TX_TRN1A_N; break;
    case K56TX_TRN1B: t->len = K56TX_TRN1B_N; t->alt = 0; break;
    case K56TX_TRN1C: t->len = K56TX_TRN1C_N; break;
    case K56TX_JS:    t->len = K56TX_JS_N; break;
    case K56TX_SIL2:  t->len = t->j_in ? sil2_len(0) : -1; break;
    case K56TX_TRN2A: t->len = t->reneg ? K56TX_RN_TRN2A_N : K56TX_TRN2A_N; t->alt = 0; break;
    case K56TX_MARK:  t->len = K56TX_MARK_N; break;
    case K56TX_TRN2B: t->len = -1; t->nb = 2; break;
    case K56TX_MP:
    case K56TX_MPA:   t->len = K56TX_MP_N; t->nb = t->reneg ? 1 : 2; break;
    case K56TX_E:     t->len = t->reneg ? K56TX_RN_E_N : K56TX_E_N; break;
    default:          /* data: a new encoder and scrambler (Ruling C4) */
        t->len = -1;
        k56enc_init(&t->enc, t->d_rate, t->d_pad, t->d_rbs);
        v90_scr_init(&t->dscr);
        t->b1 = B1_FRAMES;
        t->fpos = K56ENC_N;
        break;
    }
}

/* the open ends: silence 2 once the J is in, TRN2B once the MP is in and
 * the grid holds */
static void settle(k56tx_t *t)
{
    if (t->seg == K56TX_SIL2 && t->j_in && t->n >= t->len)
        enter(t, K56TX_R2);
    if (t->seg == K56TX_TRN2B && t->mp_in && t->n >= K56TX_TRN2B_MIN &&
        (t->sym + (t->reneg ? K56TX_RN_TO_DATA : K56TX_TO_DATA)) % GRID == 0)
        enter(t, K56TX_MP);
}

void k56tx_init(k56tx_t *t)
{
    memset(t, 0, sizeof *t);
    v90_scr_init(&t->scr);
    v90_scr_init(&t->dscr);
    t->sym = -(K56TX_SIL1_N + K56TX_R_N + K56TX_TRN1A_N);
    t->nb = 1;
    enter(t, K56TX_SIL1);
}

/* the client looks for R in our data, at a frame boundary: in the middle
 * of a frame, or with our MP out but our data not yet begun (its S in its
 * B1: Task 10d), R2 follows that frame, or our data's first */
void k56tx_reneg(k56tx_t *t)
{
    t->mp_in = 0;
    if ((t->seg == K56TX_DATA && t->fpos != K56ENC_N) || (t->seg >= K56TX_MP && t->seg < K56TX_DATA)) {
        t->reneg_due = 1;
        return;
    }
    t->reneg = 1;
    t->reneg_due = 0;
    enter(t, K56TX_R2);
}

void k56tx_got_j(k56tx_t *t, uint8_t j_octet)
{
    int pad = j_octet >> 6;
    if (pad > 2 || t->j_in || t->seg > K56TX_SIL2)
        return;
    t->j_in = 1;
    t->pad = (uint8_t)pad;
    if (t->seg == K56TX_SIL2) {
        t->len = sil2_len(t->n);
        settle(t);
    }
}

void k56tx_set_mp(k56tx_t *t, const v34_mp_t *mp)
{
    v34_mp_t m = *mp;
    m.type = 0;
    m.ack = 0;
    if (v34_mp_build(&m, t->mpbits[0]) != V34_MP0_BITS)
        return;
    m.ack = 1;
    (void)v34_mp_build(&m, t->mpbits[1]);
    t->mp_in = 1;
    settle(t);
}

void k56tx_data(k56tx_t *t, int rate, int pad, uint8_t rbs,
                int (*getbit)(void *), void *user)
{
    /* out of range: an encoder that sends zeros (k56enc_init) */
    t->d_rate = (uint8_t)(rate >= 1 && rate <= K56_NRATES ? rate : 0);
    t->d_pad = (uint8_t)(pad >= 0 && pad <= 2 ? pad : 3);
    t->d_rbs = rbs;
    t->getbit = getbit;
    t->user = user;
}

/* one training bit through the scrambler, into the history */
static int scr(k56tx_t *t, int bit)
{
    int b = v90_scramble_gpa(&t->scr, bit);
    t->hist = t->hist << 1 | (uint32_t)b;
    return b;
}

/* the octet of magnitude m, its value into the running sum */
static uint8_t code(k56tx_t *t, int m, int positive)
{
    uint8_t o = (uint8_t)(positive ? 0xff - m : 0x7f - m);
    t->sum += v90_octet_linear(0, o);
    return o;
}

/* an odd symbol's sign: against the running sum */
static int sum_sign(const k56tx_t *t) { return t->sum <= 0; }

static int hist(const k56tx_t *t, int k) { return (int)((t->hist >> k) & 1u); }

/* the next input bit of TRN2B (the 2-bit value 1: a one, then a zero),
 * MP, MP' (their 88 bits round and round) or E (ones) */
static int p4_in(k56tx_t *t)
{
    int32_t k = t->nbit++;
    switch (t->seg) {
    case K56TX_TRN2B: return !(k & 1);
    case K56TX_E:     return 1;
    default:          return t->mpbits[t->seg == K56TX_MPA][k % V34_MP0_BITS];
    }
}

/* TRN2B, MP, MP', E: nb scrambled input bits a symbol on the pad's
 * magnitudes (2: d0 + 2 d1 of 4; 1: of 2); even: negative when hist bit
 * 24 is set; odd: the running sum's */
static uint8_t p4_symbol(k56tx_t *t, int even)
{
    int b0 = scr(t, p4_in(t)), m;
    if (t->nb == 2)
        m = pad2_mag[t->pad][b0 + 2 * scr(t, p4_in(t))];
    else
        m = pad1_mag[t->pad][b0];
    return code(t, m, even ? !hist(t, 24) : sum_sign(t));
}

static uint8_t data_symbol(k56tx_t *t)
{
    if (t->fpos == K56ENC_N) {
        uint8_t bits[64];
        int nb = k56enc_bits(&t->enc), i;
        for (i = 0; i < nb; i++) {
            int b = t->b1 || !t->getbit ? 1 : t->getbit(t->user) & 1;
            bits[i] = (uint8_t)b;
        }
        v90_scramble_gpa_bits(&t->dscr, bits, nb);
        k56enc_frame(&t->enc, bits, t->frame);
        if (t->b1)
            t->b1--;
        t->fpos = 0;
    }
    return t->frame[t->fpos++];
}

uint8_t k56tx_tick(k56tx_t *t)
{
    int even = !(t->alt & 1);
    uint8_t o;

    switch (t->seg) {
    case K56TX_SIL1:
    case K56TX_SIL2:
        o = 0xff;
        break;
    case K56TX_R1:
    case K56TX_R2: {
        int pos = !((t->n >> 1) & 1);
        if (t->n >= R_BAR_FROM)
            pos = !pos;
        o = (uint8_t)(pos ? 0xff - R_MAG : 0x7f - R_MAG);
        break;
    }
    case K56TX_TRN1A: {
        int b = scr(t, 1);
        o = code(t, hist(t, 24) ? TRN1A_MAG1 : TRN1A_MAG0, b);
        break;
    }
    case K56TX_TRN1B: {
        int b0 = scr(t, 1), b1 = scr(t, 1), b2 = scr(t, 1);
        o = code(t, trn1b_mag[b0 + 2 * b1 + 4 * b2], even ? hist(t, 23) : sum_sign(t));
        break;
    }
    case K56TX_TRN1C:
    case K56TX_TRN2A:
    case K56TX_MARK:
    case K56TX_JS:
        if (even) {
            /* the input value (1, 0, J's bit), nb bits of it lowest
             * first; the sign is the first scrambled bit */
            int in = t->seg == K56TX_MARK ? 0
                   : t->seg == K56TX_JS ? v34_j16[(t->n >> 1) & 15] : 1;
            int b = scr(t, in);
            if (t->nb == 2)
                (void)scr(t, 0);
            o = code(t, ONE_MAG, b);
        } else {
            o = code(t, ONE_MAG, sum_sign(t));
        }
        break;
    case K56TX_TRN2B:
    case K56TX_MP:
    case K56TX_MPA:
    case K56TX_E:
        o = p4_symbol(t, even);
        break;
    default:
        o = data_symbol(t);
        break;
    }

    t->n = clk_sat_inc(t->n);              /* (neither is read in data) */
    t->alt = clk_sat_inc(t->alt);
    if (++t->sym >= 2 * SYM_WRAP)
        t->sym -= SYM_WRAP;
    if (t->seg == K56TX_DATA) {
        if (t->reneg_due && t->fpos == K56ENC_N) {
            t->reneg = 1;
            t->reneg_due = 0;
            enter(t, K56TX_R2);
        }
    } else if (t->len >= 0 && t->n >= t->len) {
        enter(t, t->seg + 1);
    }
    settle(t);
    return o;
}

k56tx_seg k56tx_segment(const k56tx_t *t) { return (k56tx_seg)t->seg; }

int32_t k56tx_sym(const k56tx_t *t) { return t->sym; }

int k56tx_rate(const k56tx_t *t)
{
    return t->seg == K56TX_DATA && t->enc.rate ? 30000 + 2000 * t->enc.rate : 0;
}
