/* V.92 analogue modem: the digital modem's Phase 4 sequences as they come
 * out of the TRN2d descrambler, one data frame of bits at a time: SUVd
 * (8.8.5, Table 31), CPd (8.8.3, Table 30) and Ed (8.8.2).
 *
 * CPd may be some 20000 bits long (384 coefficients, six sets of 128
 * points). Every field after its frame sync is a start bit and 16
 * information bits, so it is taken word by word into the v92_cpd_t, the
 * CRC (10.1.2.3.2/V.34) running along, and no bit is kept: what the
 * parsers of ../v92/v92_seq.h would need a 20 kbyte array for. The layout
 * follows v92_cpd_parse() field for field (the tests compare the two).
 *
 * A sequence ends with its CRC, a fill bit and zeros to the end of the data
 * frame. Ed is two data frames of zeros "outside any sequence": it is
 * reported for the first all-zero data frame that follows the end of a
 * sequence directly (the digital modem completes the current sequence and
 * sends Ed, 9.6.1.1.4); the frame after it is Ed's second.
 *
 * Integer only, no allocation. */
#ifndef V90D_MODEM_V92ASEQ_H
#define V90D_MODEM_V92ASEQ_H
#include <stdint.h>
#include "seqhunt.h"
#include "../v92/v92_seq.h"

/* words in the longest CPd: header, gain, M, lengths, 384 coefficients,
 * indices, sizes, 6 x 128 points, CRC */
#define V92AS_MAX_WORDS (2 + 6 + 4 + V92_MAX_LTOT + 2 + 3 + V92_NCONST * V92_MAX_LC + 1)

enum {
    V92AS_SUVD = 1,             /* an SUVd (or SUVd') was received */
    V92AS_CPD  = 2,             /* a CPd (or CPd') was received */
    V92AS_ED   = 4,             /* the first data frame of Ed */
    V92AS_BAD  = 8              /* a sequence began and did not check out */
};

typedef struct {
    seqhunt_t sh;               /* hunting: ones in a row */
    int      in;                /* 1: inside a sequence */
    int      nb;                /* bits of the current word: 0 = the start bit is next */
    uint32_t cur;               /* the word being collected */
    int      nw;                /* words completed, the header being word 0 */
    int      crc_at;            /* the word that is the CRC, as far as known */
    uint16_t crc, crc_want;
    uint8_t  kind;              /* V92_DN_CPD or V92_DN_SUVD */
    uint8_t  has;               /* V92AS_HAS_*: the parts of the stored CPd */
    int      i_lc, i_pts, ncoef, npts;  /* CPd: where its parts begin */
    int      fill;              /* the sequence has ended: the rest of the frame is fill */
    int      skip;              /* ... and so is the next frame (the CRC ended this one) */
    int      after;             /* 0: the frame that follows a sequence's fill, -1 otherwise */
    int      ev;                /* events of the frame being taken */
    uint16_t w0;                /* the header word */
    v92_cpd_t cpd;              /* the CPd being received */
    v92_cpd_t out;              /* the stored CPd with it merged, checked before it is stored */
} v92aseq_t;

enum {                          /* v92aseq_t.has */
    V92AS_HAS_MOD = 1, V92AS_HAS_FILT = 2, V92AS_HAS_CONST = 4
};

void v92aseq_init(v92aseq_t *s);
/* One data frame of n descrambled bits. Returns the V92AS_ events of the
 * frame; an SUVd received goes to *suvd (not touched otherwise).
 *
 * *cpd is the stored CPd, the same one at every call: a CPd received is
 * merged into it. Table 30's bits 19:21 say which of its three parts a CPd
 * carries; drn, trellis, e2u_extend, ack and gain4 are always there. Those
 * and each part carried replace what *cpd held; a part left out is kept
 * from an earlier CPd, or zero if none had it, and *cpd's has_ flags say
 * which parts it holds. A CPd that does not check out leaves *cpd as it
 * was (V92AS_BAD): one whose CRC fails, and one that names an upstream
 * rate but would leave *cpd with a map or filter that fails v92_map_check
 * (once it holds the moduli and the constellations) or v92_filt_check
 * (once it holds the filters). */
int  v92aseq_frame(v92aseq_t *s, const uint8_t *bits, int n, v92_suvd_t *suvd, v92_cpd_t *cpd);
/* The start of a Phase 4 (after Phase 3, 9.5.2): hunt afresh, nothing
 * stored. */
void v92aseq_phase4(v92aseq_t *s);
/* The start of a rate renegotiation's Phase 4 (9.8): hunt afresh; the
 * stored CPd is kept, a CPd there carrying only what changed. */
void v92aseq_restart(v92aseq_t *s);
/* 1 when the stored CPd has all three parts (moduli, filters,
 * constellations): what the upstream encoder needs. */
int  v92aseq_cpd_complete(const v92aseq_t *s);

#endif
