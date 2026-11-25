#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "answer.h"
#include "v8call.h"
#include "p2a.h"
#include "p34a.h"
#include "channel.h"
#include "g711.h"
#define FS 8000
typedef struct { uint32_t s; long n, bad; } prbs;
static int allones;
static int nx(prbs *p) { p->s = p->s * 1103515245u + 12345u; return allones ? 1 : (int)((p->s >> 16) & 1); }
static int get(void *u) { prbs *p = ((void **)u)[0]; p->n++; return nx(p); }
static void put(void *u, int b) { prbs *p = ((void **)u)[1]; p->n++; if (nx(p) != (b & 1)) p->bad++; }
int main(int argc, char **argv)
{
    static answer_t a; static v8call_t v8; static p2a_t p2; static p34a_t p34;
    static prbs dsrc, dsink, asrc, asink; static void *aio[2], *dio[2];
    static const double one[1] = {1.0};
    double noise = argc > 1 ? atof(argv[1]) : -55, echo = argc > 2 ? atof(argv[2]) : 0.2;
    int secs = argc > 3 ? atoi(argv[3]) : 60; allones = argc > 4;
    channel_t ch; channel_cfg cfg; answer_cfg ac; v8_menu_t cm; p2a_cfg pc; p34a_cfg qc;
    uint8_t atx = 0xFF; double ctx = 0; int n, stage = 0; long lastbad = 0, lastn = 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = cfg.us_noise_dbfs = noise; cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = echo; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
    channel_init(&ch, &cfg);
    dsrc.s = asink.s = 5; asrc.s = dsink.s = 9;
    dio[0] = &dsrc; dio[1] = &dsink; aio[0] = &asrc; aio[1] = &asink;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_V90; ac.getbit = get; ac.putbit = put; ac.user = dio;
    answer_init(&a, &ac);
    memset(&cm, 0, sizeof cm); cm.call_function = V8_CALLF_DATA;
    cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3; cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
    v8call_init(&v8, FS, 4000, &cm);
    memset(&pc, 0, sizeof pc);
    pc.level = 4000; pc.v90 = 1; pc.up_sr = -1; pc.uinfo = 91;
    pc.info0a.lo3000 = pc.info0a.hi3000 = 1; pc.info0a.lo3200 = pc.info0a.hi3200 = 1;
    pc.info0a.pwr_reduce = 1; pc.info0a.max_sr_diff = 5; pc.info0a.c1664 = 1;
    for (n = 0; n < secs * FS; n++) {
        double crx; uint8_t srv; int16_t x;
        channel_tick(&ch, atx, ctx, &crx, &srv);
        atx = answer_tick_pcm(&a, srv);
        x = (int16_t)lround(crx);
        if (stage == 0) { int16_t o = 0; if (v8call_tick(&v8, x, &o) == V8C_DONE) { stage = 1; p2a_init(&p2, &pc); } ctx = o; }
        else if (stage == 1) {
            ctx = p2a_tick(&p2, x);
            if (p2.state == P2A_DONE) {
                int sr = p2.info1a.up_sr, hi = p2.info1d.sr[sr].hi_carrier;
                memset(&qc, 0, sizeof qc);
                qc.uinfo = 91; qc.sym_rate = sr == 3 ? 3000 : 3200;
                qc.fc = sr == 3 ? (hi ? 2000.0 : 1800.0) : (hi ? 1920.0 : 3200.0 * 4 / 7);
                qc.gain = 4000; qc.sil0 = 70 * qc.sym_rate / 1000; qc.ntrn = 600; qc.jd_wait = 50;
                qc.dil_n = 8; qc.dil_syms = 1200; qc.Kt = 12; qc.Mt = 4; qc.Kd = 32; qc.Md = 41; qc.up_rates = 0x1FFF;
                qc.getbit = get; qc.putbit = put; qc.user = aio;
                p34a_init(&p34, &qc); stage = 2;
            }
        } else ctx = p34a_tick(&p34, atx);
        if (a.phase == ANS_FAILED) { printf("FAILED at %.2f err %d\n", n / 8000.0, a.p34.error); break; }
        if (n % (2 * FS) == 2 * FS - 1 && a.connected) {
            { int k; double te = 0; qamrx_t *r = &a.p34.up.rx; for (k = 0; k < QRX_NTAP; k++) te += (double)r->cr[k] * r->cr[k] + (double)r->ci[k] * r->ci[k];
            printf("t=%2d s: up %ld bits, %ld bad (snr %d) mse %.1f dB cent %d freq %d tinteg %d gain %d tapE %.3g\n", (n + 1) / FS, dsink.n - lastn, dsink.bad - lastbad, (int)a.p34.snr_db10,
                   10 * log10((r->mse + 1e-9) / 65536.0), qamrx_centroid(r), (int)r->freq, (int)r->t_integ, (int)r->gain, te); }
            lastn = dsink.n; lastbad = dsink.bad;
        }
    }
    printf("noise %.0f echo %.2f: %d down / %d up (sym %d); down %ld bits %ld bad; up %ld bits %ld bad\n", noise, echo,
           a.bitrate, a.rx_bitrate, a.p34.cfg.sym_rate, asink.n, asink.bad, dsink.n, dsink.bad);
    return 0;
}
