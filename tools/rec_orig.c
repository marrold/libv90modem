/* Record a simulated call of the library's caller in the format v90d records
 * real calls in (tools/README.md), from the answering side: per 10 ms what
 * the answerer's codec received and what it sent. The answerer is the
 * library's, through the public API and set up as v90m-replay and
 * v90m-fields set it up for the options given, so that those tools follow
 * the recording exactly as they follow a real one. Host only (the lines are
 * the tests' simulations, in double).
 *
 *   rec_orig KIND OUT
 *
 *   v92   caller V.90 + V.92 (and V.34, V.32bis, V.22bis, V.21), answerer
 *         with V.92 (replay with -9): test/v90link.h, 2 km, +100 ppm, the
 *         answerer's own V.92 upstream line
 *   v90   the same caller, the answerer without V.92 (replay without -9)
 *   v34   caller V.34, V.32bis, V.22bis, V.21 (no PCM): test/line34.h's
 *         "real" line (mu-law codec at the answerer, -100 ppm, 1 Hz shifts)
 *   v21   caller V.21 only, the same line
 *
 * ec after v34 or v21: V.42 on both sides from their connection on, the
 * caller's as the originator and the answerer's as v90m-replay runs it (so
 * the replay follows the recording into data mode): the caller's frames
 * are in the recording for v90m-replay -F.
 *
 * The call is recorded from 1 s before the answerer's answer tone (the 200
 * ms of silence the replay looks for are in it) until 2 s after both
 * modems are in data mode (4 s with ec), or 40 s. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v90modem.h"
#include "v90modem_priv.h"
#include "../test/v90link.h"
#include "../test/line34.h"

static const double LOOP_MILD[3] = { 0.1, 0.8, 0.1 };

static FILE *out;
static uint8_t blk[160]; static int nblk;
static long nrec;
static void rec(uint8_t heard, uint8_t sent)
{
    blk[nblk] = heard; blk[80 + nblk] = sent;
    if (++nblk == 80) { fwrite(blk, 1, 160, out); nblk = 0; }
    nrec++;
}

static v90m_t *ans;
/* the answerer's data side, v90m-replay's: V.42 from its connection on with
 * "ec", else ones */
static v90m_ec_t *aec; static int aec_on;
static int ones(void *u) { (void)u; return aec_on ? v90m_ec_tx_bit(aec) : 1; }
static void drop(void *u, int b) { (void)u; if (aec_on) v90m_ec_rx_bit(aec, b); }
/* after each sample, as v90m-replay does after sample nrec - 1 */
static void aec_step(void)
{
    if (!aec) return;
    if (!aec_on) {
        v90m_state st; v90m_ec_cfg c;
        v90m_get_state(ans, &st);
        if (st.connected) { v90m_ec_cfg_default(&c, 0); v90m_ec_reset(aec, &c); aec_on = 1; }
    }
    if (aec_on && (nrec - 1) % 8 == 0) v90m_ec_tick(aec, 1);
}

/* the answerer as v90m-replay sets it up (tools/v90m_replay.c, configure) */
static v90m_t *make_answerer(int v92)
{
    v90m_cfg c; int err;
    v90m_cfg_default(&c);
    c.modulations = V90M_MOD_V21 | V90M_MOD_V22 | V90M_MOD_V32BIS | V90M_MOD_V34 | V90M_MOD_V90 | (v92 ? V90M_MOD_V92 : 0);
    c.tx_level = 3963;                  /* -12 dBm0: level_rms(-12) there */
    c.max_down_bps = 56000;
    c.max_up_bps = v92 ? 48000 : 33600;
    c.alaw = 0;
    c.codec_linear_rx = 0;
    c.echo_delay = -1;
    c.max_tx_code = 0;
    c.getbit = ones; c.putbit = drop;
    return v90m_create(&c, &err);
}

/* the network side of test/v90link.h: one codeword heard, one sent */
static uint8_t dig(void *u, uint8_t in)
{
    uint8_t o;
    (void)u;
    v90m_process_g711(ans, &in, &o, 1);
    rec(in, o);
    aec_step();
    return o;
}

/* the answer side of test/line34.h (a mu-law codec there) */
static int16_t atick(void *u, int16_t rx)
{
    uint8_t in = pcm_encode(0, rx), o;
    (void)u;
    v90m_process_g711(ans, &in, &o, 1);
    rec(in, o);
    aec_step();
    return (int16_t)v90_octet_linear(0, o);
}
/* the caller's V.42 with "ec", from its connection on */
static v90m_ec_t *cec; static int cec_on; static long ct;
static int16_t ctick(void *u, int16_t rx)
{
    int16_t y = orig_tick((orig_t *)u, rx);
    if (cec && !cec_on && ((orig_t *)u)->connected) cec_on = 1;
    if (cec_on && ct % 8 == 0) v90m_ec_tick(cec, 1);
    ct++;
    return y;
}

static vl_t VL;
static line34_t L34;
static orig_t O;
static int obit(void *u) { (void)u; return cec_on ? v90m_ec_tx_bit(cec) : 1; }
static void odrop(void *u, int b) { (void)u; if (cec_on) v90m_ec_rx_bit(cec, b); }

