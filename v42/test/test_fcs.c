/* v42_fcs16 and v42_fcs32 against the bit-at-a-time definition (8.1.1.6,
 * run least significant bit first as v42_hdlc.c does): every octet from
 * 10^5 random register states (10^4 on 32-bit ARM, TEST_SHORT), random
 * blocks of 0 to 300 octets in one call and in pieces, and the residue a
 * frame plus its transmitted FCS leaves. Prints a hash of every result. */
#include <stdint.h>
#include "v42_hdlc.h"
#include "test.h"

#ifdef TEST_SHORT
#define NSTATES 10000L
#else
#define NSTATES 100000L
#endif

static uint32_t seed = 0x2545F491u;
static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed;
}

static uint16_t ref16(uint16_t fcs, const uint8_t *d, size_t n)
{
    size_t i; int b;
    for (i = 0; i < n; i++)
        for (b = 0; b < 8; b++) {
            unsigned x = (fcs ^ (unsigned)(d[i] >> b)) & 1u;
            fcs = (uint16_t)(x ? (fcs >> 1) ^ 0x8408u : fcs >> 1);
        }
    return fcs;
}

static uint32_t ref32(uint32_t fcs, const uint8_t *d, size_t n)
{
    size_t i; int b;
    for (i = 0; i < n; i++)
        for (b = 0; b < 8; b++) {
            uint32_t x = (fcs ^ (uint32_t)(d[i] >> b)) & 1u;
            fcs = x ? (fcs >> 1) ^ 0xEDB88320u : fcs >> 1;
        }
    return fcs;
}

static uint32_t hash = 2166136261u;
static void mix(uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) { hash = (hash ^ (v & 0xFFu)) * 16777619u; v >>= 8; }
}

/* every octet from NSTATES random states */
static void test_octets(void)
{
    long k, bad16 = 0, bad32 = 0;
    for (k = 0; k < NSTATES; k++) {
        uint16_t s16 = (uint16_t)(rnd() >> 16);
        uint32_t s32 = rnd();
        unsigned o;
        uint32_t x16 = 0, x32 = 0;
        for (o = 0; o < 256; o++) {
            uint8_t c = (uint8_t)o;
            uint16_t g16 = v42_fcs16(s16, &c, 1);
            uint32_t g32 = v42_fcs32(s32, &c, 1);
            if (g16 != ref16(s16, &c, 1)) bad16++;
            if (g32 != ref32(s32, &c, 1)) bad32++;
            x16 = x16 * 31u + g16; x32 = x32 * 31u + g32;
        }
        mix(x16); mix(x32);
    }
    printf("octets: %ld states x 256, fcs16 differ %ld, fcs32 differ %ld\n", NSTATES, bad16, bad32);
    CHECK_EQ_I(bad16, 0);
    CHECK_EQ_I(bad32, 0);
}

/* random blocks, whole and in two pieces, from random states */
static void test_blocks(void)
{
    uint8_t d[300];
    int k, i, bad = 0;
    for (k = 0; k < 2000; k++) {
        size_t n = rnd() % 301u, cut = n ? rnd() % (n + 1) : 0;
        uint16_t s16 = (uint16_t)(rnd() >> 16);
        uint32_t s32 = rnd();
        uint16_t w16; uint32_t w32;
        for (i = 0; i < (int)n; i++) d[i] = (uint8_t)(rnd() >> 24);
        w16 = ref16(s16, d, n); w32 = ref32(s32, d, n);
        if (v42_fcs16(s16, d, n) != w16 || v42_fcs32(s32, d, n) != w32) bad++;
        if (v42_fcs16(v42_fcs16(s16, d, cut), d + cut, n - cut) != w16) bad++;
        if (v42_fcs32(v42_fcs32(s32, d, cut), d + cut, n - cut) != w32) bad++;
        mix(w16); mix(w32);
    }
    printf("blocks: 2000 of 0 to 300 octets, whole and in two pieces, differ %d\n", bad);
    CHECK_EQ_I(bad, 0);
}

/* a frame plus its transmitted FCS (ones complement, low octet first)
 * leaves the residue */
static void test_residue(void)
{
    uint8_t f[V42HDLC_MAX_FRAME];
    int k, i, bad = 0;
    for (k = 0; k < 2000; k++) {
        int n = 1 + (int)(rnd() % V42HDLC_MAX_PAYLOAD);
        uint16_t c16; uint32_t c32;
        for (i = 0; i < n; i++) f[i] = (uint8_t)(rnd() >> 24);
        c16 = (uint16_t)~v42_fcs16(V42_FCS16_INIT, f, (size_t)n);
        f[n] = (uint8_t)c16; f[n + 1] = (uint8_t)(c16 >> 8);
        if (v42_fcs16(V42_FCS16_INIT, f, (size_t)n + 2) != V42_FCS16_GOOD) bad++;
        c32 = ~v42_fcs32(V42_FCS32_INIT, f, (size_t)n);
        f[n] = (uint8_t)c32; f[n + 1] = (uint8_t)(c32 >> 8);
        f[n + 2] = (uint8_t)(c32 >> 16); f[n + 3] = (uint8_t)(c32 >> 24);
        if (v42_fcs32(V42_FCS32_INIT, f, (size_t)n + 4) != V42_FCS32_GOOD) bad++;
        mix(c16); mix(c32);
    }
    printf("residue: 2000 frames of 1 to %d octets, FCS-16 and FCS-32, wrong %d\n", V42HDLC_MAX_PAYLOAD, bad);
    CHECK_EQ_I(bad, 0);
}

int main(void)
{
    test_octets();
    test_blocks();
    test_residue();
    printf("hash %08lx\n", (unsigned long)hash);
    T_REPORT("test_fcs");
}
