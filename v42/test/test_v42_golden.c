/* The V.42 layer's identity proof: hashes (FNV-1a, 32 bits) of what it
 * produces from fixed inputs, one line per case, kept in the repository's
 * golden copy of this program's output (tools/golden.sh). A refactoring or
 * an optimisation of V.42 bis, V.44 or LAPM must leave every line as it is.
 *
 *   - V.42 bis encoder output, flushed at the end, for four inputs of 8192
 *     octets (an LCG, seed 1; an English sentence repeated; zeros; the LCG
 *     cut to 16 letters, "sym16") with P1 512 and 2048 and P2 6, 32 and
 *     250. The LCG's octets do not compress: the encoder goes to
 *     transparent mode (7.8, V.44 7.11.5), and its lines, the same for
 *     every P1 and P2, pin only that mode. sym16 stays compressed (to
 *     about two thirds) with a dictionary that fills and is recycled: the
 *     mixed-data case (its strings are short, so P2 does not change it;
 *     the text and the zeros cover P2);
 *   - V.44 encoder output for the same inputs, with the defaults (1024
 *     codewords, strings of 255, a history of 3072) and with 2048, 255 and
 *     4096;
 *   - a back-to-back pair of v42ec_t (default configuration, V.42 bis: V.44
 *     not offered), 56 line bits per millisecond for 2000 ms, both ends
 *     writing the sentence: the line bits of each direction.
 *
 * It also checks that each decoder gives back the input. */
#include <string.h>
#include "test.h"
#include "v42.h"

#define N 8192

static const char sentence[] = "the quick brown fox jumps over the lazy dog. ";

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
#define FNV0 2166136261u

#define NIN 4
static uint8_t in[NIN][N];
static const char *const in_name[NIN] = { "lcg", "text", "zeros", "sym16" };
static uint8_t out[3 * N + 256], back[N + 256];

static void inputs(void)
{
    uint32_t s = 1;
    size_t i, k = strlen(sentence);
    for (i = 0; i < N; i++) {
        s = s * 1103515245u + 12345u;
        in[0][i] = (uint8_t)(s >> 16);
        in[1][i] = (uint8_t)sentence[i % k];
        in[2][i] = 0;
    }
    for (i = 0, s = 1; i < N; i++) {
        s = s * 1103515245u + 12345u;
        in[3][i] = (uint8_t)('a' + (s >> 28));
    }
}

static v42comp_enc_t benc;
static v42comp_dec_t bdec;

static void bis_case(int x, int p1, int p2)
{
    size_t n = 0, used, got = 0, c;
    int rc = 0;
    CHECK_EQ_I(v42comp_enc_init(&benc, p1, p2), 0);
    c = v42comp_encode(&benc, in[x], N, out, sizeof out, &n);
    CHECK_EQ_I(c, N);
    CHECK_EQ_I(v42comp_flush(&benc, out, sizeof out, &n), 1);
    printf("v42bis %-5s P1 %4d P2 %3d: %5lu octets, hash %08lx\n", in_name[x], p1, p2,
           (unsigned long)n, (unsigned long)fnv(FNV0, out, n));
    CHECK_EQ_I(v42comp_dec_init(&bdec, p1, p2), 0);
    for (c = 0; rc == 0 && (c < n || v42comp_dec_pending(&bdec)) && got < N; c += used) {
        size_t before = got;
        rc = v42comp_decode(&bdec, out + c, n - c, &used, back, sizeof back, &got);
        if (used == 0 && got == before) break;
    }
    CHECK_EQ_I(rc, 0);
    CHECK_EQ_I(got, N);
    CHECK(got == N && memcmp(back, in[x], N) == 0);
}

static v44comp_enc_t venc;
static v44comp_dec_t vdec;

static void v44_case(int x, int p1, int p2, int p3)
{
    size_t n = 0, used, got = 0, c;
    int rc = 0;
    CHECK_EQ_I(v44comp_enc_init(&venc, p1, p2, p3), 0);
    c = v44comp_encode(&venc, in[x], N, out, sizeof out, &n);
    CHECK_EQ_I(c, N);
    CHECK_EQ_I(v44comp_flush(&venc, out, sizeof out, &n), 1);
    printf("v44    %-5s P1 %4d P2 %3d P3 %4d: %5lu octets, hash %08lx\n", in_name[x], p1, p2, p3,
           (unsigned long)n, (unsigned long)fnv(FNV0, out, n));
    CHECK_EQ_I(v44comp_dec_init(&vdec, p1, p2, p3), 0);
    for (c = 0; rc == 0 && (c < n || v44comp_dec_pending(&vdec)) && got < N; c += used) {
        size_t before = got;
        rc = v44comp_decode(&vdec, out + c, n - c, &used, back, sizeof back, &got);
        if (used == 0 && got == before) break;
    }
    CHECK_EQ_I(rc, 0);
    CHECK_EQ_I(got, N);
    CHECK(got == N && memcmp(back, in[x], N) == 0);
}

