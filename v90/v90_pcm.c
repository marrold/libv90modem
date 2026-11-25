/* V.90 Table 1 in closed form. See v90_pcm.h.
 *
 * The table is regular: with chord c = Ucode >> 4 and step s = Ucode & 15,
 *   mu-law octet   = 0xFF - Ucode
 *   mu-law linear  = ((2s + 33) << (c + 2)) - 132
 *   A-law octet    = (0x80 | Ucode) ^ 0x55      (even bit inversion applied)
 *   A-law linear   = (2s + 1) << 3              for chord 0
 *                    (2s + 33) << (c + 2)       for chords 1..7
 * test/test_pcm.c checks all 128 rows against the printed table. An octet's
 * linear value is read from v90_pcm_tab.c, this form tabulated. */
#include "v90_pcm.h"

uint8_t v90_pcm_octet(int alaw, unsigned ucode, int positive)
{
    uint8_t o;
    ucode &= 0x7F;
    o = alaw ? (uint8_t)((0x80u | ucode) ^ 0x55u) : (uint8_t)(0xFFu - ucode);
    return positive ? o : (uint8_t)(o & 0x7F);
}

int32_t v90_ucode_linear(int alaw, unsigned ucode)
{
    unsigned c, s;
    ucode &= 0x7F;
    c = ucode >> 4;
    s = ucode & 15;
    if (alaw)
        return c == 0 ? (int32_t)((2 * s + 1) << 3)
                      : (int32_t)((2 * s + 33) << (c + 2));
    return (int32_t)((2 * s + 33) << (c + 2)) - 132;
}

int32_t v90_pcm_linear(int alaw, unsigned ucode, int positive)
{
    int32_t v = v90_ucode_linear(alaw, ucode);
    return positive ? v : -v;
}

unsigned v90_octet_ucode(int alaw, uint8_t octet)
{
    if (alaw) return (unsigned)((octet ^ 0x55u) & 0x7Fu);
    return (unsigned)(0x7Fu - (octet & 0x7Fu));
}

int v90_octet_sign(uint8_t octet)
{
    return (octet >> 7) & 1;
}
