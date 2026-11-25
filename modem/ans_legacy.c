/* The answering modem's legacy scheme (modem/ans_scheme.h): V.22bis / V.22
 * and Bell 212A (ANS_V22), V.21 and Bell 103 (ANS_V21), V.32bis / V.32
 * (ANS_V32, and ANS_V32_SIL after a caller's AA), V.23 (ANS_V23). */
#include "ans_scheme.h"
#include "v21_bpf.h"
#include "bell103_bpf.h"
#include "v23_bpf.h"

#define MS(ms) ((int32_t)(ms) * 8)

#define V22_TOTAL_MS     30000   /* give up on V.22bis start-up */
#define V22_LEGACY_MS     8000   /* pre-V.8 caller silent this long: try V.21 */
#ifndef V22_BELL_MS
#define V22_BELL_MS       4000   /* silent this long: the Bell answer tone from then on */
#endif
#define V21_WAIT_MS      20000
#define V21_CD_ON_MS       100
#define CD_LOSS_MS         400
#define V21_CD_AMP         110   /* about -46 dBm0, as in v8ans */
#define BELL103_MARK_BLOCKS 20   /* 200 ms of the caller's 1270 Hz and little else */
#define BELL103_1270 ((uint32_t)(((uint64_t)BELL103_O_MARK << 32) / 8000))
/* A V.21 caller without V.8 answers the answer tone with its mark, 980 Hz
 * on channel No. 1 (V.25 5, V.21 7 a)), and waits for ours. Heard as the
 * Bell 103 mark is: 200 ms in a row of 10 ms blocks with more than half of
 * their power within the tone's 100 Hz bin. Nothing else a caller without
 * V.8 sends in our V.22 wait is that: a V.22 or Bell 212A caller's
 * scrambled ones spread over 900 to 1500 Hz around their 1200 Hz carrier
 * (600 baud), a V.22bis caller's S1 is 900 and 1500 Hz for 100 ms, a
 * Bell 103 caller's mark 1270 Hz; a V.8 caller's CM has no more than ten
 * marks (33 ms) in a row. The band-pass in front passes 980 Hz (-0.1 dB). */
#define V21_MARK_BLOCKS 20
#define V21L_980 ((uint32_t)(((uint64_t)V21L_MARK << 32) / 8000))
#if V21_BPF_N != BELL103_BPF_N || V21_BPF_N != V23_BPF_N
#error "the receive band-passes share one loop"
#endif

static void bpf_reset(answer_t *a, const int16_t *taps)
{
    for (a->bpos = 0; a->bpos < 128; a->bpos++) a->bpf[a->bpos] = 0;
    a->bpos = 0; a->bpf_taps = taps;
}

void ans_start_v22(answer_t *a)
{
    v22m_init(&a->v22, V22_ANSWERER, 1, a->cfg.level, a->cfg.getbit, a->cfg.putbit, a->cfg.user);
    bpf_reset(a, bell103_bpf);
    tonedet_init(&a->bell, BELL103_1270);
    tonedet_init(&a->v21mark, V21L_980);
    v21mod_init(&a->fmod, BELL103_A_MARK, BELL103_A_SPACE, 8000, 300, (int16_t)(a->cfg.level * 181 / 128));
    a->via_bell = 0;
    ans_enter(a, ANS_V22); a->t = 0;
}

/* 300 bit/s FSK: V.21 (we send the high channel) or, bell = 1, Bell 103
 * (we send 2225 / 2025 Hz and listen on 1270 / 1070 Hz) */
static void start_fsk(answer_t *a, int bell)
{
    /* peak amplitude of a sine with the configured RMS */
    v21mod_init(&a->fmod, bell ? BELL103_A_MARK : V21H_MARK, bell ? BELL103_A_SPACE : V21H_SPACE, 8000, 300, (int16_t)(a->cfg.level * 181 / 128));
    v21demod_init(&a->fdem, bell ? BELL103_O_MARK : V21L_MARK, bell ? BELL103_O_SPACE : V21L_SPACE, 8000, 300);
    arx_init(&a->uart, 8000, 300);
    a->cd_thresh = v21demod_energy_of(&a->fdem, V21_CD_AMP);
    a->fbit = 1; a->cd_run = 0; a->fsk_mod = bell ? ANS_MOD_BELL103 : ANS_MOD_V21;
    bpf_reset(a, bell ? bell103_bpf : v21_bpf);
    tonedet_init(&a->bell, BELL103_1270);
    ans_enter(a, ANS_V21); a->t = 0;
}

