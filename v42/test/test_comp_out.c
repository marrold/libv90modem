/* The compression encoders' output writer (comp_out.h): what V.42 bis and
 * V.44 put out over a fixed corpus, one line per case, kept in the
 * repository's golden copy of this program's output (tools/golden.sh).
 * The lines were recorded with the writers each encoder had of its own
 * before comp_out.h; a change to the writer must leave every line as it is.
 *
 * Each case encodes one input of N octets (an LCG; an English sentence
 * repeated; zeros; the LCG cut to 16 letters; blocks of the LCG and the
 * sentence in turn, which take the AUTO policy back and forth between the
 * modes; the octets 0, 51, 102, ... that meet every escape character; all
 * 256 values in turn) with one set of parameters and one policy, twice:
 *   - in one call, into one buffer, flushed at the end ("whole");
 *   - in pieces of 1 to 97 octets, flushed every FLUSH_EVERY octets and at
 *     the end, into a buffer the encoder is offered 1 to 13 octets of room
 *     at a time, so that the overflow buffer (ovf) is in use ("pieces").
 * The pieces case is run again with all the room at once: its output must
 * be the same. Both outputs are decoded back to the input.
 *
 * Then the writer itself against the two it replaced, kept below as they
 * were: random octets, codes and pads, in calls offering random room, on a
 * pair of encoders each, compared member by member after every call. */
#include <string.h>
#include "test.h"
#include "v42_comp.h"
#include "v44_comp.h"
#include "comp_out.h"

#define N 8192
#define FLUSH_EVERY 1000

static const char sentence[] = "the quick brown fox jumps over the lazy dog. ";

static uint32_t fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* the pieces and the room offered: an LCG of its own, seeded per case */
static uint32_t rs;
static unsigned rnd(unsigned m)
{
    rs = rs * 1103515245u + 12345u;
    return (unsigned)(rs >> 16) % m;
}

#define NIN 7
static uint8_t in[NIN][N];
static const char *const in_name[NIN] = { "lcg", "text", "zeros", "sym16", "mixed", "esc", "all256" };
static uint8_t whole[3 * N + 256], pieces[3 * N + 256], roomy[3 * N + 256], back[N + 256];

static void inputs(void)
{
    uint32_t s = 1;
    size_t i, k = strlen(sentence);
    for (i = 0; i < N; i++) {
        s = s * 1103515245u + 12345u;
        in[0][i] = (uint8_t)(s >> 16);
        in[1][i] = (uint8_t)sentence[i % k];
        in[2][i] = 0;
        in[3][i] = (uint8_t)('a' + (s >> 28));
        in[4][i] = (i / 1024) % 2 ? (uint8_t)sentence[i % k] : (uint8_t)(s >> 16);
        in[5][i] = (uint8_t)(i * 51);
        in[6][i] = (uint8_t)i;
    }
}

static const char *const pol_name[3] = { "auto", "always", "never" };

/* ------------------------------------------------------------ V.42 bis */
static v42comp_enc_t benc;
static v42comp_dec_t bdec;

static size_t bis_whole(int x, int p1, int p2, int pol)
{
    size_t n = 0;
    CHECK_EQ_I(v42comp_enc_init(&benc, p1, p2), 0);
    v42comp_enc_policy(&benc, pol);
    CHECK_EQ_I(v42comp_encode(&benc, in[x], N, whole, sizeof whole, &n), N);
    CHECK_EQ_I(v42comp_flush(&benc, whole, sizeof whole, &n), 1);
    return n;
}

