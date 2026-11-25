/* V.21 300-baud FSK, used by the V.8 handshake.
 *   V.21(L) low channel:  binary 1 (mark) 980 Hz, binary 0 (space) 1180 Hz.
 *   V.21(H) high channel: binary 1 (mark) 1650 Hz, binary 0 (space) 1850 Hz.
 * Continuous-phase FSK. Fixed-point signal path (Q15 sine table, 32-bit phase).
 * The answer side transmits JM on V.21(H) and receives CM/CJ on V.21(L).
 */
#ifndef V90D_MODEM_V21_H
#define V90D_MODEM_V21_H
#include <stdint.h>

#define V21L_MARK  980
#define V21L_SPACE 1180
#define V21H_MARK  1650
#define V21H_SPACE 1850
/* V.23: Mode 2 (1200/75 bps asymmetric FSK, ITU-T V.23).
 * The answering modem transmits on the forward channel and receives on the
 * backward channel; the calling modem is the reverse.
 * Forward channel (1200 bit/s): mark 1300 Hz, space 2100 Hz.
 * Backward channel (75 bit/s):  mark  390 Hz, space  450 Hz. */
#define V23H_MARK   1300
#define V23H_SPACE  2100
#define V23L_MARK    390
#define V23L_SPACE   450

/* Bell 103, the same modulation on other tones: the calling modem sends
 * mark 1270 Hz / space 1070 Hz, the answering one mark 2225 Hz / space 2025 Hz. */
#define BELL103_O_MARK  1270
#define BELL103_O_SPACE 1070
#define BELL103_A_MARK  2225
#define BELL103_A_SPACE 2025

typedef struct {
    uint32_t phase;      /* 32-bit phase accumulator */
    uint32_t dmark, dspace;
    uint32_t baud_acc;   /* fractional baud timing */
    int32_t  fs, baud;
    int16_t  amp;
} v21mod_t;

void v21mod_init(v21mod_t *m, int f_mark, int f_space, int fs, int baud, int16_t amp);

/* Modulate nbits (1 = mark, 0 = space) into out[]; return samples written
 * (~ nbits*fs/baud). Continues phase across calls. */
int v21mod_run(v21mod_t *m, const uint8_t *bits, int nbits, int16_t *out, int maxout);

/* Streaming form: produce one sample while sending `bit`; *bit_done is set to 1
 * when this sample completed the bit period (caller advances to its next bit). */
int16_t v21mod_tick(v21mod_t *m, int bit, int *bit_done);

/* ---- Noncoherent FSK demodulator ---- */
/* One bit of the slowest channel we demodulate: V.23's 75-baud backward
 * channel is 8000/75 = 106 samples. (V.21/Bell at 300 baud use 26; V.23's
 * 1200-baud forward channel 6.) The full-bit window also sharpens the
 * 390/450 Hz discrimination of that narrow backward channel. */
#define V21_MAXWIN 106

typedef struct {
    uint32_t pmark, pspace;   /* local oscillator phases */
    uint32_t dmark, dspace;
    int32_t  win;             /* correlation window length (samples/bit) */
    int32_t  mi[V21_MAXWIN], mq[V21_MAXWIN];   /* mark I/Q product history */
    int32_t  si[V21_MAXWIN], sq[V21_MAXWIN];   /* space I/Q product history */
    int32_t  smi, smq, ssi, ssq;               /* running sums */
    int32_t  wpos;
    uint32_t baud_acc;
    int32_t  fs, baud;
    int64_t  energy;          /* mark+space correlator power of the last tick */
} v21demod_t;

void v21demod_init(v21demod_t *d, int f_mark, int f_space, int fs, int baud);

/* Demodulate nsamp input samples; write recovered bits; return bit count.
 * One bit is emitted each baud period (mark power > space power => 1). */
int v21demod_run(v21demod_t *d, const int16_t *in, int nsamp, uint8_t *bits, int maxbits);

/* Streaming form: consume one sample, return the instantaneous line level
 * (1 = mark, 0 = space). d->energy holds the in-band power, for squelch.
 * Feed the level stream to an async receiver (modem/async.h), which does the
 * bit timing from start-bit edges. */
int v21demod_tick(v21demod_t *d, int16_t x);

/* d->energy produced by a tone of peak amplitude `amp` on mark or space. */
int64_t v21demod_energy_of(const v21demod_t *d, int amp);

#endif
