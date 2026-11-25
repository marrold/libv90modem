/* x2 records and their CRC. See x2rec.h.
 *
 * The format (docs/history/x2-facts.md, "Records"): the I-modem's
 * serializer (Ie030002 DSP b54c-b589) and the published records of D-Modem
 * (https://github.com/D-Modem/D-Modem, docs/dsp/x2-details.md) and
 * courier-emu (https://github.com/cryan209/courier-emu). Facts only; no code
 * of either. The CRC is the register of V.34's INFO frames (info_crc), a
 * bit at a time: v90_crc_step(). */
#include "x2rec.h"
#include "../v90/v90_seq.h"

int x2rec_build(const uint16_t *w, int nw, int ack, uint8_t *bits)
{
    uint16_t c = 0xFFFF;
    int i, k, n = 0;
    for (k = 0; k < 17; k++) bits[n++] = 1;
    bits[n++] = 0;
    for (i = 0; i < nw; i++) {
        uint16_t v = w[i];
        if (i == 0) v = ack ? (uint16_t)(v | 0x8000) : (uint16_t)(v & 0x7FFF);
        for (k = 0; k < 16; k++) {
            int b = (v >> k) & 1;
            bits[n++] = (uint8_t)b;
            c = v90_crc_step(c, b);
        }
        bits[n++] = 0;
    }
    for (k = 0; k < 16; k++) bits[n++] = (uint8_t)((c >> k) & 1);
    return n;
}

int x2rec_parse(const uint8_t *bits, int n, uint16_t *w, int nw)
{
    uint16_t c = 0xFFFF;
    int i, k, at = 0;
    if (n < X2REC_BITS(nw)) return X2REC_SHORT;
    for (k = 0; k < 17; k++) if (bits[at++] != 1) return X2REC_BAD;
    if (bits[at++] != 0) return X2REC_BAD;
    for (i = 0; i < nw; i++) {
        uint16_t v = 0;
        for (k = 0; k < 16; k++) {
            int b = bits[at++] & 1;
            v = (uint16_t)(v | (unsigned)b << k);
            c = v90_crc_step(c, b);
        }
        if (bits[at++] != 0) return X2REC_BAD;
        w[i] = v;
    }
    for (k = 0; k < 16; k++)
        if ((bits[at++] & 1) != ((c >> k) & 1)) return X2REC_BAD;
    return nw;
}
