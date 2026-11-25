/* End-to-end: the answering modem against a calling-modem peer through the
 * channel simulator; characters both ways once connected. */
#include <math.h>
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../modem/ans_scheme.h"   /* ans_enter: a phase put in place by hand */
#include "../modem/v14.h"
#include "../modem/tonedet.h"
#include "../sim/callpeer.h"
#include "../sim/channel.h"
#include "../sim/g711.h"

#define FS 8000
/* RMS on the 16-bit scale (0 dBm0 = 15770) */
#define DBM0_18 1981

typedef struct { int connected_at, modulation, bitrate, peer_rate, phase; char got_a[64], got_c[64]; int na, nc; } outcome;

static void run(int use_v8, unsigned caller_modes, int caller2400, unsigned our_modes, int seconds, int level, outcome *o)
{
    static const double one[1] = {1.0};
    static const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    channel_cfg cfg; channel_t ch;
    static answer_t a; static callpeer_t c; static v14_t dte;
    answer_cfg ac;
    int16_t atx = 0, ctx = 0;
    int n, sent = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = -55.0; cfg.us_noise_dbfs = -55.0;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
    channel_init(&ch, &cfg);
    v14_init(&dte);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = our_modes; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
    answer_init(&a, &ac);
    callpeer_init(&c, use_v8, caller_modes, caller2400, level);
    memset(o, 0, sizeof *o); o->connected_at = -1;

    for (n = 0; n < seconds * FS; n++) {
        double client_rx; uint8_t srv; int16_t t;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
        ctx = callpeer_tick(&c, (int16_t)lround(client_rx));
        atx = t;
        if (o->connected_at < 0 && a.connected && c.connected) o->connected_at = n;
        if (o->connected_at >= 0 && !sent && n > o->connected_at + FS / 4) {
            v14_write(&c.dte, (const uint8_t *)msg_c, (int)strlen(msg_c));
            v14_write(&dte, (const uint8_t *)msg_a, (int)strlen(msg_a));
            sent = 1;
        }
        if (sent) {
            o->na += v14_read(&dte, (uint8_t *)o->got_a + o->na, 63 - o->na);
            o->nc += v14_read(&c.dte, (uint8_t *)o->got_c + o->nc, 63 - o->nc);
        }
    }
    o->modulation = a.modulation; o->bitrate = a.bitrate; o->peer_rate = c.bitrate; o->phase = a.phase;
    if (o->connected_at < 0)
        printf("  not connected: answer phase %d (v8 state %d result %d, v22 state %d) peer phase %d\n",
               a.phase, a.v8.state, a.v8.result, a.v22.state, c.phase);
    /* strings for comparison */
    (void)msg_a; (void)msg_c;
}

/* A Bell 103 calling modem, as plain as the real ones: once it has heard
 * something in its receive band (2025 / 2225 Hz) for 150 ms it sends its
 * mark, 1270 Hz, and from then on start-stop characters at 300 bit/s. It
 * knows nothing of V.8 and takes any answer tone near its band for carrier. */
typedef struct { v21mod_t mod; v21demod_t dem; arx_t uart; v14_t dte; int fbit, cd, on; int64_t thr; } b103_t;

static void b103_init(b103_t *b)
{
    memset(b, 0, sizeof *b);
    v21mod_init(&b->mod, BELL103_O_MARK, BELL103_O_SPACE, 8000, 300, 4243);
    v21demod_init(&b->dem, BELL103_A_MARK, BELL103_A_SPACE, 8000, 300);
    arx_init(&b->uart, 8000, 300);
    v14_init(&b->dte);
    b->thr = v21demod_energy_of(&b->dem, 1200); b->fbit = 1;
}

static int16_t b103_tick(b103_t *b, int16_t rx)
{
    int level = v21demod_tick(&b->dem, rx), heard = b->dem.energy >= b->thr, c, i, done;
    int16_t out;
    b->cd = heard ? b->cd + 1 : 0;
    if (b->cd > 150 * 8) b->on = 1;
    if (!b->on) return 0;
    c = arx_tick(&b->uart, heard ? level : 1);
    if (c >= 0) {
        v14_putbit(&b->dte, 0);
        for (i = 0; i < 8; i++) v14_putbit(&b->dte, (c >> i) & 1);
        v14_putbit(&b->dte, 1);
    }
    out = v21mod_tick(&b->mod, b->fbit, &done);
    if (done) b->fbit = v14_getbit(&b->dte) & 1;
    return out;
}

