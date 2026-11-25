/* modem/orig: the calling modem up to V.22bis, V.22 and V.21 data mode,
 * against the library's answering modem (modem/answer) and against pre-V.8
 * answerers (ANS, then V.22bis / V.22 / V.21), over a 2-wire line
 * (sim/v32line). Text is carried both ways through V.14 start-stop framers
 * and compared. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/orig.h"
#include "../modem/answer.h"
#include "../modem/v14.h"
#include "v32line.h"

#define FS 8000
#define TWO_PI 6.283185307179586
#define PI 3.141592653589793
#define MS(ms) ((long)(ms) * FS / 1000)

static const char up_text[] = "the calling modem says: 0123456789 abcdefghijklmnopqrstuvwxyz";
static const char down_text[] = "the answering modem says: ZYXWVUTSRQPONMLKJIHGFEDCBA 9876543210";

enum { P_ANSWER, P_LEGACY_V22, P_LEGACY_V21, P_SILENT };
static int peer_kind, peer_2400, peer_mute;
static long peer_n;
static answer_t A;                 /* the library's answerer */
static v22bis_t PV;                /* a pre-V.8 V.22bis answerer */
static v21mod_t PFM; static v21demod_t PFD; static arx_t PU; static int pfbit = 1;
static v14_t c_dte, a_dte;         /* the two terminals */
static uint8_t a_got[256]; static int a_ngot;   /* what the legacy V.21 peer received */

static int16_t clip(double x) { long q = lrint(x); return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q); }

/* V.25 ANS: 2100 Hz, reversals every 450 ms, for ans_ms; then silence */
static double ans(long n, long ans_ms)
{
    double t = (double)n / FS, ph = TWO_PI * 2100 * t;
    if (n >= MS(ans_ms)) return 0;
    if (((long)(t / 0.45)) % 2) ph += PI;
    return 4243 * cos(ph);
}

static double peer(void *u, double rx)
{
    int16_t r = clip(rx);
    (void)u;
    peer_n++;
    if (peer_mute) return 0;
    switch (peer_kind) {
    case P_ANSWER:
        return answer_tick(&A, r);
    case P_LEGACY_V22:
        /* V.22bis 6.3.1.2: answer tone 3.3 s, 75 ms of silence, then the
         * answering modem's start-up */
        if (peer_n < MS(3300)) return ans(peer_n, 3300);
        if (peer_n < MS(3375)) return 0;
        return v22m_tick(&PV, r);
    case P_LEGACY_V21: {
        /* answer tone, then a steady mark on channel 2, our text after 1 s */
        int done, c;
        if (peer_n < MS(3300)) return ans(peer_n, 3300);
        {
            /* as a real receiver: noise is idle mark (squelch), and
             * characters are delivered once the carrier has been there
             * 100 ms (its onset can look like a start bit) */
            static long cd;
            int level = v21demod_tick(&PFD, r);
            if (PFD.energy < v21demod_energy_of(&PFD, 110)) { level = 1; cd = 0; } else cd++;
            c = arx_tick(&PU, level);
            if (cd < MS(100)) c = -1;
        }
        if (c >= 0 && a_ngot < (int)sizeof a_got) a_got[a_ngot++] = (uint8_t)c;
        {
            double out = v21mod_tick(&PFM, pfbit, &done);
            if (done) pfbit = peer_n > MS(4300) ? v14_getbit(&a_dte) : 1;
            return out;
        }
    }
    default:
        return 0;
    }
}

static v32line_t L;
static orig_t O;
static long now;
static int16_t o_rx;

static void line(v32line_cfg *lc, int realistic)
{
    memset(lc, 0, sizeof *lc);
    lc->mulaw = 1; lc->seed = 3; lc->delay = 40;
    lc->noise_a_dbm0 = lc->noise_c_dbm0 = -300; lc->near_db = lc->far_db = lc->c_near_db = lc->c_far_db = 300;
    if (realistic) {
        lc->loss_db = 10; lc->delay = 160;
        lc->near_db = 12; lc->far_db = 15; lc->c_near_db = 12; lc->c_far_db = 18;
        lc->noise_a_dbm0 = -45; lc->noise_c_dbm0 = -45;
        lc->freq_offset = 1; lc->ppm = 50;
    }
}