void ans_start_v21(answer_t *a) { start_fsk(a, 0); }

/* V.23: answering modem transmits on the forward channel (1300/2100 Hz,
 * 1200 baud) and receives on the backward channel (390/450 Hz, 75 baud).
 * The backward band-pass (v23_bpf) keeps the echo of our own forward channel
 * out of the receiver, as the V.21 band-pass does. */
void ans_start_v23(answer_t *a)
{
    v21mod_init(&a->fmod, V23H_MARK, V23H_SPACE, 8000, 1200, (int16_t)(a->cfg.level * 181 / 128));
    v21demod_init(&a->fdem, V23L_MARK, V23L_SPACE, 8000, 75);
    arx_init(&a->uart, 8000, 75);
    a->cd_thresh = v21demod_energy_of(&a->fdem, V21_CD_AMP);
    a->fbit = 1; a->cd_run = 0; a->fsk_mod = ANS_MOD_V23;
    bpf_reset(a, v23_bpf);
    ans_enter(a, ANS_V23); a->t = 0;
}

/* V.8 selected V.32 (item 3 covers V.32bis), or a caller without V.8 sent
 * AA: AC goes out now; the 75 ms of silence are behind us either way.
 * V.32bis has one rate for both directions: the lower ceiling counts. */
void ans_start_v32(answer_t *a)
{
    static const int rate[5] = { 4800, 7200, 9600, 12000, 14400 };
    v32_cfg c;
    int cap = a->max_down, i;
    if (a->max_up > 0 && (cap <= 0 || a->max_up < cap)) cap = a->max_up;
    c.level = a->cfg.level;
    c.rates = V32_R4800;
    for (i = 1; i < 5; i++) if (cap <= 0 || rate[i] <= cap) c.rates |= 1u << i;
    c.getbit = a->cfg.getbit; c.putbit = a->cfg.putbit; c.user = a->cfg.user;
    c.echo_delay = a->echo_delay < 0 ? 0 : a->echo_delay;
    c.no_auto_retrain = 0;
    v32_init(&a->u.v32, &c);
    ans_enter(a, ANS_V32); a->t = 0;
}

/* Keep the echo of our own channel (1650 / 1850 Hz, or 2025 / 2225 Hz in
 * Bell 103) out of the receiver: its correlators alone reject it by less
 * than 20 dB, which an ordinary hybrid (and nobody calling) turned into a
 * carrier indication. */
