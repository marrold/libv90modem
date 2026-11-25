/* x2 records: the server's record and the client's MP, both directions
 * (docs/history/x2-facts.md, "Records"). Bits are one per byte (0/1),
 * element 0 first in time, as in info.h.
 *
 * A record: 17 ones and a zero (the sync); each 16-bit word LSB first,
 * followed by a zero; then the CRC-16 of the word bits alone (not the
 * zeros), LSB first: info_crc()'s CRC-16 (register from FFFF, polynomial
 * 0x8408 reflected, no final inversion; bit 0 sent first). The caller pads
 * a record to its frames. */
#ifndef V90D_MODEM_X2REC_H
#define V90D_MODEM_X2REC_H
#include <stdint.h>

#define X2REC_SYNC_BITS 18
#define X2REC_BITS(nw) (X2REC_SYNC_BITS + 17 * (nw) + 16)
#define X2REC_SHORT 0       /* x2rec_parse: fewer bits than the record needs */
#define X2REC_BAD (-1)      /* x2rec_parse: sync, a separator or the CRC wrong */

/* The record of nw words w into bits (X2REC_BITS(nw) of them), word 0's
 * bit 15 (the acknowledge) set when ack, cleared otherwise. Returns the
 * number of bits. */
int x2rec_build(const uint16_t *w, int nw, int ack, uint8_t *bits);

/* A record of nw words from n bits that start with its sync: nw, the words
 * in w; X2REC_SHORT when n < X2REC_BITS(nw); X2REC_BAD when it does not
 * check out */
int x2rec_parse(const uint8_t *bits, int n, uint16_t *w, int nw);

/* The client's MP (4 words; "Records", the table of fields) */
typedef struct { uint16_t w0, w1, w2, w3; } x2_mp_t;

/* the highest downstream rate index accepted */
static inline int x2_mp_rate(uint16_t w0) { return (w0 >> 2) & 15; }
/* the digital pad measured: 0 none, 1 3 dB, 2 6 dB */
static inline int x2_mp_pad(uint16_t w0) { return (w0 >> 11) & 15; }
/* the acknowledge (MP') */
static inline int x2_mp_ack(uint16_t w0) { return (w0 >> 15) & 1; }
/* the number of downstream signs, MD */
static inline int x2_mp_md(uint16_t w3) { return (w3 >> 8) & 0xff; }

#endif