static void start(int kind, unsigned our_modes, unsigned their_modes, int realistic)
{
    v32line_cfg lc; orig_cfg oc; answer_cfg ac;
    line(&lc, realistic);
    v32line_init(&L, &lc);
    v14_init(&c_dte); v14_init(&a_dte);
    memset(&oc, 0, sizeof oc);
    oc.level = 3000; oc.modes = our_modes; oc.getbit = v14_getbit; oc.putbit = v14_putbit; oc.user = &c_dte;
    orig_init(&O, &oc);
    peer_kind = kind; peer_n = 0; peer_mute = 0; pfbit = 1; a_ngot = 0;
    if (kind == P_ANSWER) {
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = their_modes; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &a_dte;
        answer_init(&A, &ac);
    }
    if (kind == P_LEGACY_V22) v22m_init(&PV, V22_ANSWERER, peer_2400, 3000, v14_getbit, v14_putbit, &a_dte);
    if (kind == P_LEGACY_V21) {
        v21mod_init(&PFM, V21H_MARK, V21H_SPACE, FS, 300, 4243);
        v21demod_init(&PFD, V21L_MARK, V21L_SPACE, FS, 300);
        arx_init(&PU, FS, 300);
    }
    now = 0; o_rx = 0;
}

static void run(long samples)
{
    while (samples-- > 0) { o_rx = v32line_tick(&L, orig_tick(&O, o_rx), peer, NULL); now++; }
}

/* runs until both ends are connected (or the caller failed); returns the time */
static double to_data(long max_ms, int peer_connected_too)
{
    long end = now + MS(max_ms);
    while (now < end) {
        run(8);
        if (O.phase == ORIG_FAILED) return -1;
        if (O.connected && (!peer_connected_too || (peer_kind == P_ANSWER ? A.connected : PV.tx_ready && PV.rx_ready)))
            return now / (double)FS;
    }
    return -1;
}

/* text both ways for `ms`; checks it arrived whole */
static void exchange(const char *what, long ms)
{
    uint8_t got[256]; int ng = 0, ag = 0, k;
    uint8_t agot[256];
    v14_write(&c_dte, (const uint8_t *)up_text, (int)strlen(up_text));
    v14_write(&a_dte, (const uint8_t *)down_text, (int)strlen(down_text));
    for (k = 0; k < ms / 10; k++) {
        run(MS(10));
        ng += v14_read(&c_dte, got + ng, (int)sizeof got - 1 - ng);
        ag += v14_read(&a_dte, agot + ag, (int)sizeof agot - 1 - ag);
    }
    got[ng] = 0; agot[ag] = 0;
    printf("    %s: caller got %d of %d octets, answerer got %d of %d\n", what, ng, (int)strlen(down_text), ag, (int)strlen(up_text));
    CHECK(ng == (int)strlen(down_text) && !memcmp(got, down_text, strlen(down_text)));
    CHECK(ag == (int)strlen(up_text) && !memcmp(agot, up_text, strlen(up_text)));
}

