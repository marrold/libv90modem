/* K56flex's training transmitter (modem/k56tx; docs/history/k56-facts.md
 * "Training down", "Phase 4", "Rate renegotiation", "Oracle"). Against
 * D-Modem's training as data (test/data/k56tx_oracle.h), driven at the
 * same symbols; then the rules on their own: TRN1B's magnitudes, the pad's
 * magnitudes, Js, silence 2's length, the grid of 24, a refused J, a
 * renegotiation in mid-frame. */
#include <inttypes.h>
#include <string.h>
#include "test.h"
#include "k56tx.h"
#include "v90_scr.h"
#include "data/k56tx_oracle.h"

#define NCASE (int)(sizeof k56tx_oracle_case / sizeof k56tx_oracle_case[0])
#define CAP 80000

typedef struct { int32_t len; uint32_t fnv; uint8_t first[8]; } seg_rec;

/* a whole run's octets and their segments, when captured */
static uint8_t cap_oct[CAP];
static uint8_t cap_seg[CAP];
static int32_t cap_n;

static int xs_bit(void *u)
{
    uint32_t *x = (uint32_t *)u;
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return (int)(*x & 1u);
}

static void record(seg_rec *s, uint8_t o)
{
    if (s->len == 0)
        s->fnv = 2166136261u;
    if (s->len < 8)
        s->first[s->len] = o;
    s->fnv = (s->fnv ^ o) * 16777619u;
    s->len++;
}

static void our_mp(v34_mp_t *mp)
{
    memset(mp, 0, sizeof *mp);
    mp->max_ca = K56TX_ORACLE_RATE_CA;
    mp->max_ac = K56TX_ORACLE_RATE_AC;
    mp->rates = 0x3fff;
    mp->asym = 1;
}

typedef struct {
    int32_t data_sym;   /* k56tx_sym at the first data symbol */
    int     rate_train; /* k56tx_rate seen in training (should be 0) */
    int     rate_data;  /* k56tx_rate in data */
} drive_out;

/* from where t is to the end of the first data frame, as the oracle's
 * driver: the J before symbol j_at of silence 2 (j_at < 0: none), our MP
 * (and the data's parameters) before symbol mp_at of TRN2B */
static void drive(k56tx_t *t, uint8_t j, int32_t j_at, int32_t mp_at, uint32_t *xs,
                  seg_rec *rec, int capture, drive_out *out)
{
    int cur = -1, j_given = 0, mp_given = 0, data = 0, s;
    int32_t n = 0, guard;
    v34_mp_t mp;

    our_mp(&mp);
    memset(out, 0, sizeof *out);
    for (guard = 0; guard < 200000; guard++) {
        s = (int)k56tx_segment(t);
        if (s != cur) { cur = s; n = 0; }
        if (s == K56TX_SIL2 && !j_given && n == j_at) {
            k56tx_got_j(t, j);
            j_given = 1;
            s = (int)k56tx_segment(t);
            if (s != cur) { cur = s; n = 0; }
        }
        if (s == K56TX_TRN2B && !mp_given && n == mp_at) {
            k56tx_data(t, K56TX_ORACLE_RATE_AC, (j >> 6) & 3, (uint8_t)(j & 63), xs_bit, xs);
            k56tx_set_mp(t, &mp);
            mp_given = 1;
            s = (int)k56tx_segment(t);
            if (s != cur) { cur = s; n = 0; }
        }
        if (s == K56TX_DATA && data == 0) {
            out->data_sym = k56tx_sym(t);
            out->rate_data = k56tx_rate(t);
        }
        if (s == K56TX_DATA && data == K56ENC_N)
            return;
        if (s != K56TX_DATA && k56tx_rate(t))
            out->rate_train = k56tx_rate(t);
        {
            uint8_t o = k56tx_tick(t);
            if (rec)
                record(&rec[s], o);
            if (capture && cap_n < CAP) {
                cap_oct[cap_n] = o;
                cap_seg[cap_n] = (uint8_t)s;
                cap_n++;
            }
        }
        if (s == K56TX_DATA)
            data++;
        n++;
    }
    printf("  drive: no data within %d symbols\n", (int)guard);
    t_checks++;
    t_fails++;
}

