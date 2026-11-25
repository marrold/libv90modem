/* V.92 Phase 3 and Phase 4 sequences as bit arrays (8.5 to 8.8):
 *
 *   analogue modem sends:  Ja (DIL descriptor, Table 20), CPt and CPu
 *                          (Table 23), CPus (Table 24), SUVu (Table 27)
 *   digital modem sends:   Jd (Table 21), Jp (Table 22), Jp', SUVd
 *                          (Table 31), CPd (Table 30)
 *
 * Conventions, the same as ../v90/v90_seq.h:
 *  - a bit array holds one bit (0 or 1) per uint8_t; index 0 is "bit 0" of
 *    the Recommendation's tables and is the first bit in time;
 *  - multi-bit fields are LSB first (the tables' "LSB:MSB" column);
 *  - xxx_build() writes the sequence from bit 0 (frame sync) through its
 *    last fill bit and returns the number of bits written, or a negative
 *    V92_ERR_ code if the struct cannot be encoded;
 *  - xxx_parse() expects bits[0] to be bit 0 of the sequence (use
 *    v92_find_sync() to get there) and `n` bits to be available. It returns
 *    the number of bits the sequence occupies (> 0) or a negative V92_ERR_
 *    code. Reserved bits are ignored on parse and written as 0 on build;
 *  - the CRC is the one of ../v90/v90_seq.h (10.1.2.3.2/V.34): it covers
 *    every bit between the frame sync and the CRC except start bits.
 *
 * Fill. Jd and Jp have a fixed length. The others end with "Fill bits: 0s to
 * extend the sequence length to the next multiple of" 12 bits (Ja), 12
 * symbols (CPt, CPu, CPus, SUVu) or 6 symbols (CPd, SUVd). How many bits a
 * symbol carries depends on the modulation in use, so those builders take
 * `unit`, the number of bits in that many symbols (CPt: 12; CPu, CPus and
 * SUVu with 4-point TRN2u: 24, with 8-point TRN2u: 36; SUVd and CPd: the bits
 * per 6-symbol data frame of the TRN2d mapping), and fill to a multiple of
 * it; unit = 1 gives the sequence through its single mandatory fill bit.
 * The parsers take the same `unit` and consume the zero fill that is
 * actually there up to that boundary: the CRC decides, a short fill is
 * tolerated.
 *
 * None of this is tied to a modulation; see v92_sig.h for how the bits
 * travel.
 */
#ifndef V90D_V92_SEQ_H
#define V90D_V92_SEQ_H
#include "v92.h"
#include "../v90/v90_seq.h"

/* The sequence is not of the kind asked for (Jd / Jp, CP / SUV, CPt and CPu /
 * CPus). The kind decides the layout, so it is judged from its bits before
 * the CRC is checked. See v92_j_kind(), v92_up_kind(), v92_dn_kind(). */
#define V92_ERR_KIND   (-5)

/* Index of the first frame sync at or after `from`: at least seventeen 1s
 * followed by the start bit 0; the index returned is that of the 17th 1
 * before the 0 (bit 0 of the sequence). Unlike v90_find_sync() a longer run
 * of ones is accepted, because Ja and the first CPt follow 24 ones (8.5.1,
 * 8.5.4) and V.92 has no all-ones E sequence that could be mistaken.
 * Returns -1 if there is none in bits[from..n-1]. */
int v92_find_sync(const uint8_t *bits, int n, int from);

/* ---- Ja (8.5.4, Table 20): DIL descriptor + upstream rate mask ---------- */

#define V92_JA_PREAMBLE  24          /* binary ones before the first descriptor */
#define V92_JA_MAX_BITS  2688        /* 239 + 272 + 128 * 17, filled to 12 */

typedef struct {
    v90_dil_t dil;        /* bits 0 .. 187 + beta + ceil(N/2) * 17 of 8.3.1/V.90 */
    uint32_t  rates;      /* bit r set: upstream rate (r + 18) * 8000 / 6
                             supported, r = 0 (24000) .. 18 (48000) */
} v92_ja_t;

/* One DIL descriptor, zero filled to a multiple of 12 bits (276 for N = 0). */
int v92_ja_build(const v92_ja_t *ja, uint8_t *bits);
int v92_ja_parse(const uint8_t *bits, int n, v92_ja_t *ja);

/* ---- Jd (8.6.2, Table 21), Jp (8.6.3, Table 22), Jp' (8.6.4) ------------ */

#define V92_J_BITS    72             /* Jd and Jp */
#define V92_JPP_BITS  12             /* Jp': 12 binary zeros */

