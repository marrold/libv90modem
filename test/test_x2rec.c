/* x2 records and their CRC (modem/x2rec; docs/history/x2-facts.md,
 * "Records"): the published records come out bit for bit, parse undoes
 * build, and any one flipped bit is caught. */
#include "test.h"
#include "x2rec.h"
#include "info.h"

/* the word bits alone, LSB first, as the CRC sees them */
static int word_bits(const uint16_t *w, int nw, uint8_t *bits)
{
    int i, k, n = 0;
    for (i = 0; i < nw; i++)
        for (k = 0; k < 16; k++) bits[n++] = (uint8_t)((w[i] >> k) & 1);
    return n;
}

static unsigned field(const uint8_t *bits, int at, int nbits)
{
    unsigned v = 0; int k;
    for (k = 0; k < nbits; k++) v |= (unsigned)bits[at + k] << k;
    return v;
}

/* the record of w laid out as the facts say, checked field by field;
 * returns its CRC field */
static unsigned check_layout(const uint16_t *w, int nw, const uint8_t *bits, int n)
{
    int i, ok = 1;
    CHECK_EQ_I(n, 18 + 17 * nw + 16);
    for (i = 0; i < 17; i++) ok &= bits[i] == 1;
    ok &= bits[17] == 0;
    for (i = 0; i < nw; i++) {
        ok &= field(bits, 18 + 17 * i, 16) == w[i];
        ok &= bits[18 + 17 * i + 16] == 0;
    }
    CHECK(ok);
    return field(bits, 18 + 17 * nw, 16);
}

static uint32_t rng = 0x2c0ffee1u;
static uint32_t xs32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

int main(void)
{
    static const uint16_t mp[4]   = { 0x0344, 0x03FE, 0x0000, 0x0500 };
    static const uint16_t mpa[4]  = { 0x8344, 0x03FE, 0x0000, 0x0500 };
    static const uint16_t srv[3]  = { 0xF33C, 0x7FFF, 0x0000 };
    static const uint16_t srvn[3] = { 0x733C, 0x7FFF, 0x0000 };
    uint8_t bits[X2REC_BITS(8)], wb[16 * 8];
    uint16_t w[8], got[8];
    int n, i, k, nw, bad;

    /* the CRC of the word bits: the facts' check values */
    CHECK_EQ_I(info_crc(wb, word_bits(mp, 4, wb)), 0x14AD);
    CHECK_EQ_I(info_crc(wb, word_bits(mpa, 4, wb)), 0x92B0);
    CHECK_EQ_I(info_crc(wb, word_bits(srv, 3, wb)), 0x73F3);
    CHECK_EQ_I(info_crc(wb, word_bits(srvn, 3, wb)), 0xF9A6);
    CHECK_EQ_I(info_crc(wb, 0), 0xFFFF);

    /* the records: ack sets or clears word 0's bit 15 */
    n = x2rec_build(srvn, 3, 1, bits);
    CHECK_EQ_I(check_layout(srv, 3, bits, n), 0x73F3);
    n = x2rec_build(srv, 3, 0, bits);
    CHECK_EQ_I(check_layout(srvn, 3, bits, n), 0xF9A6);
    n = x2rec_build(mp, 4, 0, bits);
    CHECK_EQ_I(check_layout(mp, 4, bits, n), 0x14AD);
    n = x2rec_build(mp, 4, 1, bits);
    CHECK_EQ_I(check_layout(mpa, 4, bits, n), 0x92B0);

    /* parse(build(w)) == w; every single flipped bit is X2REC_BAD; fewer
       bits than the record is X2REC_SHORT */
    for (k = 0, bad = 0; k < 2000; k++) {
        nw = 1 + (int)(xs32() % 8);
        for (i = 0; i < nw; i++) w[i] = (uint16_t)xs32();
        n = x2rec_build(w, nw, (int)(w[0] >> 15), bits);
        if (x2rec_parse(bits, n, got, nw) != nw) bad++;
        for (i = 0; i < nw; i++) if (got[i] != w[i]) bad++;
        if (x2rec_parse(bits, n - 1, got, nw) != X2REC_SHORT) bad++;
        for (i = 0; i < n; i++) {
            bits[i] ^= 1;
            if (x2rec_parse(bits, n, got, nw) != X2REC_BAD) bad++;
            bits[i] ^= 1;
        }
    }
    CHECK_EQ_I(bad, 0);
    n = x2rec_build(mp, 4, 0, bits);
    CHECK_EQ_I(x2rec_parse(bits, n + 7, got, 4), 4);    /* bits after it: not read */
    CHECK(got[0] == 0x0344 && got[1] == 0x03FE && got[2] == 0 && got[3] == 0x0500);

    /* the MP's fields, the Courier's MP and its pad and acknowledge forms */
    CHECK_EQ_I(x2_mp_rate(0x0344), 1);
    CHECK_EQ_I(x2_mp_pad(0x0344), 0);
    CHECK_EQ_I(x2_mp_ack(0x0344), 0);
    CHECK_EQ_I(x2_mp_md(0x0500), 5);
    CHECK_EQ_I(x2_mp_pad(0x0B44), 1);
    CHECK_EQ_I(x2_mp_pad(0x1344), 2);
    CHECK_EQ_I(x2_mp_ack(0x8344), 1);
    CHECK_EQ_I(x2_mp_rate(0x8344), 1);
    CHECK_EQ_I(x2_mp_rate(0x0338), 14);

    T_REPORT("test_x2rec");
}
