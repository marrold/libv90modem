/* V.90 analogue modem: the digital impairment learning sequence (DIL) it asks
 * for, what it learns from it, and the constellations it asks for in CPt
 * and CP (V.90 8.3.1, 8.4.1, 8.5.2).
 *
 * The DIL asked for (v90dil_design). Every Ucode 0..100 is a training
 * symbol, one DIL-segment each (N = 101), in the order 0 2 4 ... 100 99
 * 97 ... 3 1 (no segment follows a much larger one: see v90dil.c); a segment is 18
 * symbols (Hc = 2 for every Uchord). Each segment opens with REFc = UINFO
 * three times, all positive, and then 15 training symbols, the first of them
 * positive too (TP = 0 0 0 1 ..., SP = + + + + - - + - + + - - - + - + - +,
 * both 18 long, so restarting with every segment). The opening is Ri's
 * (8.6.4: UINFO + + + - - -): when the digital modem ends the DIL with Ri at
 * a segment boundary, the first three symbols are the same either way and
 * the fourth, a positive training symbol or -UINFO, tells the two apart
 * while the receiver's decision feedback still holds nothing wrong. A pass
 * is 1818 symbols (227 ms) and shows each Ucode three times in intervals 3,
 * 4 and 5 and twice in 0, 1 and 2. Ucode 100 is the loudest at or under a
 * third of full scale in either law (Table 1: 2591 of 8031 mu-law, 2624 of
 * 8064 A-law); 101..127 are left out. A softphone's playback gain control
 * (a live V.90 call through one, reported by an independent softmodem)
 * passed every codeword to about a third of full scale exactly, held down
 * louder ones, and went on reading what followed low for a third of a
 * second: a DIL that asks for them is misread from there on. In runs of 15
 * they also overload the receiver's 16-bit input (modem/v90arx puts Sd at an
 * RMS of 4096 there; measured, Ucode 127 arrived 9 % low). The
 * constellations never use what was not measured, so none goes above a
 * third of full scale either. Within Table 15's limits at -12 dBm0 that
 * costs most of our simulated lines nothing; some move a step either way
 * (test_orig_v90's 6 km line 48000 to 46666, test_api's V.90 call 49333 to
 * 52000).
 *
 * The analysis (v90dil_acc_* , v90dil_analyse): the equaliser output of
 * every DIL symbol, sign taken off, summed per interval and Ucode. Its mean
 * is the level that Ucode arrives at in that interval, on the receiver's
 * scale (UINFO at its own Table 1 level), snapped to a Table 1 level under
 * one gain where the codec's step there is 32 units or more (see v90dil.c: what the DIL's repetitive patterns leave of the
 * equaliser's residual interference biases the means by up to 0.8 %, which
 * random data does not see): a digital pad, a robbed bit
 * (mu-law: an odd Ucode arrives at its even neighbour's level, the codeword's
 * least significant bit forced to 1) and the codec's own law all show in it.
 * An interval that departs from the median of the six at a quarter of the
 * Ucodes 64..127 (by more than a quarter of the Ucode's step) is reported as
 * robbed (mu-law only); the deviation from Table 1 (scaled by UINFO) as the size of the
 * digital impairment.
 *
 * The constellations (v90dil_design_cp): for each interval, points picked
 * from the bottom up among the measured levels, each at least d above the
 * one before and the first at least d / 2 from zero (its two signs d
 * apart), where d = z sigma, sigma the receiver's noise in that interval.
 * For a given K the moduli Mi are the least that give prod Mi >= 2^K,
 * added where they cost least power; z is then raised as far as the power
 * limit allows (more margin for the same rate). The highest K whose largest
 * z reaches the required one wins. Sr = 0 (no spectral shaping), ld = 0,
 * one constellation per interval. The average power is that of 8.5.2
 * (Table 1 levels, each point weighted by how often the modulus encoder
 * picks it), computed rounding up, against Table 15 less 0.1 dB.
 *
 * Integer only, no allocation, no static state. */
#ifndef V90D_MODEM_V90DIL_H
#define V90D_MODEM_V90DIL_H
#include <stdint.h>
#include "../v90/v90_seq.h"

#define V90DIL_NU   101             /* Ucodes measured: 0 .. V90DIL_NU - 1 (a third of full scale) */
#define V90DIL_PASS (V90DIL_NU * 18) /* symbols in one pass of the DIL of v90dil_design */

/* the DIL descriptor we send in Ja */
void v90dil_design(v90_dil_t *d, int uinfo);

/* Walking the DIL as the digital modem sends it (8.4.1): segment, position */
typedef struct { int seg, pos; } v90dil_cur_t;
void v90dil_cur_init(v90dil_cur_t *c);
/* the next DIL symbol: its Ucode and sign (1 = positive); returns 1 when it
 * is the first of a DIL-segment */
int  v90dil_next(const v90_dil_t *d, v90dil_cur_t *c, int *ucode, int *pos);

/* ---- analysis ---- */
typedef struct {
    int32_t  sum[6][128];           /* sign-corrected equaliser output, Q YF (v90arx) */
    uint16_t cnt[6][128];
} v90dil_acc_t;