int main(void)
{
    double t;

    printf("V.8 then V.22bis, against the library's answerer\n");
    start(P_ANSWER, V8_MOD_V22 | V8_MOD_V21, V8_MOD_V22 | V8_MOD_V21, 0);
    t = to_data(20000, 1);
    printf("  clean line: data at %.2f s, %d bit/s, modulation %d, V.8 %d\n", t, O.bitrate, O.modulation, O.used_v8);
    CHECK(t > 0);
    CHECK_EQ_I(O.modulation, ORIG_MOD_V22BIS); CHECK_EQ_I(O.bitrate, 2400); CHECK_EQ_I(O.used_v8, 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V22BIS);
    exchange("2400 bit/s", 2000);
    /* the answerer retrains (V.22bis 6.4): we follow, and data goes on */
    v22m_retrain(&A.v22);
    run(MS(3000));
    printf("    after a retrain started by the answerer: ours %d, theirs %d; moving %d\n", O.e.leg.v22.retrains, A.v22.retrains, orig_moving(&O));
    CHECK(O.e.leg.v22.retrains >= 1);
    CHECK(orig_moving(&O));
    exchange("after the retrain", 2000);
    /* the answerer goes away: the carrier is lost */
    peer_mute = 1;
    run(MS(1500));
    printf("    the answerer silent: phase %d, end %d\n", O.phase, O.end);
    CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_LOST);

    start(P_ANSWER, V8_MOD_V22 | V8_MOD_V21, V8_MOD_V22 | V8_MOD_V21, 1);
    t = to_data(20000, 1);
    printf("  10 dB loss, echoes, noise, 1 Hz, 50 ppm: data at %.2f s, %d bit/s\n", t, O.bitrate);
    CHECK(t > 0); CHECK_EQ_I(O.bitrate, 2400);
    exchange("2400 bit/s", 3000);

    printf("V.8 then V.21\n");
    start(P_ANSWER, V8_MOD_V22 | V8_MOD_V21, V8_MOD_V21, 1);
    t = to_data(20000, 1);
    printf("  data at %.2f s, %d bit/s, modulation %d\n", t, O.bitrate, O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V21); CHECK_EQ_I(A.modulation, ANS_MOD_V21);
    exchange("300 bit/s", 5000);

    printf("V.8 then V.23\n");
    start(P_ANSWER, V8_MOD_V23 | V8_MOD_V21, V8_MOD_V23, 0);
    t = to_data(20000, 1);
    printf("  data at %.2f s, %d/%d bit/s, modulation %d\n", t, O.bitrate, O.rx_bitrate, O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V23); CHECK_EQ_I(A.modulation, ANS_MOD_V23);
    CHECK_EQ_I(O.bitrate, 75); CHECK_EQ_I(O.rx_bitrate, 1200);
    exchange("V.23 1200/75 bit/s", 12000);           /* the caller's backward channel, 75 bit/s, is the slow side */

    printf("pre-V.8 answerers: ANS, then\n");
    peer_2400 = 1;
    start(P_LEGACY_V22, V8_MOD_V22 | V8_MOD_V21, 0, 1);
    t = to_data(20000, 1);
    printf("  V.22bis: data at %.2f s, %d bit/s, modulation %d, V.8 %d\n", t, O.bitrate, O.modulation, O.used_v8);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V22BIS); CHECK_EQ_I(O.used_v8, 0);
    exchange("2400 bit/s", 2000);
    peer_2400 = 0;
    start(P_LEGACY_V22, V8_MOD_V22 | V8_MOD_V21, 0, 1);
    t = to_data(20000, 1);
    printf("  V.22 (1200 bit/s only): data at %.2f s, %d bit/s, modulation %d\n", t, O.bitrate, O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V22); CHECK_EQ_I(O.bitrate, 1200);
    exchange("1200 bit/s", 2000);
    start(P_LEGACY_V21, V8_MOD_V22 | V8_MOD_V21, 0, 1);
    t = to_data(20000, 0);
    printf("  V.21: data at %.2f s, modulation %d\n", t, O.modulation);
    CHECK(t > 0); CHECK_EQ_I(O.modulation, ORIG_MOD_V21);
    {
        uint8_t got[256]; int ng = 0, k;
        v14_write(&c_dte, (const uint8_t *)up_text, (int)strlen(up_text));
        v14_write(&a_dte, (const uint8_t *)down_text, (int)strlen(down_text));
        for (k = 0; k < 500; k++) { run(MS(10)); ng += v14_read(&c_dte, got + ng, (int)sizeof got - 1 - ng); }
        printf("    caller got %d of %d octets, answerer %d of %d\n", ng, (int)strlen(down_text), a_ngot, (int)strlen(up_text));
        CHECK(ng == (int)strlen(down_text) && !memcmp(got, down_text, strlen(down_text)));
        CHECK(a_ngot == (int)strlen(up_text) && !memcmp(a_got, up_text, strlen(up_text)));
    }

    printf("no call\n");
    start(P_SILENT, V8_MOD_V22 | V8_MOD_V21, 0, 1);
    run(MS(12000));
    printf("  no answer tone: phase %d, end %d\n", O.phase, O.end);
    CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_NO_TONE);
    start(P_ANSWER, V8_MOD_V21, V8_MOD_V22, 1);
    run(MS(12000));
    printf("  nothing in common: phase %d, end %d\n", O.phase, O.end);
    CHECK_EQ_I(O.phase, ORIG_FAILED); CHECK_EQ_I(O.end, ORIG_END_NO_COMMON);

    T_REPORT("orig");
}