static void run_bell103(unsigned our_modes, int seconds, outcome *o)
{
    static const double one[1] = {1.0};
    static const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    channel_cfg cfg; channel_t ch;
    static answer_t a; static b103_t c; static v14_t dte;
    answer_cfg ac;
    int16_t atx = 0, ctx = 0;
    int n, sent = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = -55.0; cfg.us_noise_dbfs = -55.0;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
    channel_init(&ch, &cfg);
    v14_init(&dte);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = our_modes; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
    answer_init(&a, &ac);
    b103_init(&c);
    memset(o, 0, sizeof *o); o->connected_at = -1;
    for (n = 0; n < seconds * FS; n++) {
        double client_rx; uint8_t srv; int16_t t;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
        ctx = b103_tick(&c, (int16_t)lround(client_rx));
        atx = t;
        if (o->connected_at < 0 && a.connected) o->connected_at = n;
        if (o->connected_at >= 0 && !sent && n > o->connected_at + FS / 2) {
            v14_write(&c.dte, (const uint8_t *)msg_c, (int)strlen(msg_c));
            v14_write(&dte, (const uint8_t *)msg_a, (int)strlen(msg_a));
            sent = 1;
        }
        if (sent) {
            o->na += v14_read(&dte, (uint8_t *)o->got_a + o->na, 63 - o->na);
            o->nc += v14_read(&c.dte, (uint8_t *)o->got_c + o->nc, 63 - o->nc);
        }
    }
    o->modulation = a.modulation; o->bitrate = a.bitrate; o->phase = a.phase;
}

/* A Bell 212A calling modem that is strict about it: nothing but 2225 Hz
 * (three 100 ms blocks in a row with more than half of their power within
 * some 10 Hz of it) is an answering modem to it. Neither the 2100 Hz answer
 * tone nor the 2250 Hz of V.22's unscrambled ones passes. Then scrambled
 * ones at 1200 bit/s on the low carrier, and data once the answering side
 * sends the same on the high one. */
typedef struct { v22bis_t m; v14_t dte; int on, run, n; double re, im, pw; } b212_t;

static int16_t b212_tick(b212_t *b, int16_t rx)
{
    if (!b->on) {
        double w = 2.0 * 3.14159265358979323846 * 2225.0 * b->n / 8000.0;
        b->re += rx * cos(w); b->im += rx * sin(w); b->pw += (double)rx * rx;
        if (++b->n < 800) return 0;
        b->run = (b->pw > 800.0 * 400 * 400 && (b->re * b->re + b->im * b->im) * 4 > b->pw * 800) ? b->run + 1 : 0;
        b->n = 0; b->re = b->im = b->pw = 0;
        if (b->run < 3) return 0;
        v22m_init(&b->m, V22_CALLER, 0, 3000, v14_getbit, v14_putbit, &b->dte);
        v22tx_set_mode(&b->m.tx, V22_TX_SB1_1200);
        b->m.state = V22S_TRAIN;
        b->on = 1;
    }
    return v22m_tick(&b->m, rx);
}

static void run_bell212(unsigned our_modes, int seconds, outcome *o)
{
    static const double one[1] = {1.0};
    static const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    channel_cfg cfg; channel_t ch;
    static answer_t a; static b212_t c; static v14_t dte;
    answer_cfg ac;
    int16_t atx = 0, ctx = 0;
    int n, sent = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = -55.0; cfg.us_noise_dbfs = -55.0;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
    channel_init(&ch, &cfg);
    v14_init(&dte);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = our_modes; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
    answer_init(&a, &ac);
    memset(&c, 0, sizeof c); v14_init(&c.dte);
    memset(o, 0, sizeof *o); o->connected_at = -1;
    for (n = 0; n < seconds * FS; n++) {
        double client_rx; uint8_t srv; int16_t t;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
        ctx = b212_tick(&c, (int16_t)lround(client_rx));
        atx = t;
        if (o->connected_at < 0 && a.connected && c.on && c.m.tx_ready && c.m.rx_ready) o->connected_at = n;
        if (o->connected_at >= 0 && !sent && n > o->connected_at + FS / 4) {
            v14_write(&c.dte, (const uint8_t *)msg_c, (int)strlen(msg_c));
            v14_write(&dte, (const uint8_t *)msg_a, (int)strlen(msg_a));
            sent = 1;
        }
        if (sent) {
            o->na += v14_read(&dte, (uint8_t *)o->got_a + o->na, 63 - o->na);
            o->nc += v14_read(&c.dte, (uint8_t *)o->got_c + o->nc, 63 - o->nc);
        }
    }
    o->modulation = a.modulation; o->bitrate = a.bitrate; o->phase = a.phase;
}