/* ---- the back-to-back pair: A originates, B answers ---- */
#define BITS_PER_MS 56
#define MS 2000

static v42ec_t A, B;
typedef struct { size_t wpos, rpos; unsigned long bad; uint32_t line; } dir_t;

/* write what the end takes of the endless sentence; read and compare */
static void pump(v42ec_t *tx, dir_t *d, v42ec_t *rx)
{
    size_t k = strlen(sentence), n, i;
    uint8_t buf[256];
    for (;;) {
        size_t off = d->wpos % k, w = v42ec_write(tx, (const uint8_t *)sentence + off, k - off);
        d->wpos += w;
        if (w < k - off) break;
    }
    while ((n = v42ec_read(rx, buf, sizeof buf)) > 0)
        for (i = 0; i < n; i++, d->rpos++)
            if (buf[i] != (uint8_t)sentence[d->rpos % k]) d->bad++;
}

static void pair(void)
{
    v42ec_cfg_t ca, cb;
    v42ec_status_t sa, sb;
    dir_t ab, ba;
    int ms, i;
    memset(&ab, 0, sizeof ab); memset(&ba, 0, sizeof ba);
    ab.line = ba.line = FNV0;
    v42ec_cfg_default(&ca, 1); v42ec_cfg_default(&cb, 0);
    ca.lapm.v44_tx = ca.lapm.v44_rx = cb.lapm.v44_tx = cb.lapm.v44_rx = 0;
    v42ec_init(&A, &ca); v42ec_init(&B, &cb);
    for (ms = 0; ms < MS; ms++) {
        for (i = 0; i < BITS_PER_MS; i++) {
            int a = v42ec_tx_bit(&A), b = v42ec_tx_bit(&B);
            ab.line = (ab.line ^ (uint32_t)(a & 1)) * 16777619u;
            ba.line = (ba.line ^ (uint32_t)(b & 1)) * 16777619u;
            v42ec_rx_bit(&B, a);
            v42ec_rx_bit(&A, b);
        }
        pump(&A, &ab, &B); pump(&B, &ba, &A);
        v42ec_tick(&A, 1); v42ec_tick(&B, 1);
    }
    v42ec_get_status(&A, &sa); v42ec_get_status(&B, &sb);
    printf("v42ec  A to B: %lu octets written, %lu read, line hash %08lx\n",
           (unsigned long)ab.wpos, (unsigned long)ab.rpos, (unsigned long)ab.line);
    printf("v42ec  B to A: %lu octets written, %lu read, line hash %08lx\n",
           (unsigned long)ba.wpos, (unsigned long)ba.rpos, (unsigned long)ba.line);
    CHECK_EQ_I(v42ec_state(&A), V42EC_CONNECTED);
    CHECK_EQ_I(v42ec_state(&B), V42EC_CONNECTED);
    CHECK_EQ_I(sa.comp_alg, V42EC_COMP_V42BIS);
    CHECK_EQ_I(sb.comp_alg, V42EC_COMP_V42BIS);
    CHECK(sa.comp_tx && sa.comp_rx && sb.comp_tx && sb.comp_rx);
    CHECK(ab.rpos > 0 && ba.rpos > 0);
    CHECK_EQ_I(ab.bad, 0);
    CHECK_EQ_I(ba.bad, 0);
}

int main(void)
{
    static const int p1[2] = { 512, 2048 }, p2[3] = { 6, 32, 250 };
    int x, i, j;
    inputs();
    for (x = 0; x < NIN; x++)
        for (i = 0; i < 2; i++)
            for (j = 0; j < 3; j++) bis_case(x, p1[i], p2[j]);
    for (x = 0; x < NIN; x++) {
        v44_case(x, 1024, 255, 3072);
        v44_case(x, 2048, 255, 4096);
    }
    pair();
    T_REPORT("test_v42_golden");
}