typedef struct {
    uint32_t rates;       /* bit r: downstream rate index r as in v90_jd_t
                             (28000 .. 56000), r = 0..21 */
    uint8_t  max_ld;      /* bits 49:50: maximum look-ahead, 1..3 */
} v92_jd_t;

typedef struct {
    uint16_t frac;        /* bits 18:33: fraction of a symbol, frac / 65536 T,
                             by which the Su-bar of the Jp to Jp' transition
                             is to be extended */
    uint8_t  pt8_train;   /* bit 48: CPu, E2u, SUVu, TRN2u use the 8-point
                             constellation during training (0: 4-point) */
    uint8_t  pt8_reneg;   /* bit 49: the same during rate renegotiation */
} v92_jp_t;

enum { V92_J_JD = 0, V92_J_JP = 1 };
/* Bit 47 of a Jd / Jp candidate (no CRC check); V92_ERR_SHORT if n < 48. */
int v92_j_kind(const uint8_t *bits, int n);

int v92_jd_build(const v92_jd_t *jd, uint8_t *bits);              /* 72 */
int v92_jd_parse(const uint8_t *bits, int n, v92_jd_t *jd);       /* V92_ERR_KIND for a Jp */
int v92_jp_build(const v92_jp_t *jp, uint8_t *bits);              /* 72 */
int v92_jp_parse(const uint8_t *bits, int n, v92_jp_t *jp);       /* V92_ERR_KIND for a Jd */

/* ---- the unframed sequences of zeros ------------------------------------- */

/* E1u (8.5.2): a data frame of zeros after the last CPt, one bit per symbol.
 * E2u (8.7.2): a data frame of zeros in the TRN2u modulation, 2 or 3 bits
 * per symbol, one symbol longer if bit 29 of CPd was set. Ed (8.8.2): two
 * data frames (12 symbols) of zeros in the TRN2d modulation. Jp': 12 zeros.
 * A receiver tells them from one more repetition of the sequence before
 * (which would start with 17 ones) by their first bits. */
#define V92_E1U_BITS            12
#define V92_E2U_BITS(nb, ext)   ((12 + ((ext) ? 1 : 0)) * (nb))

/* 1 if len bits are available and all zero, else 0. */
int v92_e_match(const uint8_t *bits, int n, int len);

/* ---- sequences of the analogue modem in Phase 4 and CPt ----------------- */

enum { V92_UP_CPT = 0, V92_UP_CPU = 1, V92_UP_CPUS = 2, V92_UP_SUVU = 4 };
/* Kind of an upstream sequence from bits 18 (0 = CP, 1 = SUVu) and 19:20
 * (CP type), no CRC check. V92_ERR_SHORT if n < 21, V92_ERR_RANGE for CP
 * type 3. */
int v92_up_kind(const uint8_t *bits, int n);

/* CPt and CPu (8.5.1, 8.7.3, Table 23): the constellations and spectral
 * shaping the digital modem is to transmit with. Same content as V.90's CP
 * (v90_cp_t) at slightly different bit positions. */
#define V92_CPU_MAX_BITS  (1786 + 71)    /* 290 + delta max, plus fill for unit <= 72 */

typedef struct {
    uint8_t  type;        /* bits 19:20: V92_UP_CPT or V92_UP_CPU */
    uint8_t  drn;         /* bits 21:25: 0 = cleardown, else downstream rate
                             (drn + 20) * 8000/6 (CPu) or (drn + 8) * 8000/6 (CPt) */
    uint8_t  sr;          /* bits 31:32: Sr */
    uint8_t  ack;         /* bit 33: 1 makes this CPu' */
    uint8_t  alaw;        /* bit 35: codec type */
    uint8_t  ld;          /* bits 49:50: look-ahead frames requested */
    uint16_t rms_ratio;   /* bits 52:67: unsigned Q3.13 */
    int8_t   a1, a2, b1, b2;   /* bits 69:101: shaping filter, signed Q1.6 */
    uint8_t  idx[6];      /* constellation index of data frame intervals 0..5 */
    uint8_t  codec_differs;    /* bit 128 */
    uint8_t  mask[6][16];      /* as v90_cp_t: bit u of mask[c] = Ucode u */
    uint8_t  codec_mask[6][16];
} v92_cpu_t;

int v92_cpu_nconst(const v92_cpu_t *cp);                 /* max(idx) + 1 */
int v92_cpu_build(const v92_cpu_t *cp, int unit, uint8_t *bits);
int v92_cpu_parse(const uint8_t *bits, int n, int unit, v92_cpu_t *cp);
/* The same parameters as a V.90 CP / CPt, for the code that already takes
 * one (v90_map_from_cp, v90tx_load_cp). silence = 0, rates = 0. */