/* the first training (our MP at RENEG_FIRST_MP_AT), then data to
 * RENEG_FRAMES frames in all, then the renegotiation from R2 with our MP
 * at mp_at of its TRN2B */
static void drive_reneg(k56tx_t *t, uint8_t j, int32_t j_at, int32_t mp_at, uint32_t *xs,
                        seg_rec *rec, int capture, drive_out *out)
{
    drive_out first;
    int32_t k;
    drive(t, j, j_at, K56TX_ORACLE_RENEG_FIRST_MP_AT, xs, NULL, 0, &first);
    for (k = K56ENC_N; k < K56TX_ORACLE_RENEG_FRAMES * K56ENC_N; k++)
        (void)k56tx_tick(t);
    k56tx_reneg(t);
    drive(t, j, -1, mp_at, xs, rec, capture, out);
}

static void test_mp_bits(void)
{
    v34_mp_t mp;
    uint8_t b[V34_MP_MAX_BITS], o[11];
    int a, i, n, bad = 0;
    our_mp(&mp);
    for (a = 0; a < 2; a++) {
        mp.ack = (uint8_t)a;
        n = v34_mp_build(&mp, b);
        CHECK_EQ_I(n, V34_MP0_BITS);
        memset(o, 0, sizeof o);
        for (i = 0; i < V34_MP0_BITS; i++)
            o[i / 8] = (uint8_t)(o[i / 8] | (b[i] & 1) << (i % 8));
        bad += memcmp(o, k56tx_oracle_mp[a], 11) != 0;
    }
    printf("  our MP and MP' equal the oracle's: %s\n", bad ? "no" : "yes");
    CHECK_EQ_I(bad, 0);
}

static void test_oracle(void)
{
    static k56tx_t t;
    int c, s, cases_ok = 0;
    for (c = 0; c < NCASE; c++) {
        seg_rec rec[K56TX_ORACLE_NSEG];
        drive_out out;
        uint32_t xs = 1;
        int bad = 0;
        memset(rec, 0, sizeof rec);
        k56tx_init(&t);
        if (k56tx_oracle_case[c].reneg)
            drive_reneg(&t, k56tx_oracle_case[c].j_octet, k56tx_oracle_case[c].j_at,
                        k56tx_oracle_case[c].mp_at, &xs, rec, 0, &out);
        else
            drive(&t, k56tx_oracle_case[c].j_octet, k56tx_oracle_case[c].j_at,
                  k56tx_oracle_case[c].mp_at, &xs, rec, 0, &out);
        for (s = 0; s < K56TX_ORACLE_NSEG; s++) {
            const seg_rec *g = &rec[s];
            int differ = g->len != k56tx_oracle_seg[c][s].len ||
                         (g->len && g->fnv != k56tx_oracle_seg[c][s].fnv) ||
                         memcmp(g->first, k56tx_oracle_seg[c][s].first, 8) != 0;
            if (differ) {
                printf("  case %d (J %02x at %d, MP at %d%s) segment %d: len %d fnv %08" PRIx32
                       ", oracle len %d fnv %08" PRIx32 "\n",
                       c, k56tx_oracle_case[c].j_octet, (int)k56tx_oracle_case[c].j_at,
                       (int)k56tx_oracle_case[c].mp_at, k56tx_oracle_case[c].reneg ? ", reneg" : "",
                       s, (int)g->len, g->fnv, (int)k56tx_oracle_seg[c][s].len,
                       k56tx_oracle_seg[c][s].fnv);
                bad++;
            }
        }
        CHECK_EQ_I(bad, 0);
        CHECK_EQ_I(out.rate_train, 0);
        CHECK_EQ_I(out.rate_data, 30000 + 2000 * K56TX_ORACLE_RATE_AC);
        cases_ok += bad == 0;
    }
    printf("  oracle: %d of %d cases equal to D-Modem's, segment by segment\n", cases_ok, NCASE);
}