/* small: offer 1 to 13 octets of room at a time; else all of it */
static size_t bis_pieces(int x, int p1, int p2, int pol, uint32_t seed, int small, uint8_t *out)
{
    size_t pos = 0, n = 0, stop = FLUSH_EVERY;
    int guard = 0;
    rs = seed;
    CHECK_EQ_I(v42comp_enc_init(&benc, p1, p2), 0);
    v42comp_enc_policy(&benc, pol);
    for (;;) {
        size_t lim = stop < N ? stop : N, k = 1 + rnd(97), room = 1 + rnd(13);
        size_t max = small ? n + room : 3 * N + 256;
        if (k > lim - pos) k = lim - pos;
        if (pos < lim) pos += v42comp_encode(&benc, in[x] + pos, k, out, max, &n);
        else if (v42comp_flush(&benc, out, max, &n)) {
            if (pos == N) break;
            stop += FLUSH_EVERY;
        }
        if (++guard > 1000000) { CHECK(0); break; }
    }
    CHECK_EQ_I(v42comp_enc_pending(&benc), 0);
    return n;
}

static void bis_back(const uint8_t *s, size_t n, int x, int p1, int p2)
{
    size_t used, got = 0, c;
    int rc = 0;
    CHECK_EQ_I(v42comp_dec_init(&bdec, p1, p2), 0);
    for (c = 0; rc == 0 && (c < n || v42comp_dec_pending(&bdec)) && got < N; c += used) {
        size_t before = got;
        rc = v42comp_decode(&bdec, s + c, n - c, &used, back, sizeof back, &got);
        if (used == 0 && got == before) break;
    }
    CHECK_EQ_I(rc, 0);
    CHECK_EQ_I(got, N);
    CHECK(got == N && memcmp(back, in[x], N) == 0);
}

static void bis_case(int x, int p1, int p2, int pol, uint32_t seed)
{
    size_t nw = bis_whole(x, p1, p2, pol), np, nr;
    np = bis_pieces(x, p1, p2, pol, seed, 1, pieces);
    nr = bis_pieces(x, p1, p2, pol, seed, 0, roomy);
    CHECK(np == nr && memcmp(pieces, roomy, np) == 0);
    printf("v42bis %-6s P1 %4d P2 %3d %-6s: whole %5lu octets %08lx, pieces %5lu octets %08lx\n",
           in_name[x], p1, p2, pol_name[pol], (unsigned long)nw, (unsigned long)fnv(whole, nw),
           (unsigned long)np, (unsigned long)fnv(pieces, np));
    bis_back(whole, nw, x, p1, p2);
    bis_back(pieces, np, x, p1, p2);
}

/* ----------------------------------------------------------------- V.44 */
static v44comp_enc_t venc;
static v44comp_dec_t vdec;

static size_t v44_whole(int x, int p1, int p2, int p3, int pol)
{
    size_t n = 0;
    CHECK_EQ_I(v44comp_enc_init(&venc, p1, p2, p3), 0);
    v44comp_enc_policy(&venc, pol);
    CHECK_EQ_I(v44comp_encode(&venc, in[x], N, whole, sizeof whole, &n), N);
    CHECK_EQ_I(v44comp_flush(&venc, whole, sizeof whole, &n), 1);
    return n;
}

static size_t v44_pieces(int x, int p1, int p2, int p3, int pol, uint32_t seed, int small, uint8_t *out)
{
    size_t pos = 0, n = 0, stop = FLUSH_EVERY;
    int guard = 0;
    rs = seed;
    CHECK_EQ_I(v44comp_enc_init(&venc, p1, p2, p3), 0);
    v44comp_enc_policy(&venc, pol);
    for (;;) {
        size_t lim = stop < N ? stop : N, k = 1 + rnd(97), room = 1 + rnd(13);
        size_t max = small ? n + room : 3 * N + 256;
        if (k > lim - pos) k = lim - pos;
        if (pos < lim) pos += v44comp_encode(&venc, in[x] + pos, k, out, max, &n);
        else if (v44comp_flush(&venc, out, max, &n)) {
            if (pos == N) break;
            stop += FLUSH_EVERY;
        }
        if (++guard > 1000000) { CHECK(0); break; }
    }
    CHECK_EQ_I(v44comp_enc_pending(&venc), 0);
    return n;
}