/* A V.21 calling modem without V.8 (V.25 5, V.21 7 a)): once it has
 * recognised the answer tone it answers with its mark, 980 Hz on channel
 * No. 1, and waits for ours, 1650 Hz on channel No. 2, before data. As the
 * three recorded callers (2026-11-23) it may send that mark during ANSam
 * (mark_in_tone_ms after the tone began) or, after_tone_ms > 0, only once
 * the tone has been gone that long. Level: the RMS of its signal. */
typedef struct {
    tonedet_t ans; v21mod_t mod; v21demod_t dem; arx_t uart; v14_t dte;
    int32_t n, tone_at, tone_last, on_at; int mark_in_tone_ms, after_tone_ms, fbit, cd, on, carrier; int64_t thr;
} v21c_t;

#define ANS_2100 ((uint32_t)(((uint64_t)2100 << 32) / 8000))

static void v21c_init(v21c_t *v, int mark_in_tone_ms, int after_tone_ms, int level)
{
    memset(v, 0, sizeof *v);
    tonedet_init(&v->ans, ANS_2100);
    v21mod_init(&v->mod, V21L_MARK, V21L_SPACE, 8000, 300, (int16_t)(level * 181 / 128));
    v21demod_init(&v->dem, V21H_MARK, V21H_SPACE, 8000, 300);
    arx_init(&v->uart, 8000, 300);
    v14_init(&v->dte);
    v->thr = v21demod_energy_of(&v->dem, 1200); v->fbit = 1;
    v->mark_in_tone_ms = mark_in_tone_ms; v->after_tone_ms = after_tone_ms;
    v->tone_at = v->tone_last = v->on_at = -1;
}

static int16_t v21c_tick(v21c_t *v, int16_t rx)
{
    int level, heard, c, i, done;
    int16_t out;
    v->n++;
    if (!v->on) {
        if (tonedet_tick(&v->ans, rx)) { if (v->tone_at < 0) v->tone_at = v->n; v->tone_last = v->n; }
        if (v->tone_at >= 0 && (v->after_tone_ms > 0 ? v->n - v->tone_last >= v->after_tone_ms * 8
                                                      : v->n - v->tone_at >= v->mark_in_tone_ms * 8)) {
            v->on = 1; v->on_at = v->n;
        }
        return 0;
    }
    level = v21demod_tick(&v->dem, rx);
    heard = v->dem.energy >= v->thr;
    v->cd = heard ? v->cd + 1 : 0;
    if (v->cd > 150 * 8) v->carrier = 1;
    if (v->carrier) {
        c = arx_tick(&v->uart, heard ? level : 1);
        if (c >= 0) {
            v14_putbit(&v->dte, 0);
            for (i = 0; i < 8; i++) v14_putbit(&v->dte, (c >> i) & 1);
            v14_putbit(&v->dte, 1);
        }
    }
    out = v21mod_tick(&v->mod, v->fbit, &done);
    if (done) v->fbit = v->carrier ? (v14_getbit(&v->dte) & 1) : 1;
    return out;
}

/* o->connected_at; *mark_heard_at: where the caller's mark is first on the
 * line in our V.22 wait (the later of its start and that wait's) */