static int mag(uint8_t o) { return 0x7f - (o & 0x7f); }

static int32_t seg_start(int s)
{
    int32_t i;
    for (i = 0; i < cap_n; i++)
        if (cap_seg[i] == s)
            return i;
    return -1;
}

static int32_t seg_len(int s)
{
    int32_t i, n = 0;
    for (i = 0; i < cap_n; i++)
        n += cap_seg[i] == s;
    return n;
}

/* TRN1B: 3 scrambled ones a symbol give its magnitude; Js: the even signs
 * descramble to J 0x89b0 */
static void test_trn1(void)
{
    static const uint8_t trn1b[8] = { 0x40, 0x41, 0x4b, 0x4c, 0x50, 0x51, 0x50, 0x51 };
    static k56tx_t t;
    drive_out out;
    v90_scr_t s, d;
    uint32_t xs = 1;
    int32_t a, b, c, js, i;
    int bad = 0, jbad = 0, words = 0;
    uint16_t w = 0;

    k56tx_init(&t);
    cap_n = 0;
    drive(&t, 0x00, 12000, 3000, &xs, NULL, 1, &out);
    a = seg_start(K56TX_TRN1A);
    b = seg_start(K56TX_TRN1B);
    c = seg_start(K56TX_TRN1C);
    js = seg_start(K56TX_JS);
    CHECK_EQ_I(seg_len(K56TX_TRN1B), K56TX_TRN1B_N);
    v90_scr_init(&s);
    for (i = a; i < b; i++)
        (void)v90_scramble_gpa(&s, 1);
    for (i = b; i < c; i++) {
        int b0 = v90_scramble_gpa(&s, 1), b1 = v90_scramble_gpa(&s, 1), b2 = v90_scramble_gpa(&s, 1);
        bad += mag(cap_oct[i]) != trn1b[b0 + 2 * b1 + 4 * b2];
    }
    printf("  TRN1B: %d of %d magnitudes not 40 41 4b 4c 50 51 50 51 by 3 scrambled bits\n",
           (int)bad, (int)(c - b));
    CHECK_EQ_I(bad, 0);

    /* the client's view: TRN1C's and Js's even signs through GPA */
    CHECK_EQ_I(seg_len(K56TX_JS), K56TX_JS_N);
    v90_scr_init(&d);
    for (i = c; i < js; i += 2)
        (void)v90_descramble_gpa(&d, cap_oct[i] >> 7);
    for (i = 0; i < K56TX_JS_N; i += 2) {
        int bit = v90_descramble_gpa(&d, cap_oct[js + i] >> 7);
        int k = (i / 2) % 16;
        jbad += bit != v34_j16[k];
        w = (uint16_t)(w | bit << k);
        if (k == 15) {
            words += w == 0x89b0;
            w = 0;
        }
    }
    printf("  Js: %d words of 0x89b0, %d bits wrong\n", words, jbad);
    CHECK_EQ_I(jbad, 0);
    CHECK(words >= 2);
}

/* TRN2B, MP, MP' and E at pad 1 on 35 4d 58 60; in a renegotiation MP,
 * MP' and E on 44 5d */