static void v44_back(const uint8_t *s, size_t n, int x, int p1, int p2, int p3)
{
    size_t used, got = 0, c;
    int rc = 0;
    CHECK_EQ_I(v44comp_dec_init(&vdec, p1, p2, p3), 0);
    for (c = 0; rc == 0 && (c < n || v44comp_dec_pending(&vdec)) && got < N; c += used) {
        size_t before = got;
        rc = v44comp_decode(&vdec, s + c, n - c, &used, back, sizeof back, &got);
        if (used == 0 && got == before) break;
    }
    CHECK_EQ_I(rc, 0);
    CHECK_EQ_I(got, N);
    CHECK(got == N && memcmp(back, in[x], N) == 0);
}

static void v44_case(int x, int p1, int p2, int p3, int pol, uint32_t seed)
{
    size_t nw = v44_whole(x, p1, p2, p3, pol), np, nr;
    np = v44_pieces(x, p1, p2, p3, pol, seed, 1, pieces);
    nr = v44_pieces(x, p1, p2, p3, pol, seed, 0, roomy);
    CHECK(np == nr && memcmp(pieces, roomy, np) == 0);
    printf("v44    %-6s P1 %4d P2 %3d P3 %4d %-6s: whole %5lu octets %08lx, pieces %5lu octets %08lx\n",
           in_name[x], p1, p2, p3, pol_name[pol], (unsigned long)nw, (unsigned long)fnv(whole, nw),
           (unsigned long)np, (unsigned long)fnv(pieces, np));
    v44_back(whole, nw, x, p1, p2, p3);
    v44_back(pieces, np, x, p1, p2, p3);
}

/* ------------------------------------------------- the writer, old and new */
/* The V.42 bis writer before comp_out.h (v42_comp.c), as it was */
static void old42_put_octet(v42comp_enc_t *e, unsigned o)
{
    e->out_octets++;
    if (e->ovf_len == e->ovf_pos && e->out_len < e->out_max)
        e->out[e->out_len++] = (uint8_t)o;
    else if (e->ovf_len < V42COMP_OVF)
        e->ovf[e->ovf_len++] = (uint8_t)o;
}

static void old42_put_bits(v42comp_enc_t *e, unsigned v, int n)
{
    e->bitbuf |= (uint32_t)v << e->nbits;
    e->nbits += n;
    e->win_bits += (uint32_t)n;
    while (e->nbits >= 8) {
        old42_put_octet(e, e->bitbuf & 0xFF);
        e->bitbuf >>= 8;
        e->nbits -= 8;
    }
}

static void old42_pad_to_octet(v42comp_enc_t *e)
{
    if (e->nbits > 0) {
        old42_put_octet(e, e->bitbuf & 0xFF);
        e->bitbuf = 0; e->nbits = 0;
    }
}

static void old42_enc_begin(v42comp_enc_t *e, uint8_t *out, size_t outmax, size_t outlen)
{
    e->out = out; e->out_max = outmax; e->out_len = outlen;
    while (e->ovf_pos < e->ovf_len && e->out_len < e->out_max)
        e->out[e->out_len++] = e->ovf[e->ovf_pos++];
    if (e->ovf_pos == e->ovf_len) { e->ovf_pos = 0; e->ovf_len = 0; }
}

/* The V.44 writer before comp_out.h (v44_comp.c), as it was */
static void old44_put_octet(v44comp_enc_t *e, unsigned o)
{
    e->out_octets++;
    if (e->ovf_len == e->ovf_pos && e->out && e->out_len < e->out_max)
        e->out[e->out_len++] = (uint8_t)o;
    else if (e->ovf_len < V44COMP_OVF)
        e->ovf[e->ovf_len++] = (uint8_t)o;
}

static void old44_put_bits(v44comp_enc_t *e, uint32_t v, int n)
{
    e->win_bits += (uint32_t)n;
    if (!e->compressed) return;
    e->bitbuf |= v << e->nbits;
    e->nbits += n;
    while (e->nbits >= 8) {
        old44_put_octet(e, e->bitbuf & 0xFF);
        e->bitbuf >>= 8;
        e->nbits -= 8;
    }
}

