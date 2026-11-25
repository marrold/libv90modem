/* The answering modem with V.90 enabled against a V.90 calling peer (V.8
 * calling side, then the Phase 2 analogue side) through the channel
 * simulator: V.8 must select PCM operation and Phase 2 must complete at both
 * ends. A caller without the PCM category must still get V.22bis. The whole
 * call reaches data mode with clean data both ways; after a click in data
 * mode (Task 20: 0.75 ms of full scale on the line to us) the upstream is
 * clean from 10 s on. */
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../modem/v14.h"
#include "../sim/v8call.h"
#include "../sim/p2a.h"
#include "../sim/p34a.h"
#include "../sim/callpeer.h"
#include "../sim/channel.h"
#include "../sim/g711.h"

#define FS 8000

static void chan(channel_t *ch)
{
    static const double one[1] = {1.0};
    channel_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = -60.0; cfg.us_noise_dbfs = -60.0;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.1; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 5;
    channel_init(ch, &cfg);
}

/* bit source / checker pair for the full-call test: user = { source, sink } */
typedef struct { uint32_t s; long n, bad; } t5_prbs;
static int t5_next(t5_prbs *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
int  t5_get(void *u) { t5_prbs *p = (t5_prbs *)((void **)u)[0]; p->n++; return t5_next(p); }
void t5_put(void *u, int b) { t5_prbs *p = (t5_prbs *)((void **)u)[1]; p->n++; if (t5_next(p) != (b & 1)) p->bad++; }

/* the whole V.90 call through the PCM interface: V.8, Phase 2, Phases 3
 * and 4, then data both ways, for secs of line. Upstream goes through the
 * simulated line and the codec's mu-law A/D; downstream the peer reads our
 * octets. click_ms: a click on the line to us that long after we are
 * connected (Task 20): six samples of full-scale mu-law, 0x80 and 0x00
 * alternated; 0 none */
typedef struct { uint32_t s; long n, bad; } fc_prbs;
static fc_prbs dsrc, dsink, asrc, asink;
static p34a_t p34;
typedef struct { long at, click_at, end_at, bad10, n10; } fc_out;
static void full_call(answer_t *a, int secs, long click_ms, fc_out *r)
{
    static v8call_t v8; static p2a_t p2;
    static void *aio[2], *dio[2];
    v8_menu_t cm; p2a_cfg pc; p34a_cfg qc; answer_cfg ac; channel_t ch;
    uint8_t atx = 0xFF; double ctx = 0;
    long n;
    int stage = 0;

    r->at = r->click_at = r->end_at = -1; r->bad10 = r->n10 = 0;
    chan(&ch);
    memset(&dsrc, 0, sizeof dsrc); memset(&dsink, 0, sizeof dsink);
    memset(&asrc, 0, sizeof asrc); memset(&asink, 0, sizeof asink);
    dsrc.s = asink.s = 5; asrc.s = dsink.s = 9;
    dio[0] = &dsrc; dio[1] = &dsink; aio[0] = &asrc; aio[1] = &asink;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_V90;
    ac.getbit = t5_get; ac.putbit = t5_put; ac.user = dio;
    answer_init(a, &ac);

    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA;
    cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3;
    cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
    /* as a real V.90 client's CM (Conexant CX93001, recorded on the box): LAPM asked for, access octet present */
    cm.has_prot = 1; cm.prot = V8_PROT_LAPM; cm.has_access = 1; cm.access = 0;
    v8call_init(&v8, FS, 4000, &cm);
    memset(&pc, 0, sizeof pc);
    pc.level = 4000; pc.v90 = 1; pc.up_sr = -1; pc.uinfo = 91;
    pc.info0a.lo3000 = pc.info0a.hi3000 = 1; pc.info0a.lo3200 = pc.info0a.hi3200 = 1;
    pc.info0a.pwr_reduce = 1; pc.info0a.max_sr_diff = 5; pc.info0a.c1664 = 1;

    for (n = 0; n < (long)secs * FS; n++) {
        double crx; uint8_t srv; int16_t x;
        channel_tick(&ch, atx, ctx, &crx, &srv);
        if (click_ms && r->at >= 0) {
            r->click_at = r->at + 8L * click_ms;
            if (n >= r->click_at && n < r->click_at + 6) srv = (n - r->click_at) & 1 ? 0x00 : 0x80;
            if (n == r->click_at + 8000L * 10) { r->bad10 = dsink.bad; r->n10 = dsink.n; }
        }
        atx = answer_tick_pcm(a, srv);
        x = (int16_t)lround(crx);
        if (stage == 0) {
            int16_t o = 0;
            if (v8call_tick(&v8, x, &o) == V8C_DONE) { stage = 1; p2a_init(&p2, &pc); }
            ctx = o;
        } else if (stage == 1) {
            ctx = p2a_tick(&p2, x);
            if (p2.state == P2A_DONE) {
                int sr = p2.info1a.up_sr, hi = p2.info1d.sr[sr].hi_carrier;
                memset(&qc, 0, sizeof qc);
                qc.uinfo = 91; qc.sym_rate = sr == 3 ? 3000 : 3200;
                qc.fc = sr == 3 ? (hi ? 2000.0 : 1800.0) : (hi ? 1920.0 : 3200.0 * 4 / 7);
                qc.gain = 4000; qc.sil0 = 70 * qc.sym_rate / 1000; qc.ntrn = 600; qc.jd_wait = 50;
                qc.dil_n = 8; qc.dil_syms = 1200;
                qc.Kt = 12; qc.Mt = 4; qc.Kd = 32; qc.Md = 41; qc.up_rates = 0x1FFF;
                qc.getbit = t5_get; qc.putbit = t5_put; qc.user = aio;
                p34a_init(&p34, &qc);
                stage = 2;
            } else if (p2.state == P2A_FAILED) break;
        } else {
            ctx = p34a_tick(&p34, atx);
        }
        if (r->at < 0 && a->connected) r->at = n;
        if (a->phase == ANS_FAILED) { r->end_at = n; break; }
    }
}

int main(void)
{
    static answer_t a; static v14_t dte;
    answer_cfg ac;
    channel_t ch;

    /* 1. V.90 caller */
    {
        static v8call_t v8; static p2a_t p2;
        v8_menu_t cm; p2a_cfg pc;
        int16_t atx = 0, ctx = 0;
        int n, in_p2 = 0, p2_at = -1, a_done = -1, d_done = -1;

        chan(&ch); v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_V90;
        ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);

        memset(&cm, 0, sizeof cm);
        cm.call_function = V8_CALLF_DATA;
        cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3;
        cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
        v8call_init(&v8, FS, 4000, &cm);
        memset(&pc, 0, sizeof pc);
        pc.level = 4000; pc.v90 = 1; pc.up_sr = -1; pc.uinfo = 91;
        pc.info0a.lo3000 = pc.info0a.hi3000 = 1; pc.info0a.lo3200 = pc.info0a.hi3200 = 1;
        pc.info0a.pwr_reduce = 1; pc.info0a.max_sr_diff = 5; pc.info0a.c1664 = 1;

        for (n = 0; n < 20 * FS; n++) {
            double crx; uint8_t srv; int16_t t, x;
            channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &crx, &srv);
            t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            x = (int16_t)lround(crx);
            if (!in_p2) {
                int16_t o = 0;
                if (v8call_tick(&v8, x, &o) == V8C_DONE) { in_p2 = 1; p2a_init(&p2, &pc); }
                ctx = o;
            } else {
                ctx = p2a_tick(&p2, x);
                if (p2.state == P2A_DONE && a_done < 0) a_done = n;
            }
            atx = t;
            if (a.phase == ANS_P2 && p2_at < 0) p2_at = n;
            if (a.phase == ANS_P3 && d_done < 0) d_done = n;
            if (a.phase == ANS_FAILED) break;
            if (a_done >= 0 && d_done >= 0 && n > d_done + FS / 10) break;
        }
        printf("  answer_v90: Phase 2 from %.2f s, done at %.2f s (peer %.2f s), rtd %d\n",
               p2_at / 8000.0, d_done / 8000.0, a_done / 8000.0, (int)a.p2.rtd);
        CHECK(v8.got_jm);
        CHECK(v8.jm.has_pcm && (v8.jm.pcm & V8_PCM_DIGITAL));
        CHECK_EQ_I(a.v8.selected, V8_SEL_V90);
        CHECK(p2_at > 0);
        CHECK(d_done > p2_at); CHECK(a_done > p2_at);
        CHECK_EQ_I(a.phase, ANS_P3);
        CHECK_EQ_I(a.p2.error, P2D_OK);
        CHECK(a.p2.info1a.v90);
        CHECK(a.p2.info1a.up_sr == 4 || a.p2.info1a.up_sr == 3);
        CHECK_EQ_I(a.p2.info1a.uinfo, 91);
        CHECK(!a.connected);                 /* training is not a connection */
    }

    /* 2. V.8 caller without PCM: V.22bis as before, even with V.90 enabled */
    {
        static callpeer_t c;
        int16_t atx = 0, ctx = 0; int n, at = -1;
        chan(&ch); v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_V90;
        ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);
        callpeer_init(&c, 1, V8_MOD_V22 | V8_MOD_V21, 1, 3000);
        for (n = 0; n < 14 * FS && at < 0; n++) {
            double crx; uint8_t srv; int16_t t;
            channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &crx, &srv);
            t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            ctx = callpeer_tick(&c, (int16_t)lround(crx));
            atx = t;
            if (a.connected && c.connected) at = n;
        }
        CHECK(at > 0);
        CHECK_EQ_I(a.modulation, ANS_MOD_V22BIS);
    }

    /* 3. V.90 caller, V.90 not enabled here: no PCM in JM, no Phase 2 */
    {
        static v8call_t v8; v8_menu_t cm;
        int16_t atx = 0, ctx = 0; int n;
        chan(&ch); v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21;
        ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);
        memset(&cm, 0, sizeof cm);
        cm.call_function = V8_CALLF_DATA;
        cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3;
        cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
        v8call_init(&v8, FS, 4000, &cm);
        for (n = 0; n < 10 * FS; n++) {
            double crx; uint8_t srv; int16_t t, o = 0;
            channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &crx, &srv);
            t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            if (v8call_tick(&v8, (int16_t)lround(crx), &o) == V8C_DONE) break;
            ctx = o; atx = t;
        }
        CHECK(v8.got_jm);
        CHECK(!(v8.jm.has_pcm && (v8.jm.pcm & V8_PCM_DIGITAL)));
        CHECK(a.phase != ANS_P2 && a.phase != ANS_P3);
    }

    /* 4. V.34 caller without PCM, V.90 enabled here: we have no V.34 data
     * mode, so JM must not offer V.34 and V.22bis is selected */
    {
        static v8call_t v8; v8_menu_t cm;
        int16_t atx = 0, ctx = 0; int n;
        chan(&ch); v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_V90;
        ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);
        memset(&cm, 0, sizeof cm);
        cm.call_function = V8_CALLF_DATA;
        cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3;
        v8call_init(&v8, FS, 4000, &cm);
        for (n = 0; n < 10 * FS; n++) {
            double crx; uint8_t srv; int16_t t, o = 0;
            channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &crx, &srv);
            t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            if (v8call_tick(&v8, (int16_t)lround(crx), &o) == V8C_DONE) break;
            ctx = o; atx = t;
        }
        CHECK(v8.got_jm);
        CHECK(!(v8.jm.modes & V8_MOD_V34));
        CHECK(v8.jm.modes & V8_MOD_V22);
        CHECK(!v8.jm.has_pcm);
        CHECK_EQ_I(v8_selected(&v8.jm), 4);
    }

    /* 5. the whole V.90 call (full_call) */
    {
        fc_out r;
        full_call(&a, 16, 0, &r);
        printf("  answer_v90: full call connected at %.2f s: %d bit/s down, %d bit/s up; "
               "down %ld bits %ld bad, up %ld bits %ld bad\n", r.at / 8000.0, a.bitrate, a.rx_bitrate,
               asink.n, asink.bad, dsink.n, dsink.bad);
        if (a.phase == ANS_FAILED) printf("    failed: p2 state %d err %d, p34 state %d err %d\n", a.p2.state, a.p2.error, a.p34.state, a.p34.error);
        CHECK(r.at > 0);
        CHECK_EQ_I(a.phase, ANS_V90); CHECK_EQ_I(a.modulation, ANS_MOD_V90);
        /* our JM says what a V.90 client needs to hear before it will try V.90:
         * a digital modem ON a digital connection (V.8 6.5, bit b7), and LAPM
         * when it was asked for (6.4) */
        CHECK(a.v8.jm.has_pcm && (a.v8.jm.pcm & V8_PCM_DIGITAL));
        CHECK(a.v8.jm.has_access && (a.v8.jm.access & V8_ACC_DIGITAL));
        CHECK(a.v8.jm.has_prot && a.v8.jm.prot == V8_PROT_LAPM);
        CHECK_EQ_I(a.bitrate, 50666); CHECK(a.rx_bitrate >= 19200);
        CHECK(p34.tx_ready && p34.rx_ready);
        CHECK(asink.n > 100000); CHECK_EQ_I(asink.bad, 0);
        CHECK(dsink.n > 40000); CHECK_EQ_I(dsink.bad, 0);
    }

    /* 6. a click in data mode (Task 20): 0.75 ms of full scale on the line
     * to us, 2 s after we are connected. From 10 s after it the upstream
     * is clean, or the call has ended */
    {
        fc_out r;
        full_call(&a, 30, 2000, &r);
        printf("  answer_v90: a click 2 s into data mode: up %ld bits %ld bad, %ld bits %ld bad from 10 s after it; end %d\n",
               dsink.n, dsink.bad, dsink.n - r.n10, dsink.bad - r.bad10, answer_end(&a));
        CHECK(r.at > 0); CHECK(r.click_at > r.at);
        CHECK((dsink.bad == r.bad10 && dsink.n - r.n10 >= 4L * a.rx_bitrate) || answer_end(&a) != ANS_END_NONE);
    }

    T_REPORT("answer_v90");
}