void v90dil_acc_init(v90dil_acc_t *a);
/* one DIL symbol: interval, Ucode and sign sent, the equaliser output y
 * (Table 1 units, V90ARX_YF fraction bits) */
void v90dil_acc_add(v90dil_acc_t *a, int slot, int ucode, int pos, int32_t y);
/* the smallest count over interval and the Ucodes the DIL has (0 .. V90DIL_NU - 1) */
int  v90dil_acc_min(const v90dil_acc_t *a);
/* the current mean of (slot, ucode), or 0 when none; Q YF */
int32_t v90dil_acc_mean(const v90dil_acc_t *a, int slot, int ucode);

typedef struct {
    int32_t lv[6][128];             /* measured level, Table 1 units Q YF; -1 = not measured */
    uint8_t robbed[6];              /* the interval loses the codeword's LSB */
    int     dev_q8;                 /* largest |level - Table 1 level| over Ucodes 64..127,
                                       relative, Q8 (0 for a clean channel) */
    int32_t gain_q16;               /* the levels are this times Table 1's */
    int     uinfo_as;               /* the codeword UINFO arrives as (a pad lowers it) */
} v90dil_res_t;

void v90dil_analyse(const v90dil_acc_t *a, int alaw, int uinfo, v90dil_res_t *r);

/* The same in steps of some tens of thousands of ARM instructions each (the
 * whole was 9 M in one tick at first, 1.1 M made faster): begin (it reads a
 * and clears r), then step until it returns 1. a may change after begin;
 * r must not change between steps. */
typedef struct {
    int     phase, alaw, uinfo, v, bv, s;
    int64_t best, tu, g, gs[6];
    int32_t t1[128];
} v90dil_an_t;
void v90dil_analyse_begin(v90dil_an_t *an, const v90dil_acc_t *a, int alaw, int uinfo, v90dil_res_t *r);
int  v90dil_analyse_step(v90dil_an_t *an, v90dil_res_t *r);

/* ---- constellations for CPt and CP ---- */
typedef struct {
    int      alaw;
    int      is_cp;                 /* 1 CP (data mode), 0 CPt (Phase 4) */
    uint32_t sigma2_q8[6];          /* noise per interval, Table 1 units^2 Q8 (v90arx_mse_slot) */
    int      z_q4;                  /* least d / sigma, Q4 */
    int      max_tx_code;           /* INFO0d bits 33:37: maximum transmit power, Table 15 row */
    uint32_t rates;                 /* CP: Jd's mask, bit r = (r + 21) * 8000 / 6 bit/s; 0 = all */
    int      max_bps;               /* our ceiling, bit/s; 0 = none */
    int      max_k;                 /* CPt: the highest K to consider (<= 24); 0 = 24 */
    uint16_t up_rates;              /* CP bits 36:48: what our transmitter can send */
} v90dil_req_t;

typedef struct {
    int      K, D, bps;             /* chosen; 0 when nothing fits */
    int      z_q4;                  /* d / sigma reached */
    uint32_t power;                 /* average power, Table 1 units^2 */
    uint8_t  M[6];
} v90dil_choice_t;

/* Fills cp (and ch) with the best constellation set; returns K, or 0 when
 * not even the lowest rate fits (cp is then a cleardown, drn = 0). */
int  v90dil_design_cp(const v90dil_res_t *res, const v90dil_req_t *req, v90_cp_t *cp, v90dil_choice_t *ch);

/* The same in steps, one trial constellation set each (some tens of
 * thousands of ARM instructions; a design takes 10 to 30 of them): begin
 * (it copies req, clears cp and ch), then step until it returns 1; then
 * j->k is what v90dil_design_cp() returns. res, cp and ch must not change
 * between steps. */
typedef struct {
    const v90dil_res_t *res;
    int alaw;
    int32_t t1[128];                /* Table 1 of the law */
    int32_t sig[6];                 /* noise, Table 1 units Q YF */
    uint64_t lim2;                  /* the power limit, units^2 */
    /* a design: points of each interval, ascending */
    uint8_t n[6], pts[6][128];
    uint8_t M[6];
    uint8_t C[6][128];
    uint32_t power;
} v90dil_dsn_t;
typedef struct {
    int phase, K, kmin, kmax, lo, hi, k;
    v90dil_req_t req;
    v90dil_dsn_t g;
} v90dil_cpjob_t;
void v90dil_design_begin(v90dil_cpjob_t *j, const v90dil_res_t *res, const v90dil_req_t *req, v90_cp_t *cp, v90dil_choice_t *ch);
int  v90dil_design_step(v90dil_cpjob_t *j, const v90dil_res_t *res, v90_cp_t *cp, v90dil_choice_t *ch);

/* 8.5.2's average power of a constellation set (K, Mi, Ucodes of label j
 * in C[i][j], law), Table 1 units squared, rounded up */
uint32_t v90dil_power(int alaw, int K, const uint8_t M[6], const uint8_t C[6][128]);
/* Table 15: the limit for INFO0d's maximum transmit power code (0 = -0.5 dBm0
 * .. 31 = -16 dBm0), Table 1 units (RMS) */
int32_t v90dil_limit(int max_tx_code);

#endif