void v92_cpu_to_v90(const v92_cpu_t *cp, v90_cp_t *out);

/* CPus (Table 24): short CPu, rate renegotiation and fast parameter
 * exchange only. */
typedef struct {
    uint8_t drn;          /* bits 21:25 */
    uint8_t ack;          /* bit 33 */
} v92_cpus_t;

int v92_cpus_build(const v92_cpus_t *c, int unit, uint8_t *bits);     /* 52 + fill */
int v92_cpus_parse(const uint8_t *bits, int n, int unit, v92_cpus_t *c);

/* SUVu (8.7.5, Table 27) */
#define V92_SUV_BITS  52             /* SUVu and SUVd before the fill to `unit` */
#define V92_SUVU_NO_LEVEL (-16)

typedef struct {
    uint8_t wait_cpu;     /* bit 26: please wait for a CPu before sending CPd */
    int8_t  level;        /* bits 27:31: 20 log10 of the measured RMS level of
                             G * v(n), signed Q2.2 (quarter dB, -15..15);
                             V92_SUVU_NO_LEVEL = no measurement */
    uint8_t silence;      /* bit 32: silent period requested */
    uint8_t ack;          /* bit 33: 1 makes this SUVu' */
} v92_suvu_t;

int v92_suvu_build(const v92_suvu_t *s, int unit, uint8_t *bits);
int v92_suvu_parse(const uint8_t *bits, int n, int unit, v92_suvu_t *s);

/* ---- sequences of the digital modem in Phase 4 -------------------------- */

enum { V92_DN_CPD = 0, V92_DN_SUVD = 1 };
/* Bit 18 of a downstream Phase 4 sequence (no CRC check). */
int v92_dn_kind(const uint8_t *bits, int n);

/* SUVd (8.8.5, Table 31) */
typedef struct {
    uint8_t silence;      /* bit 32 */
    uint8_t ack;          /* bit 33: 1 makes this SUVd' */
} v92_suvd_t;

int v92_suvd_build(const v92_suvd_t *s, int unit, uint8_t *bits);
int v92_suvd_parse(const uint8_t *bits, int n, int unit, v92_suvd_t *s);

/* CPd (8.8.3, Table 30): the upstream data mode parameters.
 *
 * Bits 0:50 are always there. The three optional parts are present when the
 * has_ flags (bits 19, 20, 21) say so; an absent part takes no bits at all
 * and the corresponding fields below are zero after a parse and ignored by
 * build:
 *   has_mod:   map.M[0..11]
 *   has_filt:  filt.lz1, lp1, lz2, lp2 and filt.coef
 *   has_const: map.cidx, map.lc, map.pt
 * Always carried: drn, map.trellis, e2u_extend, ack, filt.gain4. map.K is
 * not in the sequence: parse sets it to V92_UP_K(drn) (0 for a cleardown),
 * build ignores it.
 * After a parse with all three parts present, map and filt can be handed to
 * v92_map_check() / v92_dec_init() and v92_filt_check() / v92_enc_init(). */
#define V92_CPD_MAX_BITS  (324 + 17 * (V92_MAX_LTOT + V92_NCONST * V92_MAX_LC) + 71)

typedef struct {
    uint8_t  has_mod, has_filt, has_const;   /* bits 19, 20, 21 */
    uint8_t  drn;         /* bits 22:26: 0 = cleardown, 1..19: upstream rate
                             (drn + 17) * 8000 / 6 */
    uint8_t  e2u_extend;  /* bit 29: extend E2u by one symbol */
    uint8_t  ack;         /* bit 33: 1 makes this CPd' */
    v92_map_t  map;
    v92_filt_t filt;
} v92_cpd_t;

/* V92_ERR_RANGE on build: drn > 19, trellis > 2, gain4 = 0; with has_filt a
 * section longer than 320 or more than 384 taps in all; with has_const a
 * cidx above 5, an LC above 128, LC1 = 0, a non-empty set listed after an
 * empty one, or a zero point (8.8.3).
 * On parse V92_ERR_RANGE means the lengths announced do not fit these
 * limits (the lengths decide the layout, so they are checked before the
 * CRC); values are otherwise delivered as received. */
int v92_cpd_build(const v92_cpd_t *c, int unit, uint8_t *bits);
int v92_cpd_parse(const uint8_t *bits, int n, int unit, v92_cpd_t *c);

#endif