static int16_t v21_bandpass(answer_t *a, int16_t x)
{
    int32_t acc = 0;
    int k;
    a->bpf[a->bpos] = x;
    for (k = 0; k < V21_BPF_N; k++) acc += (int32_t)a->bpf[(a->bpos - k) & 127] * a->bpf_taps[k] >> 4;
    a->bpos = (a->bpos + 1) & 127;
    acc >>= 11;
    return (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
}

static int16_t tick_v21(answer_t *a, int16_t rx)
{
    int level, c, done, i;
    int16_t out;

    int16_t x = v21_bandpass(a, rx);

    /* waiting for a caller without V.8 in V.21: one that sends 1270 Hz is a
     * Bell 103 modem (a V.21 caller's mark is 980 Hz). Decided in 60 ms,
     * before the V.21 receiver's 100 ms take the tone for its carrier. */
    if (!a->connected && !a->used_v8 && a->fsk_mod == ANS_MOD_V21 && (a->cfg.modes & ANS_MODE_BELL103)
        && tonedet_tick(&a->bell, x)) { start_fsk(a, 1); return 0; }
    level = v21demod_tick(&a->fdem, x);
    if (a->fdem.energy < a->cd_thresh) {
        level = 1;
        if (a->cd_run > 0) a->cd_run = 0;
        if (a->connected && --a->cd_run < -MS(CD_LOSS_MS)) { ans_fail(a, ANS_END_LOST); return 0; }
    } else {
        if (a->cd_run < 0) a->cd_run = 0;
        if (a->cd_run < MS(V21_CD_ON_MS)) a->cd_run++;
        else if (!a->connected) { a->connected = 1; a->modulation = a->fsk_mod; a->bitrate = 300; }
    }
    c = arx_tick(&a->uart, level);
    if (c >= 0 && a->connected) {            /* hand the character on as start-stop bits */
        a->cfg.putbit(a->cfg.user, 0);
        for (i = 0; i < 8; i++) a->cfg.putbit(a->cfg.user, (c >> i) & 1);
        a->cfg.putbit(a->cfg.user, 1);
    }
    out = v21mod_tick(&a->fmod, a->fbit, &done);
    if (done) a->fbit = a->connected ? (a->cfg.getbit(a->cfg.user) & 1) : 1;
    if (!a->connected && a->t > MS(V21_WAIT_MS)) ans_fail(a, ANS_END_FAILED);
    return out;
}

static int16_t tick_v23(answer_t *a, int16_t rx)
{
    int level, c, done, i;
    int16_t out;

    level = v21demod_tick(&a->fdem, v21_bandpass(a, rx));
    if (a->fdem.energy < a->cd_thresh) {
        level = 1;
        if (a->cd_run > 0) a->cd_run = 0;
        if (a->connected && --a->cd_run < -MS(CD_LOSS_MS)) { ans_fail(a, ANS_END_LOST); return 0; }
    } else {
        if (a->cd_run < 0) a->cd_run = 0;
        if (a->cd_run < MS(V21_CD_ON_MS)) a->cd_run++;
        else if (!a->connected) {
            a->connected = 1; a->modulation = ANS_MOD_V23;
            a->bitrate = 1200; a->rx_bitrate = 75;
        }
    }
    c = arx_tick(&a->uart, level);
    if (c >= 0 && a->connected) {
        a->cfg.putbit(a->cfg.user, 0);
        for (i = 0; i < 8; i++) a->cfg.putbit(a->cfg.user, (c >> i) & 1);
        a->cfg.putbit(a->cfg.user, 1);
    }
    out = v21mod_tick(&a->fmod, a->fbit, &done);
    if (done) a->fbit = a->connected ? (a->cfg.getbit(a->cfg.user) & 1) : 1;
    if (!a->connected && a->t > MS(V21_WAIT_MS)) ans_fail(a, ANS_END_FAILED);
    return out;
}

static int16_t tick_v22(answer_t *a, int16_t rx)
{
    int16_t tx;
    /* A Bell 103 caller takes our unscrambled ones (2250 Hz is in them)
     * or the answer tone before them for its carrier and replies with a
     * steady 1270 Hz. Listened for behind the band-pass that keeps our
     * own signal out: its echo may be the louder of the two. A V.22
     * caller's scrambled signal is spread over the band and a V.22bis
     * caller's S1 is 900 and 1500 Hz: neither is one tone at 1270 Hz.
     * A V.21 caller's mark, 980 Hz, is heard the same way behind the
     * same band-pass, and we answer it in V.21 (V.8 8.2.2: no CM, then
     * "other appropriate Recommendation"; V.21 7 a): our mark, 1650 Hz
     * on channel No. 2). Before, it held V.22's carrier detect on and
     * the fall-back to V.21 on silence never came. */
    if (!a->connected && !a->used_v8 && (a->cfg.modes & (ANS_MODE_BELL103 | V8_MOD_V21)) && a->v22.state == V22S_WAIT) {
        int16_t x = v21_bandpass(a, rx);
        if (a->cfg.modes & ANS_MODE_BELL103) {
            (void)tonedet_tick(&a->bell, x);
            if (a->bell.blocks >= BELL103_MARK_BLOCKS) { start_fsk(a, 1); return 0; }
        }
        if (a->cfg.modes & V8_MOD_V21) {
            (void)tonedet_tick(&a->v21mark, x);
            if (a->v21mark.blocks >= V21_MARK_BLOCKS) { ans_start_v21(a); return 0; }
        }
    }
    {
        /* Nobody has answered our unscrambled ones: a Bell 212A (or a
         * Bell 103) caller may be waiting for 2225 Hz and nothing else.
         * It gets that in place of the ones while the V.22bis modem
         * goes on listening; what a 212A caller then sends is what a
         * V.22 caller sends, and the start-up continues from there. */
        int tone = !a->connected && !a->used_v8 && (a->cfg.modes & ANS_MODE_BELL212A)
                   && a->v22.state == V22S_WAIT && a->t > MS(V22_BELL_MS);
        tx = v22m_tick(&a->v22, rx);
        if (tone) {
            int done;
            if (a->v22.state == V22S_WAIT) tx = v21mod_tick(&a->fmod, 1, &done);
            else a->via_bell = 1;
        }
    }
    if (!a->connected) {
        if (a->v22.tx_ready && a->v22.rx_ready) {
            a->connected = 1; a->bitrate = a->v22.bitrate;
            a->modulation = a->bitrate == 2400 ? ANS_MOD_V22BIS : a->via_bell ? ANS_MOD_BELL212A : ANS_MOD_V22;
            a->cd_run = 0;
        } else if (!a->used_v8 && (a->cfg.modes & V8_MOD_V21) && a->v22.state == V22S_WAIT
                   && !a->v22.rx.carrier && a->t > MS(V22_LEGACY_MS)) {
            ans_start_v21(a);
        } else if (a->t > MS(V22_TOTAL_MS) || a->v22.state == V22S_FAILED) {
            ans_fail(a, ANS_END_FAILED);
        }
    } else {
        if (a->v22.state == V22S_FAILED) ans_fail(a, ANS_END_LOST);   /* a retrain (V.22bis 6.4) that never completed */
        else if (a->v22.rx.carrier) a->cd_run = 0;
        else if (++a->cd_run > MS(CD_LOSS_MS)) ans_fail(a, ANS_END_LOST);
    }
    return tx;
}

static int16_t tick_v32(answer_t *a, int16_t rx)
{
    int16_t tx = v32_tick(&a->u.v32, rx);
    if (a->u.v32.state == V32S_FAILED) { ans_fail(a, ans_lost_or(a, ANS_END_FAILED)); return 0; }   /* carrier loss included */
    if (a->u.v32.connected) {
        a->connected = 1; a->modulation = ANS_MOD_V32BIS;
        a->bitrate = a->rx_bitrate = a->u.v32.bitrate;    /* may change at a retrain */
    }
    a->retraining = a->u.v32.retraining; a->retrains = a->u.v32.retrains;
    return tx;
}

static int16_t tick(answer_t *a, int16_t rx)
{
    switch (a->phase) {
    case ANS_V22: return tick_v22(a, rx);
    case ANS_V21: return tick_v21(a, rx);
    case ANS_V23: return tick_v23(a, rx);
    case ANS_V32_SIL:
        if (a->t >= MS(75)) ans_start_v32(a);
        return 0;
    default: return tick_v32(a, rx);         /* ANS_V32 */
    }
}

static int retrain(answer_t *a) { return a->phase == ANS_V32 ? v32_retrain(&a->u.v32) : -1; }

static int snr_den(const answer_t *a, int64_t *num, int64_t *den)
{
    if (a->modulation != ANS_MOD_V22 && a->modulation != ANS_MOD_V22BIS) return -1;
    /* 4096 = one unit squared; the points (1 and 3 units on each axis)
     * have a mean power of 10, and the four of 1200 bit/s have just that */
    *num = 10 * 4096; *den = a->v22.rx.mse;
    return 0;
}

const ans_scheme_t ans_legacy = { tick, NULL, retrain, NULL, snr_den, NULL, NULL, 0 };