static void test_pad_mags(void)
{
    static const uint8_t pad2[4] = { 0x35, 0x4d, 0x58, 0x60 }, pad1[2] = { 0x44, 0x5d };
    static k56tx_t t;
    drive_out out;
    uint32_t xs = 1;
    int32_t i;
    int bad = 0, bad_rn = 0, k, n = 0, n_rn = 0;

    k56tx_init(&t);
    cap_n = 0;
    drive(&t, 0x40, 12000, 3000, &xs, NULL, 1, &out);
    for (i = 0; i < cap_n; i++) {
        int s = cap_seg[i], ok = 0;
        if (s < K56TX_TRN2B || s > K56TX_E)
            continue;
        for (k = 0; k < 4; k++)
            ok |= mag(cap_oct[i]) == pad2[k];
        bad += !ok;
        n++;
    }
    printf("  pad 1: %d of %d TRN2B / MP / MP' / E symbols off 35 4d 58 60\n", bad, n);
    CHECK_EQ_I(bad, 0);
    CHECK(n > 3000);

    k56tx_init(&t);
    xs = 1;
    cap_n = 0;
    drive_reneg(&t, 0x40, 12000, 1500, &xs, NULL, 1, &out);
    for (i = 0; i < cap_n; i++) {
        int s = cap_seg[i];
        if (s < K56TX_MP || s > K56TX_E)
            continue;
        bad_rn += mag(cap_oct[i]) != pad1[0] && mag(cap_oct[i]) != pad1[1];
        n_rn++;
    }
    printf("  pad 1, renegotiation: %d of %d MP / MP' / E symbols off 44 5d; E %d\n",
           bad_rn, n_rn, (int)seg_len(K56TX_E));
    CHECK_EQ_I(bad_rn, 0);
    CHECK_EQ_I(n_rn, 2 * K56TX_MP_N + K56TX_RN_E_N);
    CHECK_EQ_I(seg_len(K56TX_TRN2A), K56TX_RN_TRN2A_N);
    CHECK_EQ_I(seg_len(K56TX_E), K56TX_RN_E_N);
    CHECK_EQ_I(out.data_sym % 24, 0);
}

/* TRN2A's even signs through GPA, one bit a symbol: the ones left out of
 * 2048 / 2 - 23 */
static int trn2a_not_ones(void)
{
    v90_scr_t d;
    int32_t i, k = 0;
    int bad = 0;
    v90_scr_init(&d);
    for (i = 0; i < cap_n; i++) {
        if (cap_seg[i] != K56TX_TRN2A)
            continue;
        if (!(k & 1)) {
            int b = v90_descramble_gpa(&d, cap_oct[i] >> 7);
            bad += k >= 2 * 23 && b != 1;
        }
        k++;
    }
    return bad;
}

/* the training scrambler after the captured E (pad 0): its bits read back
 * from the magnitudes, nb a symbol (b0 first) */
static v90_scr_t e_state(int nb)
{
    static const uint8_t p2[4] = { 0x2f, 0x45, 0x51, 0x57 }, p1[2] = { 0x3d, 0x55 };
    v90_scr_t r;
    int32_t i;
    int k;
    r.reg = 0;
    for (i = 0; i < cap_n; i++) {
        if (cap_seg[i] != K56TX_E)
            continue;
        if (nb == 2) {
            for (k = 0; k < 4 && p2[k] != mag(cap_oct[i]); k++)
                ;
            r.reg = r.reg << 2 | (uint32_t)(k & 1) << 1 | (uint32_t)(k >> 1);
        } else {
            r.reg = r.reg << 1 | (uint32_t)(p1[1] == mag(cap_oct[i]));
        }
    }
    return r;
}

/* TRN2A's (and MARK's) even signs against a model scrambling nb bits an
 * even symbol (the value 1, or 0 in MARK, lowest bit first) from the
 * state s: the symbols that differ */
static int trn2a_vs_model(v90_scr_t s, int nb)
{
    int32_t i, k = 0;
    int bad = 0;
    for (i = 0; i < cap_n; i++) {
        int seg = cap_seg[i];
        if (seg != K56TX_TRN2A && seg != K56TX_MARK)
            continue;
        if (!(k & 1)) {
            int b = v90_scramble_gpa(&s, seg == K56TX_TRN2A);
            if (nb == 2)
                (void)v90_scramble_gpa(&s, 0);
            bad += b != (cap_oct[i] >> 7);
        }
        k++;
    }
    return bad;
}