static void run_v21(unsigned our_modes, int mark_in_tone_ms, int after_tone_ms, int level, int seconds, outcome *o, int *mark_heard_at)
{
    static const double one[1] = {1.0};
    static const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    channel_cfg cfg; channel_t ch;
    static answer_t a; static v21c_t c; static v14_t dte;
    answer_cfg ac;
    int16_t atx = 0, ctx = 0;
    int n, sent = 0, v22_at = -1;

    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = -55.0; cfg.us_noise_dbfs = -55.0;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
    channel_init(&ch, &cfg);
    v14_init(&dte);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = our_modes; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
    answer_init(&a, &ac);
    v21c_init(&c, mark_in_tone_ms, after_tone_ms, level);
    memset(o, 0, sizeof *o); o->connected_at = -1;
    for (n = 0; n < seconds * FS; n++) {
        double client_rx; uint8_t srv; int16_t t;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
        ctx = v21c_tick(&c, (int16_t)lround(client_rx));
        atx = t;
        if (v22_at < 0 && a.phase == ANS_V22) v22_at = n;
        if (o->connected_at < 0 && a.connected && c.carrier) o->connected_at = n;
        if (o->connected_at >= 0 && !sent && n > o->connected_at + FS / 2) {
            v14_write(&c.dte, (const uint8_t *)msg_c, (int)strlen(msg_c));
            v14_write(&dte, (const uint8_t *)msg_a, (int)strlen(msg_a));
            sent = 1;
        }
        if (sent) {
            o->na += v14_read(&dte, (uint8_t *)o->got_a + o->na, 63 - o->na);
            o->nc += v14_read(&c.dte, (uint8_t *)o->got_c + o->nc, 63 - o->nc);
        }
    }
    o->modulation = a.modulation; o->bitrate = a.bitrate; o->phase = a.phase;
    *mark_heard_at = c.on_at < 0 ? -1 : c.on_at > v22_at ? c.on_at : v22_at;
}

