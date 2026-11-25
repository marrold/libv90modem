/* V.90 / V.34 Phase 2 INFO sequences (V.90 8.2.3.2, Tables 7 to 11; CRC per
 * V.34 10.1.2.3.2). Frames are handled as arrays of bits (one per byte, 0/1),
 * element 0 first in time.
 *
 * Frame = fill 1111, sync 01110010, information bits, CRC-16, fill 1111.
 * The "payload" is everything between the sync and the trailing fill
 * (information bits followed by the 16 CRC bits). */
#ifndef V90D_MODEM_INFO_H
#define V90D_MODEM_INFO_H
#include <stdint.h>

#define INFO0D_BITS 62
#define INFO0A_BITS 49
#define INFO1D_BITS 109
#define INFO1A_BITS 70
#define INFO1C_BITS 109                     /* V.34 Table 15: the length of INFO1d */
#define INFO_HDR_BITS 12                    /* fill + sync */
#define INFO_PAYLOAD(total) ((total) - 16)  /* information + CRC */
#define INFO_MAX_BITS 109

/* CRC of n bits: x^16+x^12+x^5+1, register preset to ones, no inversion.
 * Bit 0 of the result is transmitted first. The register is v90_crc()'s
 * (v90/v90_seq.c), one bit at a time v90_crc_step(). */
uint16_t info_crc(const uint8_t *bits, int n);
/* 1 when a payload (information + CRC, npayload bits) checks out */
int info_check(const uint8_t *payload, int npayload);

/* INFO0: bits 12..28 are common to INFO0d and INFO0a; the rest is INFO0d only */
typedef struct {
    uint8_t sr2743, sr2800, sr3429;     /* V.34 symbol rates supported */
    uint8_t lo3000, hi3000, lo3200, hi3200;
    uint8_t allow3429;                  /* 0 = 3429 transmission disallowed */
    uint8_t pwr_reduce;                 /* can reduce transmit power */
    uint8_t max_sr_diff;                /* 0..5 */
    uint8_t cme, c1664;
    uint8_t ack;                        /* bit 28 */
    /* INFO0d only */
    uint8_t tx_power;                   /* 0..15: -6 .. -21 dBm0 */
    uint8_t max_tx_power;               /* 0..31: -0.5 .. -16 dBm0 */
    uint8_t pwr_at_codec, alaw, up3429;
    /* V.92 (8.4.1, Tables 15 and 16); zero for V.90 and V.34, where the two
     * bits are reserved. The two tables place them the other way round:
     * INFO0d has short Phase 2 in bit 26 and V.92 in bit 27, INFO0a has V.92
     * in bit 26 and short Phase 2 in bit 27 (9.3/V.92 confirms the bits). */
    uint8_t short_p2;                   /* short Phase 2 requested */
    uint8_t v92;                        /* V.92 capability */
} info0_t;

typedef struct { uint8_t hi_carrier, preemph, max_rate; } info_probe_t;

typedef struct {
    uint8_t min_pwr_red, add_pwr_red;   /* dB, 0..7 */
    uint8_t md_len;                     /* x 35 ms */
    info_probe_t sr[6];                 /* 2400 2743 2800 3000 3200 3429 */
    int16_t freq_off;                   /* 0.02 Hz units, -512 = ignore */
} info1d_t;

/* V.34 INFO1c (Table 15), what the call modem's receiver asks of the
 * answer modem's transmitter: the same fields in the same places as V.90's
 * INFO1d */
typedef info1d_t info1c_t;

typedef struct {
    int     v90;                        /* 1: Table 10, 0: Table 11 (V.34) */
    uint8_t md_len;
    uint8_t uinfo;                      /* V.90: Ucode for the 2-point train */
    uint8_t up_sr;                      /* analogue -> digital symbol rate index */
    uint8_t down_sr;                    /* V.34 only (V.90 carries the integer 6) */
    uint8_t min_pwr_red, add_pwr_red;   /* V.34 only */
    uint8_t hi_carrier, preemph, max_rate; /* V.34 only */
    int16_t freq_off;
    /* V.92 PCM upstream (Table 18/V.92): bits 34:36 and 37:39 both carry 6.
     * v90 is 1 too (PCM downstream), up_sr is 6, md_len counts 276 symbols
     * and freq_off is not carried (bits 40:49 are ones). */
    uint8_t pcm;
    uint8_t sections;                   /* bits 12:13: bit 0 = z1, bit 1 = p2 supported */
    uint8_t ltot_code;                  /* bits 14:15: Ltot = 192 + 64 * code */
    uint8_t lmax_code;                  /* bits 16:17: Lmax = 128 + 64 * code */
} info1a_t;

/* What INFO1a selects (V.92 9.3, V.90 9.2.1.1.8) */
enum { INFO1A_V34 = 0,                  /* bits 37:39 in 0..5: V.34 both ways */
       INFO1A_V90 = 1,                  /* 37:39 = 6: PCM downstream, V.34 upstream */
       INFO1A_PCM = 2 };                /* 34:36 = 6 as well: PCM both ways (V.92) */
int info1a_kind(const info1a_t *i);

/* build a complete frame; returns its length in bits */
int info0d_build(const info0_t *i, uint8_t *bits);
int info0a_build(const info0_t *i, uint8_t *bits);
int info1d_build(const info1d_t *i, uint8_t *bits);
/* INFO1d of Table 17/V.92, sent when both INFO0 sequences indicated V.92
 * (9.3/V.92): bit 70 says whether the channel supports PCM upstream, and
 * the 3429 entry has no carrier bit (sr[5].hi_carrier is not sent). A
 * receiver that parses it with info1d_parse() finds bit 70 in
 * sr[5].hi_carrier. */
int info1d_build_v92(const info1d_t *i, int pcm_up, uint8_t *bits);
int info1a_build(const info1a_t *i, uint8_t *bits);

/* parse a payload (the bits after the sync). CRC is not checked here. */
void info0d_parse(const uint8_t *payload, info0_t *i);
void info0a_parse(const uint8_t *payload, info0_t *i);
void info1d_parse(const uint8_t *payload, info1d_t *i);
void info1a_parse(const uint8_t *payload, info1a_t *i);

/* V.34 Table 15: INFO1c puts the same fields in the same bits as INFO1d */
static inline int info1c_build(const info1c_t *i, uint8_t *bits) { return info1d_build(i, bits); }
static inline void info1c_parse(const uint8_t *payload, info1c_t *i) { info1d_parse(payload, i); }

#endif