/* the bits a symbol carry into the next TRN2A and MARK, as in D-Modem
 * (the oracle's renegotiation): after the first Phase 4 (2 bits a symbol)
 * the renegotiation's TRN2A scrambles 2 bits an even symbol, a one and a
 * zero, the sign the first; after a renegotiation's (1 bit) the next
 * TRN2A 1. Either way the client, descrambling one sign an even symbol,
 * sees ones (the scrambler's sequence taken every second bit obeys the
 * same recurrence); in a second renegotiation too */
static void test_reneg_bits(void)
{
    static k56tx_t t;
    drive_out out;
    uint32_t xs = 1;
    int32_t k;
    int first, second, call, m[2][2];
    v90_scr_t e1, e2;

    k56tx_init(&t);
    cap_n = 0;
    drive(&t, 0x00, 12000, 3000, &xs, NULL, 1, &out);
    call = trn2a_not_ones();
    e1 = e_state(2);
    for (k = K56ENC_N; k < 10 * K56ENC_N; k++)
        (void)k56tx_tick(&t);
    k56tx_reneg(&t);
    cap_n = 0;
    drive(&t, 0x00, -1, 2048, &xs, NULL, 1, &out);
    first = trn2a_not_ones();
    m[0][0] = trn2a_vs_model(e1, 1);
    m[0][1] = trn2a_vs_model(e1, 2);
    e2 = e_state(1);
    for (k = K56ENC_N; k < 10 * K56ENC_N; k++)
        (void)k56tx_tick(&t);
    k56tx_reneg(&t);
    cap_n = 0;
    drive(&t, 0x00, -1, 2048, &xs, NULL, 1, &out);
    second = trn2a_not_ones();
    m[1][0] = trn2a_vs_model(e2, 1);
    m[1][1] = trn2a_vs_model(e2, 2);
    printf("  TRN2A / MARK even signs off a model from the last E's bits, 1 / 2 bits a symbol:"
           " first renegotiation %d / %d, second %d / %d\n", m[0][0], m[0][1], m[1][0], m[1][1]);
    CHECK(m[0][0] > 100);
    CHECK_EQ_I(m[0][1], 0);
    CHECK_EQ_I(m[1][0], 0);
    CHECK(m[1][1] > 100);
    printf("  TRN2A's even signs descrambled, not ones: call %d, first renegotiation %d, second %d\n",
           call, first, second);
    CHECK_EQ_I(call, 0);
    CHECK_EQ_I(first, 0);
    CHECK_EQ_I(second, 0);
    CHECK_EQ_I(out.data_sym % 24, 0);
}

/* a J given before silence 2 is kept: silence 2 is then 12288 and the
 * J's pad is used */
static void test_early_j(void)
{
    static k56tx_t t;
    int32_t n = 0, guard;
    int bad = 0;
    k56tx_init(&t);
    while (k56tx_segment(&t) != K56TX_JS)
        (void)k56tx_tick(&t);
    k56tx_got_j(&t, 0x40);
    for (guard = 0; guard < 100000 && k56tx_segment(&t) != K56TX_R2; guard++) {
        n += k56tx_segment(&t) == K56TX_SIL2;
        (void)k56tx_tick(&t);
    }
    CHECK_EQ_I(k56tx_segment(&t), K56TX_R2);
    CHECK_EQ_I(n, K56TX_SIL2_MIN);
    for (guard = 0; guard < 20000 && k56tx_segment(&t) != K56TX_TRN2B; guard++)
        (void)k56tx_tick(&t);
    CHECK_EQ_I(k56tx_segment(&t), K56TX_TRN2B);
    for (guard = 0; guard < 100; guard++) {
        int m = mag(k56tx_tick(&t));
        bad += m != 0x35 && m != 0x4d && m != 0x58 && m != 0x60;
    }
    CHECK_EQ_I(bad, 0);
    printf("  J in Js: silence 2 %d, TRN2B on pad 1's magnitudes (%d off)\n", (int)n, bad);
}