static void old44_pad_to_octet(v44comp_enc_t *e)
{
    if (e->nbits > 0) {
        e->win_bits += (uint32_t)(8 - e->nbits);
        old44_put_octet(e, e->bitbuf & 0xFF);
        e->bitbuf = 0; e->nbits = 0;
    }
}

static void old44_enc_begin(v44comp_enc_t *e, uint8_t *out, size_t outmax, size_t outlen)
{
    e->out = out; e->out_max = outmax; e->out_len = outlen;
    while (e->ovf_pos < e->ovf_len && e->out_len < e->out_max)
        e->out[e->out_len++] = e->ovf[e->ovf_pos++];
    if (e->ovf_pos == e->ovf_len) { e->ovf_pos = 0; e->ovf_len = 0; }
}

/* comp_out.h's writer for each, under names of its own here */
#define out_begin w42_begin
#define out_end w42_end
#define out_pending w42_pending
#define out_octet w42_octet
#define out_bits w42_bits
#define out_pad w42_pad
COMP_OUT_WRITER(v42comp_enc_t, V42COMP_OVF)
#undef out_begin
#undef out_end
#undef out_pending
#undef out_octet
#undef out_bits
#undef out_pad
#define out_begin w44_begin
#define out_end w44_end
#define out_pending w44_pending
#define out_octet w44_octet
#define out_bits w44_bits
#define out_pad w44_pad
COMP_OUT_WRITER(v44comp_enc_t, V44COMP_OVF)
#undef out_begin
#undef out_end
#undef out_pending
#undef out_octet
#undef out_bits
#undef out_pad

/* and what the encoders now put around it (v42_comp.c, v44_comp.c) */
static void new42_put_bits(v42comp_enc_t *e, unsigned v, int n)
{
    e->win_bits += (uint32_t)n;
    w42_bits(e, v, n);
}

static void new44_put_bits(v44comp_enc_t *e, uint32_t v, int n)
{
    e->win_bits += (uint32_t)n;
    if (!e->compressed) return;
    w44_bits(e, v, n);
}

static void new44_pad_to_octet(v44comp_enc_t *e)
{
    if (e->nbits > 0) e->win_bits += (uint32_t)(8 - e->nbits);
    w44_pad(e);
}

/* the writer's members of a pair, and what each put in its buffer */
#define SAME_WRITER(a, b, oa, ob) \
    ((a)->bitbuf == (b)->bitbuf && (a)->nbits == (b)->nbits \
     && (a)->ovf_len == (b)->ovf_len && (a)->ovf_pos == (b)->ovf_pos \
     && memcmp((a)->ovf, (b)->ovf, sizeof (a)->ovf) == 0 \
     && (a)->out_max == (b)->out_max && (a)->out_len == (b)->out_len \
     && (a)->out_octets == (b)->out_octets && (a)->win_bits == (b)->win_bits \
     && memcmp((oa), (ob), sizeof (oa)) == 0)

#define CALLS 20000
static v42comp_enc_t o42, n42;
static v44comp_enc_t o44, n44;
static uint8_t ob_old[256], ob_new[256];

static void writer_v42(void)
{
    int call, k, bad = 0;
    size_t len_old = 0, len_new = 0;
    unsigned long octets = 0;
    rs = 42;
    memset(&o42, 0, sizeof o42); memset(&n42, 0, sizeof n42);
    for (call = 0; call < CALLS; call++) {
        size_t room = rnd(4) ? rnd(9) : rnd(80), at = rnd(4) ? 0 : rnd(64);
        int ops = (int)rnd(12);
        memset(ob_old, 0, sizeof ob_old); memset(ob_new, 0, sizeof ob_new);
        if (at + room > sizeof ob_old) room = sizeof ob_old - at;
        old42_enc_begin(&o42, ob_old, at + room, at);
        w42_begin(&n42, ob_new, at + room, at);
        for (k = 0; k < ops; k++) {
            unsigned what = rnd(8), v = rnd(65536);
            int n = 9 + (int)rnd(8);                    /* C2: 9 to N1 (16) */
            if (what == 0) { old42_put_octet(&o42, v & 0xFF); w42_octet(&n42, v & 0xFF); }
            else if (what == 1) { old42_pad_to_octet(&o42); w42_pad(&n42); }
            else { v &= (1u << n) - 1; old42_put_bits(&o42, v, n); new42_put_bits(&n42, v, n); }
        }
        len_old = o42.out_len; o42.out = NULL;
        len_new = w42_end(&n42);
        if (len_old != len_new || !SAME_WRITER(&o42, &n42, ob_old, ob_new)
            || v42comp_enc_pending(&o42) != w42_pending(&n42)) bad++;
    }
    octets = n42.out_octets;
    CHECK_EQ_I(bad, 0);
    printf("writer v42bis: %d calls, %lu octets, the same as before\n", CALLS, octets);
}