int main(int argc, char **argv)
{
    const char *kind;
    long data_at = -1, end = 8000L * 40;
    int pcm;
    if (argc < 3) { fprintf(stderr, "usage: rec_orig v92|v90|v34|v21 OUT [ec]\n"); return 2; }
    kind = argv[1];
    pcm = !strcmp(kind, "v92") || !strcmp(kind, "v90");
    if (!pcm && strcmp(kind, "v34") && strcmp(kind, "v21")) { fprintf(stderr, "rec_orig: unknown kind %s\n", kind); return 2; }
    if (argc > 3) {
        v90m_ec_cfg c; int err;
        if (pcm || strcmp(argv[3], "ec")) { fprintf(stderr, "rec_orig: \"ec\" after v34 or v21 only\n"); return 2; }
        v90m_ec_cfg_default(&c, 0); aec = v90m_ec_create(&c, &err);
        v90m_ec_cfg_default(&c, 1); cec = v90m_ec_create(&c, &err);
        if (!aec || !cec) { fprintf(stderr, "rec_orig: no V.42 endpoint\n"); return 1; }
    }
    out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    ans = make_answerer(!strcmp(kind, "v92"));
    if (!ans) { fprintf(stderr, "rec_orig: no answerer\n"); return 1; }
    /* a second before the call is answered, as v90d's recordings have: the
     * replay takes for the answer tone the first sound after 200 ms of
     * silence, more than a second in, so what is "sent" here is not silent */
    { long k; for (k = 0; k < 8000; k++) rec(0xFF, 0xFE); }
    end += 8000;

    if (pcm) {
        vl_cfg c; orig_cfg oc; vl_t *l = &VL; int ran90;
        memset(&c, 0, sizeof c);
        c.loop_km = 2; c.noise_dbm0 = -75; c.ppm = 100; c.phase = 0.3; c.echo_db = -16;
        c.up_loss_db = 6; c.up_noise_dbm0 = -70; c.full = 1; c.caller_orig = 1; c.seed = 5;
        c.caller_v92 = 1; c.up_line = 1;
        vl_init(l, &c);
        l->dig = dig; l->dig_user = NULL;
        /* the caller with every modulation it has, as a real one offers them */
        memset(&oc, 0, sizeof oc);
        oc.level = 3000;
        oc.modes = V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21 | ORIG_MODE_V90 | ORIG_MODE_V92;
        oc.echo_delay = 0; oc.law = 0;
        oc.getbit = vl_get_up; oc.putbit = vl_put_down; oc.user = l;
        orig_init(&l->org, &oc);
        while (nrec < end) {
            vl_tick(l);
            if (data_at < 0 && orig_moving(&l->org)) { data_at = nrec - 8000; end = nrec + 16000; }
            if (l->org.phase == ORIG_FAILED) break;
        }
        /* the V.90 engine's, if JM chose it (the engines share memory;
         * 0, 0 as an engine never started, otherwise) */
        ran90 = l->org.used_v8 && l->org.v8.selected == V8_SEL_V90;
        printf("rec_orig %s: %ld samples, caller %s (V.90 engine state %d, error %d), data mode at %.3f s, %d / %d bit/s down / up\n", kind, nrec,
               l->org.modulation == ORIG_MOD_V92 ? "V.92" : l->org.modulation == ORIG_MOD_V90 ? "V.90" : "other",
               ran90 ? (int)l->org.e.v90.state : 0, ran90 ? l->org.e.v90.error : 0,
               data_at / 8000.0, l->org.rx_bitrate, l->org.bitrate);
    } else {
        line34_cfg lc; orig_cfg oc;
        memset(&lc, 0, sizeof lc);
        lc.law = 1; lc.d_ca = 160; lc.d_ac = 160; lc.gain = 0.5; lc.echo_a = 0.3; lc.echo_c = 0.3;
        lc.noise = 15.0; lc.ppm = -100; lc.foff_ca = 1.0; lc.foff_ac = -1.0; lc.loop = LOOP_MILD; lc.nloop = 3;
        line34_init(&L34, &lc);
        memset(&oc, 0, sizeof oc);
        oc.level = 3000;
        oc.modes = !strcmp(kind, "v21") ? V8_MOD_V21 : V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21;
        oc.echo_delay = -1; oc.law = 0;
        oc.getbit = obit; oc.putbit = odrop; oc.user = NULL;
        orig_init(&O, &oc);
        while (nrec < end) {
            (void)line34_step(&L34, atick, NULL, ctick, &O);
            if (data_at < 0 && O.connected && orig_moving(&O)) { data_at = nrec - 8000; end = nrec + (cec ? 32000 : 16000); }
            if (O.phase == ORIG_FAILED) break;
        }
        printf("rec_orig %s: %ld samples, caller modulation %d, data mode at %.3f s, %d / %d bit/s rx / tx\n", kind, nrec,
               O.modulation, data_at / 8000.0, O.rx_bitrate, O.bitrate);
    }
    { int k, pad = nblk ? 80 - nblk : 0; for (k = 0; k < pad; k++) rec(0xFF, 0xFF); }
    fclose(out);
    v90m_destroy(ans);
    v90m_ec_destroy(aec); v90m_ec_destroy(cec);
    return data_at < 0;
}