/* k56tx_reneg outside data: R2 at once, then the renegotiation's lengths */
static void test_reneg_training(void)
{
    static k56tx_t t;
    drive_out out;
    uint32_t xs = 1;
    k56tx_init(&t);
    while (k56tx_segment(&t) != K56TX_TRN1A)
        (void)k56tx_tick(&t);
    (void)k56tx_tick(&t);
    k56tx_reneg(&t);
    CHECK_EQ_I(k56tx_segment(&t), K56TX_R2);
    cap_n = 0;
    drive(&t, 0x00, -1, 2048, &xs, NULL, 1, &out);
    CHECK_EQ_I(seg_len(K56TX_R2), K56TX_R_N);
    CHECK_EQ_I(seg_len(K56TX_TRN2A), K56TX_RN_TRN2A_N);
    CHECK_EQ_I(seg_len(K56TX_MP), K56TX_MP_N);
    CHECK_EQ_I(seg_len(K56TX_E), K56TX_RN_E_N);
    CHECK_EQ_I(seg_len(K56TX_TRN1B), 0);
    printf("  renegotiation in TRN1A: R2 at once; TRN2A %d, E %d\n",
           (int)seg_len(K56TX_TRN2A), (int)seg_len(K56TX_E));
}

/* k56tx_reneg with our MP, MP' or E going out (its S in its B1, Task
 * 10d): R2 not before our data has begun, at the end of its first frame
 * (the client looks for R in our data, at a frame boundary) */
static void test_reneg_in_e(void)
{
    static const k56tx_seg in[3] = { K56TX_MP, K56TX_MPA, K56TX_E };
    static k56tx_t t;
    v34_mp_t mp;
    int32_t n, data_n, r2_n;
    int i;
    for (i = 0; i < 3; i++) {
        k56tx_init(&t);
        our_mp(&mp);
        k56tx_data(&t, 13, 0, 0, NULL, NULL);
        k56tx_set_mp(&t, &mp);
        for (n = 0; n < 200000 && k56tx_segment(&t) < in[i]; n++) {
            if (k56tx_segment(&t) == K56TX_SIL2) k56tx_got_j(&t, 0x00);
            (void)k56tx_tick(&t);
        }
        CHECK_EQ_I(k56tx_segment(&t), in[i]);
        (void)k56tx_tick(&t);
        k56tx_reneg(&t);
        CHECK_EQ_I(k56tx_segment(&t), in[i]);
        data_n = r2_n = -1;
        for (n = 0; n < 4000 && r2_n < 0; n++) {
            (void)k56tx_tick(&t);
            if (data_n < 0 && k56tx_segment(&t) == K56TX_DATA) data_n = n;
            if (k56tx_segment(&t) == K56TX_R2) r2_n = n;
        }
        printf("  renegotiation in our %s: data after %d symbols, R2 after %d\n",
               i == 0 ? "MP" : i == 1 ? "MP'" : "E", (int)data_n, (int)r2_n);
        CHECK(data_n >= 0);
        CHECK_EQ_I(r2_n - data_n, K56ENC_N);
    }
}

/* silence 2: max(12288, the J's symbol) rounded up to 8 */
static int32_t sil2_for(int32_t j_at, uint8_t j, int32_t extra, int *left_sil2)
{
    static k56tx_t t;
    int32_t n = 0, guard;
    int given = 0;
    k56tx_init(&t);
    *left_sil2 = 0;
    for (guard = 0; guard < 120000; guard++) {
        k56tx_seg s = k56tx_segment(&t);
        if (s == K56TX_SIL2 && !given && n == j_at) {
            k56tx_got_j(&t, j);
            given = 1;
        }
        s = k56tx_segment(&t);
        if (s == K56TX_R2)
            break;
        if (s == K56TX_SIL2) {
            n++;
            if (extra && n == j_at + extra)
                return n;   /* still in silence 2 */
        }
        (void)k56tx_tick(&t);
    }
    *left_sil2 = 1;
    return n;
}

