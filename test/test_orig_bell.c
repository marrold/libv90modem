/* modem/orig: the calling modem in Bell 103 and Bell 212A, and its choice
 * between them and V.8, against
 *   - the library's answering modem (modem/answer), which plays ANSam, then
 *     V.22's unscrambled ones, then (Bell 212A on) 2225 Hz from 4 s on, and
 *     listens for a Bell 103 caller's 1270 Hz all along;
 *   - two pre-V.8 Bell answerers written here, which play 2225 Hz and
 *     nothing else until a caller answers it: a Bell 212A answering modem
 *     (that also takes Bell 103 callers) and a Bell 103 one;
 * over a 2-wire line (sim/v32line) with loss, echoes, noise, a carrier
 * shift and a clock offset. Pseudo-random octets go both ways through V.14
 * start-stop framers for at least 10 s; every bit is compared. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/orig.h"
#include "../modem/answer.h"
#include "../modem/v14.h"
#include "../modem/tonedet.h"
#include "../modem/bell103_bpf.h"
#include "v32line.h"

#define FS 8000
#define TWO_PI 6.283185307179586
#define MS(ms) ((long)(ms) * FS / 1000)

static const unsigned ANS_ALL = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_BELL103 | ANS_MODE_BELL212A;

/* ---- the peers ---- */
enum { P_ANSWER, P_B212A, P_B103A };

/* a pre-V.8 Bell answering modem: 2225 Hz from the moment it answers; a
 * 212A one (b212 = 1) runs a V.22 answerer at 1200 bit/s underneath and
 * leaves the tone for its scrambled ones once the caller's 1200 bit/s has
 * been heard; either goes Bell 103 on 200 ms of the caller's 1270 Hz */
typedef struct {
    int       b212, fsk;              /* fsk: in Bell 103 */
    v22bis_t  v22;
    v21mod_t  mod; v21demod_t dem; arx_t uart; int fbit; long cd; int64_t thr;
    tonedet_t t1270;
    int16_t   bpf[128]; int bpos;
    int       connected, modulation;
} bellans_t;

static int peer_kind;
static long peer_n;
static answer_t A;
static bellans_t B;
static v14_t c_dte, a_dte;

static int16_t clip(double x) { long q = lrint(x); return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q); }

static void bellans_init(bellans_t *b, int b212)
{
    memset(b, 0, sizeof *b);
    b->b212 = b212;
    v22m_init(&b->v22, V22_ANSWERER, 0, 3000, v14_getbit, v14_putbit, &a_dte);
    v21mod_init(&b->mod, BELL103_A_MARK, BELL103_A_SPACE, FS, 300, 4243);
    v21demod_init(&b->dem, BELL103_O_MARK, BELL103_O_SPACE, FS, 300);
    arx_init(&b->uart, FS, 300);
    b->thr = v21demod_energy_of(&b->dem, 110);
    b->fbit = 1;
    tonedet_init(&b->t1270, (uint32_t)(((uint64_t)BELL103_O_MARK << 32) / FS));
}

static int16_t bellans_tick(bellans_t *b, int16_t rx)
{
    int32_t acc = 0; int16_t x; int k, done;
    b->bpf[b->bpos] = rx;
    for (k = 0; k < BELL103_BPF_N; k++) acc += (int32_t)b->bpf[(b->bpos - k) & 127] * bell103_bpf[k] >> 4;
    b->bpos = (b->bpos + 1) & 127;
    x = clip(acc >> 11);
    if (!b->fsk && (!b->b212 || b->v22.state == V22S_WAIT)) {
        (void)tonedet_tick(&b->t1270, x);
        if (b->t1270.blocks >= 20) b->fsk = 1;
    }
    if (b->fsk) {
        int level = v21demod_tick(&b->dem, x), c, i;
        if (b->dem.energy < b->thr) { level = 1; b->cd = 0; }
        else if (++b->cd >= MS(100) && !b->connected) { b->connected = 1; b->modulation = ANS_MOD_BELL103; }
        c = arx_tick(&b->uart, level);
        if (c >= 0 && b->connected) {
            v14_putbit(&a_dte, 0);
            for (i = 0; i < 8; i++) v14_putbit(&a_dte, (c >> i) & 1);
            v14_putbit(&a_dte, 1);
        }
        {
            int16_t out = v21mod_tick(&b->mod, b->fbit, &done);
            if (done) b->fbit = b->connected ? (v14_getbit(&a_dte) & 1) : 1;
            return out;
        }
    }
    if (b->b212) {
        int16_t out = v22m_tick(&b->v22, rx);
        if (b->v22.tx_ready && b->v22.rx_ready && !b->connected) { b->connected = 1; b->modulation = ANS_MOD_BELL212A; }
        if (b->v22.state != V22S_WAIT) return out;
    }
    /* the answer tone, which is also a Bell 103 answerer's mark */
    return v21mod_tick(&b->mod, 1, &done);
}