int main(void)
{
    outcome o;
    const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    unsigned ours = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_BELL103 | ANS_MODE_BELL212A;

    /* 1. V.8 caller offering V.22bis: 2400 bit/s */
    run(1, V8_MOD_V22 | V8_MOD_V21, 1, ours, 14, 3000, &o);
    printf("  answer: V.8 + V.22bis connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 0);
    CHECK_EQ_I(o.modulation, ANS_MOD_V22BIS); CHECK_EQ_I(o.bitrate, 2400); CHECK_EQ_I(o.peer_rate, 2400);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 2. V.8 caller offering only V.21: 300 bit/s FSK */
    run(1, V8_MOD_V21, 0, ours, 14, 3000, &o);
    printf("  answer: V.8 + V.21 connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 0);
    CHECK_EQ_I(o.modulation, ANS_MOD_V21); CHECK_EQ_I(o.bitrate, 300);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 3. pre-V.8 V.22bis caller: ANSam runs out, then the legacy answer sequence */
    run(0, 0, 1, ours, 16, 3000, &o);
    printf("  answer: legacy V.22bis connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 5 * FS);
    CHECK_EQ_I(o.modulation, ANS_MOD_V22BIS); CHECK_EQ_I(o.bitrate, 2400);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 4. pre-V.8 V.22 (1200) caller */
    run(0, 0, 0, ours, 16, 3000, &o);
    CHECK(o.connected_at > 5 * FS);
    CHECK_EQ_I(o.modulation, ANS_MOD_V22); CHECK_EQ_I(o.bitrate, 1200);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 5. V.8 caller with nothing in common (V.32 only): clean failure */
    run(1, V8_MOD_V32, 0, ours, 14, 3000, &o);
    CHECK(o.connected_at < 0);
    CHECK_EQ_I(o.phase, ANS_FAILED);

    /* 6. a Bell 103 caller: no V.8, and its mark is 1270 Hz where a V.21
     * caller's is 980 Hz. We answer on 2225 / 2025 Hz, 300 bit/s. */
    run_bell103(ours, 22, &o);
    printf("  answer: Bell 103 connected at %.2f s, \"%s\" / \"%s\"\n", o.connected_at / 8000.0, o.got_a, o.got_c);
    CHECK(o.connected_at > 0);
    CHECK_EQ_I(o.modulation, ANS_MOD_BELL103); CHECK_EQ_I(o.bitrate, 300); CHECK_EQ_I(o.phase, ANS_V21);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* the same on a port without V.22: the wait for the caller is V.21's */
    run_bell103(V8_MOD_V21 | ANS_MODE_BELL103, 22, &o);
    printf("  answer: Bell 103 without V.22 connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 0);
    CHECK_EQ_I(o.modulation, ANS_MOD_BELL103); CHECK_EQ_I(o.bitrate, 300);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 7. the same caller with Bell 103 not offered: never that modulation */
    run_bell103(V8_MOD_V22 | V8_MOD_V21, 45, &o);
    CHECK(o.modulation != ANS_MOD_BELL103);
    CHECK(o.na == 0);

    /* 8. a Bell 212A caller that wants 2225 Hz: it gets that after the V.22
     * answer sequence has gone unanswered, and then 1200 bit/s as in V.22 */
    run_bell212(ours, 20, &o);
    printf("  answer: Bell 212A connected at %.2f s, \"%s\" / \"%s\"\n", o.connected_at / 8000.0, o.got_a, o.got_c);
    CHECK(o.connected_at > 0);
    CHECK_EQ_I(o.modulation, ANS_MOD_BELL212A); CHECK_EQ_I(o.bitrate, 1200); CHECK_EQ_I(o.phase, ANS_V22);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 9. not offered: that caller hears no 2225 Hz from us and stays silent */
    run_bell212(V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_BELL103, 45, &o);
    CHECK(o.connected_at < 0);
    CHECK_EQ_I(o.modulation, ANS_MOD_NONE);

    /* 10. a V.21 caller without V.8 (three ISP calls, 2026-11-23): it
     * answers ANSam with its 980 Hz mark, sends no CM and waits for our
     * 1650 Hz. Heard in the V.22 wait, V.21 within half a second of it;
     * before, the mark held V.22's carrier detect on and the caller heard
     * V.22 and the 2225 Hz tone until it gave up. First as recorded (the
     * mark from 950 ms into ANSam), then a caller that waits for the end of
     * the tone, at -18 dBm0, the level of the recorded ones. */
    {
        int mark_at;
        run_v21(ours, 950, 0, 3000, 14, &o, &mark_at);
        printf("  answer: V.21 caller (mark during ANSam) connected at %.2f s, %.2f s after its mark in the V.22 wait, \"%s\" / \"%s\"\n",
               o.connected_at / 8000.0, (o.connected_at - mark_at) / 8000.0, o.got_a, o.got_c);
        CHECK(o.connected_at > 0); CHECK(mark_at > 0);
        CHECK(o.connected_at - mark_at <= FS / 2);
        CHECK_EQ_I(o.modulation, ANS_MOD_V21); CHECK_EQ_I(o.bitrate, 300); CHECK_EQ_I(o.phase, ANS_V21);
        CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

        run_v21(ours, 0, 1000, DBM0_18, 16, &o, &mark_at);
        printf("  answer: V.21 caller (mark after the tone, -18 dBm0) connected at %.2f s, %.2f s after its mark\n",
               o.connected_at / 8000.0, (o.connected_at - mark_at) / 8000.0);
        CHECK(o.connected_at > 0); CHECK(mark_at > 0);
        CHECK(o.connected_at - mark_at <= FS / 2);
        CHECK_EQ_I(o.modulation, ANS_MOD_V21); CHECK_EQ_I(o.bitrate, 300);
        CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

        /* 11. V.21 not offered: that caller is never answered in V.21 */
        run_v21(ours & ~V8_MOD_V21, 950, 0, 3000, 40, &o, &mark_at);
        CHECK(o.connected_at < 0);
        CHECK_EQ_I(o.modulation, ANS_MOD_NONE); CHECK_EQ_I(o.phase, ANS_FAILED);
    }

    /* 12. V.22bis and V.22 callers without V.8 at -18 dBm0: as before, the
     * 980 Hz watch takes neither for a V.21 caller */
    run(0, 0, 1, ours, 16, DBM0_18, &o);
    printf("  answer: legacy V.22bis at -18 dBm0 connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 5 * FS);
    CHECK_EQ_I(o.modulation, ANS_MOD_V22BIS); CHECK_EQ_I(o.bitrate, 2400);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));
    run(0, 0, 0, ours, 16, DBM0_18, &o);
    printf("  answer: legacy V.22 at -18 dBm0 connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 5 * FS);
    CHECK_EQ_I(o.modulation, ANS_MOD_V22); CHECK_EQ_I(o.bitrate, 1200);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 13. nobody there and white noise at -30 dBm0 (above V.22's carrier
     * threshold): no V.21 caller is heard in it. V.22 waits to its end. */
    {
        static const double one[1] = {1.0};
        channel_cfg cfg; channel_t ch; static answer_t a; static v14_t dte; answer_cfg ac;
        int16_t atx = 0; int n, ever = 0, v21 = 0;
        memset(&cfg, 0, sizeof cfg);
        cfg.ds_noise_dbfs = -60.0; cfg.us_noise_dbfs = -30.0 - 3.17;
        cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
        cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 5;
        channel_init(&ch, &cfg);
        v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = ours; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);
        for (n = 0; n < 45 * FS && a.phase != ANS_FAILED; n++) {
            double crx; uint8_t srv;
            channel_tick(&ch, linear_to_ulaw(atx), 0.0, &crx, &srv);
            atx = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            if (a.connected) ever = 1;
            if (a.phase == ANS_V21) v21 = 1;
        }
        printf("  answer: noise at -30 dBm0: gave up after %.1f s, V.21 tried %d\n", n / 8000.0, v21);
        CHECK(!ever); CHECK(!v21);
        CHECK_EQ_I(a.phase, ANS_FAILED);
    }

    /* nobody there, but a hybrid that returns a fifth of our own signal: the
     * V.21 fall-back must not take the echo of its 1650 Hz channel for a
     * caller. No carrier, ever. */
    {
        static const double one[1] = {1.0};
        channel_cfg cfg; channel_t ch; static answer_t a; static v14_t dte; answer_cfg ac;
        int16_t atx = 0; int n, ever = 0;
        memset(&cfg, 0, sizeof cfg);
        cfg.ds_noise_dbfs = -60.0; cfg.us_noise_dbfs = -60.0;
        cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
        cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 3;
        channel_init(&ch, &cfg);
        v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = ours; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);
        for (n = 0; n < 45 * FS && a.phase != ANS_FAILED; n++) {
            double crx; uint8_t srv;
            channel_tick(&ch, linear_to_ulaw(atx), 0.0, &crx, &srv);
            atx = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            if (a.connected) ever = 1;
        }
        printf("  answer: silent line with echo: gave up after %.1f s\n", n / 8000.0);
        CHECK(!ever);
        CHECK_EQ_I(a.phase, ANS_FAILED);
    }

    /* V.22bis retrain (6.4) under the answering modem: started by the caller in
     * mid-call, the call stays up (circuits 107 / 109 ON) and characters pass
     * again afterwards; then one of ours that the caller never answers (it has
     * gone deaf but still sends carrier): the call is given up, not left
     * "connected" with a receiver that is clamped for good. */
    {
        static const double one[1] = {1.0};
        static const char *m1 = "before the retrain", *m2 = "and after it";
        channel_cfg cfg; channel_t ch; static answer_t a; static callpeer_t c; static v14_t dte; answer_cfg ac;
        int16_t atx = 0, ctx = 0; int n, up = -1, dropped = 0, t_rt = -1, t_dead = -1, t_fail = -1, na = 0, rt_seen = 0;
        char got[64];
        memset(&cfg, 0, sizeof cfg); memset(got, 0, sizeof got);
        cfg.ds_noise_dbfs = -55.0; cfg.us_noise_dbfs = -55.0;
        cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
        cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
        channel_init(&ch, &cfg);
        v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = ours; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);
        callpeer_init(&c, 1, V8_MOD_V22 | V8_MOD_V21, 1, 3000);
        for (n = 0; n < 40 * FS; n++) {
            double client_rx; uint8_t srv; int16_t t;
            channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
            if (t_dead >= 0 && n >= t_dead) client_rx = 0.0;
            t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            ctx = callpeer_tick(&c, (int16_t)lround(client_rx));
            atx = t;
            if (up < 0 && a.connected && c.connected) up = n;
            if (up < 0) continue;
            if (n == up + FS / 4) v14_write(&c.dte, (const uint8_t *)m1, (int)strlen(m1));
            if (n == up + 2 * FS) { v22m_retrain(&c.v22); t_rt = n; }
            if (t_rt >= 0 && n < t_rt + 2 * FS && a.v22.retraining) rt_seen = 1;
            if (n == up + 4 * FS) v14_write(&c.dte, (const uint8_t *)m2, (int)strlen(m2));
            if (n < up + 6 * FS && !a.connected) dropped = 1;
            na += v14_read(&dte, (uint8_t *)got + na, 63 - na);
            if (n == up + 6 * FS) { t_dead = n; }
            if (n == up + 6 * FS + 400) v22m_retrain(&a.v22);
            if (t_fail < 0 && t_dead >= 0 && a.phase == ANS_FAILED) t_fail = n;
        }
        printf("  answer: V.22bis retrain by the caller: seen %d, completed %d, call dropped %d, text \"%s\"\n", rt_seen, a.v22.retrains, dropped, got);
        CHECK(up > 0); CHECK(rt_seen); CHECK_EQ_I(a.v22.retrains, 1); CHECK_EQ_I(dropped, 0);
        CHECK(!strcmp(got, "before the retrainand after it"));
        printf("  answer: our retrain never answered: call given up %.1f s after it was asked for\n", t_fail < 0 ? -1.0 : (t_fail - t_dead - 400) / 8000.0);
        CHECK(t_fail > 0); CHECK_EQ_I(a.phase, ANS_FAILED); CHECK(!a.connected);
    }

    /* The backstop of the start-up phases (spec D5): no phase before data
     * mode lasts longer than its budget, whatever its engine does. Each
     * engine is put in place by hand and its clock moved on. */
    {
        static answer_t a; static v14_t dte; answer_cfg ac; v34ans_cfg vc; int k, conn;
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21;
        ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        v14_init(&dte);
        /* V.90 Phases 3 and 4: 60 s */
        answer_init(&a, &ac);
        p2d_init(&a.p2, &a.p2cfg);
        a.p2.state = P2D_DONE; a.p2.mode = P2D_MODE_V90; a.p2.info1a.v90 = 1; a.p2.info1a.up_sr = 4;
        ans_enter(&a, ANS_P2); a.t = 0;
        (void)answer_tick(&a, 0);
        CHECK_EQ_I(a.phase, ANS_P3); CHECK_EQ_I(a.t_phase, 0);
        a.t_phase = 60000 * 8 - 1;
        (void)answer_tick(&a, 0);
        CHECK_EQ_I(a.phase, ANS_P3);
        (void)answer_tick(&a, 0);
        CHECK_EQ_I(a.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&a), ANS_END_FAILED);
        /* V.34's Phase 2, a sub-state of ANS_V34: 30 s; in a connected call
         * (a retrain's Phase 2) the retrain failed */
        for (conn = 0; conn < 2; conn++) {
            answer_init(&a, &ac);
            memset(&vc, 0, sizeof vc);
            vc.level = 3000; vc.law = 1; vc.getbit = v14_getbit; vc.putbit = v14_putbit; vc.user = &dte;
            v34ans_init(&a.v34, &vc);
            ans_enter(&a, ANS_V34); a.t = 0; a.connected = conn;
            (void)answer_tick(&a, 0);
            CHECK_EQ_I(a.phase, ANS_V34); CHECK_EQ_I(a.v34.state, V34A_PHASE2);
            a.t_phase = 30000 * 8 - 1;
            (void)answer_tick(&a, 0);
            CHECK_EQ_I(a.phase, ANS_V34);
            (void)answer_tick(&a, 0);
            CHECK_EQ_I(a.phase, ANS_FAILED);
            CHECK_EQ_I(answer_end(&a), conn ? ANS_END_RETRAIN_FAILED : ANS_END_FAILED);
        }
        /* outside its Phase 2 ANS_V34 has no budget (the 70 ms before a
         * retrain's Phase 2 here), and the clock starts again where that
         * Phase 2 begins */
        answer_init(&a, &ac);
        v34ans_init(&a.v34, &vc);
        a.v34.state = V34A_RETRAIN; a.v34.t = 0; a.v34.have0c = 1;
        ans_enter(&a, ANS_V34); a.t = 0; a.connected = 1;
        a.t_phase = 100000 * 8;
        for (k = 0; k < 8 * 70 && a.v34.state == V34A_RETRAIN; k++) (void)answer_tick(&a, 0);
        CHECK_EQ_I(a.phase, ANS_V34); CHECK_EQ_I(a.v34.state, V34A_PHASE2);
        CHECK(a.t_phase < 8);
        for (k = 0; k < 8 * 100; k++) (void)answer_tick(&a, 0);
        CHECK_EQ_I(a.phase, ANS_V34); CHECK_EQ_I(a.v34.state, V34A_PHASE2);
    }

    T_REPORT("answer");
}