static void test_sil2(void)
{
    static const int32_t at[] = { 0, 12287, 12288, 12289, 13000, 13001, 13007 };
    static const int32_t want[] = { 12288, 12288, 12288, 12296, 13000, 13008, 13008 };
    int i, left;
    for (i = 0; i < (int)(sizeof at / sizeof at[0]); i++) {
        int32_t n = sil2_for(at[i], 0x00, 0, &left);
        CHECK_EQ_I(left, 1);
        CHECK_EQ_I(n, want[i]);
    }
    printf("  silence 2: max(12288, J) rounded up to 8 at J 0 .. 13007\n");

    /* pad code 3: refused, silence 2 goes on */
    {
        int32_t n = sil2_for(12000, 0xc0, 20000, &left);
        CHECK_EQ_I(left, 0);
        CHECK_EQ_I(n, 32000);
    }
    {
        static k56tx_t t;
        int32_t k;
        k56tx_init(&t);
        while (k56tx_segment(&t) != K56TX_SIL2)
            (void)k56tx_tick(&t);
        for (k = 0; k < 12000; k++)
            (void)k56tx_tick(&t);
        k56tx_got_j(&t, 0xc0);
        for (k = 0; k < 20000; k++)
            (void)k56tx_tick(&t);
        CHECK_EQ_I(k56tx_segment(&t), K56TX_SIL2);
        /* a good J after it ends silence 2 at once (32000 sent) */
        k56tx_got_j(&t, 0x40);
        CHECK_EQ_I(k56tx_segment(&t), K56TX_R2);
        printf("  J with pad 3 refused: silence 2 still after 32000; a good J then ends it\n");
    }
}

/* the grid: the first data symbol at a multiple of 24 from TRN1B's first,
 * TRN2B at least 2048 and no longer than needed */
static void test_grid(void)
{
    static k56tx_t t;
    int32_t mp_at;
    int bad = 0;
    for (mp_at = 2048; mp_at <= 2071; mp_at++) {
        seg_rec rec[K56TX_ORACLE_NSEG];
        drive_out out;
        uint32_t xs = 1;
        int32_t over;
        memset(rec, 0, sizeof rec);
        k56tx_init(&t);
        drive(&t, 0x12, 12000, mp_at, &xs, rec, 0, &out);
        over = rec[K56TX_TRN2B].len - mp_at;
        if (out.data_sym % 24 != 0 || rec[K56TX_TRN2B].len < K56TX_TRN2B_MIN || over < 0 || over >= 24 ||
            rec[K56TX_DATA].len != K56ENC_N) {
            printf("  grid: MP at %d: TRN2B %d, data at sym %d\n", (int)mp_at,
                   (int)rec[K56TX_TRN2B].len, (int)out.data_sym);
            bad++;
        }
    }
    printf("  grid: MP at 2048 .. 2071: %d with the data off the grid of 24\n", bad);
    CHECK_EQ_I(bad, 0);
}

/* a renegotiation asked in mid-frame starts R2 at the frame's end */
static void test_reneg_frame(void)
{
    static k56tx_t t;
    drive_out out;
    uint32_t xs = 1;
    int k;
    k56tx_init(&t);
    drive(&t, 0x00, 12000, 3000, &xs, NULL, 0, &out);
    for (k = 0; k < 3; k++)
        (void)k56tx_tick(&t);
    k56tx_reneg(&t);
    for (k = 3; k < K56ENC_N; k++) {
        CHECK_EQ_I(k56tx_segment(&t), K56TX_DATA);
        (void)k56tx_tick(&t);
    }
    CHECK_EQ_I(k56tx_segment(&t), K56TX_R2);
    CHECK_EQ_I(k56tx_rate(&t), 0);
    printf("  renegotiation in mid-frame: R2 at the frame's end\n");
}

int main(void)
{
    printf("k56tx\n");
    test_mp_bits();
    test_oracle();
    test_trn1();
    test_pad_mags();
    test_sil2();
    test_grid();
    test_reneg_frame();
    test_reneg_bits();
    test_early_j();
    test_reneg_training();
    test_reneg_in_e();
    T_REPORT("k56tx");
}