static double peer(void *u, double rx)
{
    int16_t r = clip(rx);
    (void)u;
    peer_n++;
    if (peer_kind < 0) return 0;      /* nobody there */
    if (peer_kind == P_ANSWER) return answer_tick(&A, r);
    return bellans_tick(&B, r);
}

static int peer_connected(void) { return peer_kind < 0 ? 0 : peer_kind == P_ANSWER ? A.connected : B.connected; }
static int peer_modulation(void) { return peer_kind == P_ANSWER ? A.modulation : B.modulation; }

/* ---- the lines ---- */
typedef struct { const char *name; double loss, near, far, cnear, cfar, noise, foff, ppm; int delay; } line_t;
static const line_t LINES[] = {
    { "clean",                                     0, 300, 300, 300, 300, -300, 0,   0,  40 },
    { "10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm", 10, 12, 15, 12, 18,  -45, 1,  50, 160 },
    { "20 dB, -52 dBm0 noise, -3 Hz, -100 ppm",    20, 20, 20, 20, 20,  -52, -3, -100, 300 },
    { "6 dB, loud echoes (6 dB), 7 Hz, 100 ppm",    6,  6,  8,  6,  8,  -50, 7, 100,  80 },
};
#define NLINES ((int)(sizeof LINES / sizeof LINES[0]))

static v32line_t L;
static orig_t O;
static long now;
static int16_t o_rx;

static void start(int kind, unsigned our_modes, unsigned their_modes, const line_t *ln)
{
    v32line_cfg lc; orig_cfg oc; answer_cfg ac;
    memset(&lc, 0, sizeof lc);
    lc.mulaw = 1; lc.seed = 3; lc.delay = ln->delay; lc.loss_db = ln->loss;
    lc.near_db = ln->near; lc.far_db = ln->far; lc.c_near_db = ln->cnear; lc.c_far_db = ln->cfar;
    lc.noise_a_dbm0 = lc.noise_c_dbm0 = ln->noise;
    lc.freq_offset = ln->foff; lc.ppm = ln->ppm;
    v32line_init(&L, &lc);
    v14_init(&c_dte); v14_init(&a_dte);
    memset(&oc, 0, sizeof oc);
    oc.level = 3000; oc.modes = our_modes; oc.getbit = v14_getbit; oc.putbit = v14_putbit; oc.user = &c_dte;
    orig_init(&O, &oc);
    peer_kind = kind; peer_n = 0;
    if (kind == P_ANSWER) {
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = their_modes; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &a_dte;
        answer_init(&A, &ac);
    } else bellans_init(&B, kind == P_B212A);
    now = 0; o_rx = 0;
}

static void run(long samples)
{
    while (samples-- > 0) { o_rx = v32line_tick(&L, orig_tick(&O, o_rx), peer, NULL); now++; }
}

/* runs until both ends are connected (or the caller failed); the time, or -1 */
static double to_data(long max_ms)
{
    long end = now + MS(max_ms);
    while (now < end) {
        run(8);
        if (O.phase == ORIG_FAILED) return -1;
        if (O.connected && peer_connected()) return now / (double)FS;
    }
    return -1;
}

