/* The output writer of the V.42 bis and V.44 encoders: codes packed least
 * significant bit first into octets (V.42 bis 7.5, V.44 6.6), the octets
 * into the caller's buffer, and what does not fit there into a small
 * overflow buffer (ovf) that the next call empties first.
 *
 * The encoders run one step (a character, or the end of a string) only
 * while ovf is empty and the caller's buffer has room, so ovf never holds
 * more than one step's output. COMP_OUT_FITS checks at compile time that
 * the most one step can put out fits ovf.
 *
 * COMP_OUT_WRITER(T, OVF) defines the writer, static, for an encoder type T
 * with these members (both encoders' structs have them, as they always had):
 *     uint32_t bitbuf; int nbits;              code bits not yet an octet
 *     uint8_t ovf[OVF]; int ovf_len, ovf_pos;  overflow buffer
 *     uint8_t *out; size_t out_max, out_len;   the caller's buffer, in a call
 *     unsigned long out_octets;                octets produced
 * It defines:
 *     out_begin(e, out, outmax, outlen)  a call starts: ovf into out first
 *     out_end(e)                         a call ends: returns out's length
 *     out_pending(e)                     octets produced, not yet handed over
 *     out_octet(e, o)                    one octet
 *     out_bits(e, v, n)                  n bits of v, least significant first
 *     out_pad(e)                         0 bits up to the octet boundary
 * What the encoders count (win_bits) and whether bits go out at all (V.44
 * transparent mode) stays with them. */
#ifndef V42_COMP_OUT_H
#define V42_COMP_OUT_H

#include <stddef.h>
#include <stdint.h>

/* The bits that hold codeword n - 1 (n up to 65536), at least 6: N1 for a
 * dictionary of n codewords (V.42 bis 7.1, V.44 Table 11) */
#define COMP_OUT_BITS(n) ((n) <= 64 ? 6 : (n) <= 128 ? 7 : (n) <= 256 ? 8 : (n) <= 512 ? 9 \
                        : (n) <= 1024 ? 10 : (n) <= 2048 ? 11 : (n) <= 4096 ? 12 \
                        : (n) <= 8192 ? 13 : (n) <= 16384 ? 14 : (n) <= 32768 ? 15 : 16)

/* compile-time: one step's output, at most step_octets, fits ovf */
#define COMP_OUT_FITS(name, step_octets, ovf_size) \
    typedef char name[(step_octets) <= (ovf_size) ? 1 : -1]

#define COMP_OUT_WRITER(T, OVF)                                                 \
static void out_begin(T *e, uint8_t *out, size_t outmax, size_t outlen)         \
{                                                                               \
    e->out = out; e->out_max = outmax; e->out_len = outlen;                     \
    while (e->ovf_pos < e->ovf_len && e->out_len < e->out_max)                  \
        e->out[e->out_len++] = e->ovf[e->ovf_pos++];                            \
    if (e->ovf_pos == e->ovf_len) { e->ovf_pos = 0; e->ovf_len = 0; }           \
}                                                                               \
                                                                                \
static size_t out_end(T *e)                                                     \
{                                                                               \
    e->out = NULL;                                                              \
    return e->out_len;                                                          \
}                                                                               \
                                                                                \
static size_t out_pending(const T *e)                                           \
{                                                                               \
    return (size_t)(e->ovf_len - e->ovf_pos);                                   \
}                                                                               \
                                                                                \
static void out_octet(T *e, unsigned o)                                         \
{                                                                               \
    e->out_octets++;                                                            \
    if (e->ovf_len == e->ovf_pos && e->out && e->out_len < e->out_max)          \
        e->out[e->out_len++] = (uint8_t)o;                                      \
    else if (e->ovf_len < (OVF))                                                \
        e->ovf[e->ovf_len++] = (uint8_t)o;                                      \
}                                                                               \
                                                                                \
static void out_bits(T *e, uint32_t v, int n)                                   \
{                                                                               \
    e->bitbuf |= v << e->nbits;                                                 \
    e->nbits += n;                                                              \
    while (e->nbits >= 8) {                                                     \
        out_octet(e, e->bitbuf & 0xFF);                                         \
        e->bitbuf >>= 8;                                                        \
        e->nbits -= 8;                                                          \
    }                                                                           \
}                                                                               \
                                                                                \
static void out_pad(T *e)                                                       \
{                                                                               \
    if (e->nbits > 0) {                                                         \
        out_octet(e, e->bitbuf & 0xFF);                                         \
        e->bitbuf = 0; e->nbits = 0;                                            \
    }                                                                           \
}

#endif