static void writer_v44(void)
{
    int call, k, bad = 0;
    size_t len_old = 0, len_new = 0;
    rs = 44;
    memset(&o44, 0, sizeof o44); memset(&n44, 0, sizeof n44);
    for (call = 0; call < CALLS; call++) {
        size_t room = rnd(4) ? rnd(9) : rnd(80), at = rnd(4) ? 0 : rnd(64);
        int ops = (int)rnd(12), none = rnd(16) == 0;  /* no buffer at all */
        memset(ob_old, 0, sizeof ob_old); memset(ob_new, 0, sizeof ob_new);
        if (at + room > sizeof ob_old) room = sizeof ob_old - at;
        if (rnd(8) == 0) o44.compressed = n44.compressed = !o44.compressed;
        old44_enc_begin(&o44, none ? NULL : ob_old, none ? 0 : at + room, none ? 0 : at);
        w44_begin(&n44, none ? NULL : ob_new, none ? 0 : at + room, none ? 0 : at);
        for (k = 0; k < ops; k++) {
            unsigned what = rnd(8);
            uint32_t v = (uint32_t)rnd(65536) << 1 | rnd(2);
            int n = 1 + 6 + (int)rnd(11);               /* prefix and C2: 6 to N1 (16) */
            if (what == 0) { old44_put_octet(&o44, v & 0xFF); w44_octet(&n44, v & 0xFF); }
            else if (what == 1) { old44_pad_to_octet(&o44); new44_pad_to_octet(&n44); }
            else { v &= (1u << n) - 1; old44_put_bits(&o44, v, n); new44_put_bits(&n44, v, n); }
        }
        len_old = o44.out_len; o44.out = NULL;
        len_new = w44_end(&n44);
        if (len_old != len_new || o44.compressed != n44.compressed
            || !SAME_WRITER(&o44, &n44, ob_old, ob_new)
            || v44comp_enc_pending(&o44) != w44_pending(&n44)) bad++;
    }
    CHECK_EQ_I(bad, 0);
    printf("writer v44: %d calls, %lu octets, the same as before\n", CALLS, n44.out_octets);
}

int main(void)
{
    /* P1 512 fills and recycles the dictionary; P2 250 and 255 are the
     * longest strings; V.44 256 codewords and a history of 512 reinitialize
     * it often */
    static const int b1[2] = { 512, 2048 }, b2[2] = { 6, 250 };
    static const int v1[3] = { 256, 1024, 2048 }, v2[3] = { 32, 255, 255 }, v3[3] = { 512, 3072, 4096 };
    int x, i, j, pol;
    uint32_t seed = 1;
    inputs();
    for (x = 0; x < NIN; x++)
        for (i = 0; i < 2; i++)
            for (j = 0; j < 2; j++)
                for (pol = 0; pol < 3; pol++) bis_case(x, b1[i], b2[j], pol, seed++);
    for (x = 0; x < NIN; x++)
        for (i = 0; i < 3; i++)
            for (pol = 0; pol < 3; pol++) v44_case(x, v1[i], v2[i], v3[i], pol, seed++);
    writer_v42();
    writer_v44();
    T_REPORT("test_comp_out");
}