/* ---- data both ways, every bit compared ---- */
typedef struct { uint32_t s; } prbs_t;
static uint8_t prbs_next(prbs_t *p) { p->s = p->s * 1103515245u + 12345u; return (uint8_t)(p->s >> 23); }
static int popcount8(unsigned v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

typedef struct { long octets, bit_errors; } dir_t;

static void feed(v14_t *d, prbs_t *p)
{
    while (v14_tx_pending(d) < 32) { uint8_t c = prbs_next(p); v14_write(d, &c, 1); }
}

static void check_rx(v14_t *d, prbs_t *p, dir_t *r)
{
    uint8_t buf[64]; int n, k;
    while ((n = v14_read(d, buf, (int)sizeof buf)) > 0)
        for (k = 0; k < n; k++) { r->bit_errors += popcount8((unsigned)(buf[k] ^ prbs_next(p))); r->octets++; }
}

/* `ms` of data; up / down: what the answerer and we received */
static void exchange(long ms, dir_t *up, dir_t *down)
{
    prbs_t tu = { 11 }, ru = { 11 }, td = { 977 }, rd = { 977 };
    long k;
    memset(up, 0, sizeof *up); memset(down, 0, sizeof *down);
    run(MS(500));                     /* both ends settle before the first octet */
    check_rx(&c_dte, &rd, down); check_rx(&a_dte, &ru, up);    /* nothing yet: flush stray octets */
    memset(up, 0, sizeof *up); memset(down, 0, sizeof *down);
    rd.s = td.s; ru.s = tu.s;
    for (k = 0; k < ms / 10; k++) {
        feed(&c_dte, &tu); feed(&a_dte, &td);
        run(MS(10));
        check_rx(&c_dte, &rd, down); check_rx(&a_dte, &ru, up);
    }
}

typedef struct { const char *what; int kind; unsigned ours, theirs; int want_mod, want_peer_mod, want_v8; int want_bps; } call_t;

static int call(const call_t *c, const line_t *ln, int data)
{
    double t; dir_t up, down; int ok;
    start(c->kind, c->ours, c->theirs, ln);
    t = to_data(40000);
    printf("  %-44s %-46s ", c->what, ln->name);
    if (t < 0) {
        printf("no call: phase %d end %d, ours mod %d, theirs mod %d\n", O.phase, O.end, O.modulation, peer_modulation());
        CHECK(t > 0);
        return 0;
    }
    CHECK_EQ_I(O.modulation, c->want_mod); CHECK_EQ_I(peer_modulation(), c->want_peer_mod);
    CHECK_EQ_I(O.used_v8, c->want_v8); CHECK_EQ_I(O.bitrate, c->want_bps);
    if (!data) { printf("up %5.2f s, mod %d / %d, %d bit/s, V.8 %d\n", t, O.modulation, peer_modulation(), O.bitrate, O.used_v8); return 1; }
    exchange(10500, &up, &down);
    printf("up %5.2f s, mod %d / %d, %4d bit/s; up %ld bits %ld errors, down %ld bits %ld errors\n", t, O.modulation,
           peer_modulation(), O.bitrate, up.octets * 8, up.bit_errors, down.octets * 8, down.bit_errors);
    /* 10 s at 10 line bits per octet, less the octets still in flight */
    ok = up.bit_errors == 0 && down.bit_errors == 0 && up.octets >= O.bitrate - 8 && down.octets >= O.bitrate - 8;
    CHECK(up.bit_errors == 0); CHECK(down.bit_errors == 0);
    CHECK(up.octets >= O.bitrate - 8); CHECK(down.octets >= O.bitrate - 8);
    return ok;
}

/* the cases (test/test.h, t_case_arg): each call on each line, then each
 * fallback call, then each of the three calls that must not connect */
int main(int argc, char **argv)
{
    static const call_t calls[] = {
        { "Bell 103, to our answerer",            P_ANSWER, ORIG_MODE_BELL103, ANS_ALL,
          ORIG_MOD_BELL103, ANS_MOD_BELL103, 0, 300 },
        { "Bell 212A (and 103), to our answerer", P_ANSWER, ORIG_MODE_BELL212A | ORIG_MODE_BELL103, ANS_ALL,
          ORIG_MOD_BELL212A, ANS_MOD_BELL212A, 0, 1200 },
        { "Bell 212A only, to our answerer",      P_ANSWER, ORIG_MODE_BELL212A, ANS_ALL,
          ORIG_MOD_BELL212A, ANS_MOD_BELL212A, 0, 1200 },
    };
    static const call_t fallbacks[] = {
        { "212A and 103, answerer 103 only",      P_ANSWER, ORIG_MODE_BELL212A | ORIG_MODE_BELL103, V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_BELL103,
          ORIG_MOD_BELL103, ANS_MOD_BELL103, 0, 300 },
        { "V.22, V.21 and Bell, answerer with V.8", P_ANSWER, V8_MOD_V22 | V8_MOD_V21 | ORIG_MODE_BELL212A | ORIG_MODE_BELL103, ANS_ALL,
          ORIG_MOD_V22BIS, ANS_MOD_V22BIS, 1, 2400 },
        { "212A and 103, 2225 Hz-only 212A answerer", P_B212A, ORIG_MODE_BELL212A | ORIG_MODE_BELL103, 0,
          ORIG_MOD_BELL212A, ANS_MOD_BELL212A, 0, 1200 },
        { "V.22 and Bell, 2225 Hz-only 212A answerer", P_B212A, V8_MOD_V22 | V8_MOD_V21 | ORIG_MODE_BELL212A | ORIG_MODE_BELL103, 0,
          ORIG_MOD_BELL212A, ANS_MOD_BELL212A, 0, 1200 },
        { "103 only, 2225 Hz-only 212A answerer",  P_B212A, ORIG_MODE_BELL103, 0,
          ORIG_MOD_BELL103, ANS_MOD_BELL103, 0, 300 },
        { "212A and 103, 2225 Hz-only 103 answerer", P_B103A, ORIG_MODE_BELL212A | ORIG_MODE_BELL103, 0,
          ORIG_MOD_BELL103, ANS_MOD_BELL103, 0, 300 },
        { "V.22 and Bell, 2225 Hz-only 103 answerer", P_B103A, V8_MOD_V22 | V8_MOD_V21 | ORIG_MODE_BELL212A | ORIG_MODE_BELL103, 0,
          ORIG_MOD_BELL103, ANS_MOD_BELL103, 0, 300 },
    };
    enum { NC = sizeof calls / sizeof calls[0], NF = sizeof fallbacks / sizeof fallbacks[0],
           C_FALL = NC * NLINES, C_NONE = C_FALL + NF * 2, NCASES = C_NONE + 3 };
    int i, l;
    const int only = t_case_arg(argc, argv, NCASES);

    for (i = 0; i < NC; i++)
        for (l = 0; l < NLINES; l++) {
            if (!T_CASE(only, i * NLINES + l + 1)) continue;
            if (i == 0 && l == 0) printf("Bell calls, 10.5 s of data each way (bits: data bits compared)\n");
            call(&calls[i], &LINES[l], 1);
        }

    for (i = 0; i < NF; i++)
        for (l = 0; l < 2; l++) {
            if (!T_CASE(only, C_FALL + i * 2 + l + 1)) continue;
            if (i == 0 && l == 0) printf("fallbacks\n");
            call(&fallbacks[i], &LINES[l], fallbacks[i].want_mod != ORIG_MOD_V22BIS || l == 0);
        }

    if (T_CASE(only, C_NONE + 1)) {
        printf("no call\n");
        /* 212A only, the answerer 103 only: it never plays 2225 Hz, and we never send 1270 Hz */
        start(P_ANSWER, ORIG_MODE_BELL212A, V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_BELL103, &LINES[1]);
        run(MS(45000));
        printf("  212A only, answerer 103 only: phase %d, end %d; answerer phase %d, modulation %d\n", O.phase, O.end, A.phase, A.modulation);
        CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_NO_CARRIER); CHECK_EQ_I(A.modulation, ANS_MOD_NONE);
    }
    if (T_CASE(only, C_NONE + 2)) {
        /* Bell not offered: the 2225 Hz-only answerer is no answer tone to us (as before) */
        start(P_B212A, V8_MOD_V22 | V8_MOD_V21, 0, &LINES[1]);
        run(MS(12000));
        printf("  no Bell offered, 2225 Hz-only answerer: phase %d, end %d\n", O.phase, O.end);
        CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_NO_TONE);
    }
    if (T_CASE(only, C_NONE + 3)) {
        /* Bell offered, no answer at all */
        start(P_B103A, ORIG_MODE_BELL212A | ORIG_MODE_BELL103, 0, &LINES[1]);
        peer_kind = -1;
        run(MS(12000));
        printf("  Bell offered, nobody answers: phase %d, end %d\n", O.phase, O.end);
        CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_NO_TONE);
    }

    T_REPORT("orig_bell");
}
