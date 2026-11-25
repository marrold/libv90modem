/* See v8orig.h. */
#include <string.h>
#include "v8orig.h"
#include "sintab.h"

#define FS 8000
#define MS(ms) ((int32_t)((int64_t)(ms) * FS / 1000))

/* CI cadence (7.1): ON for 3 sequences, OFF for 1 s; the first OFF period is
 * the 1 s of silence 8.1.1 asks for before the call signal */
#define CI_SEQS    3
#define CI_OFF_MS  1000
/* carrier squelch of the JM receiver: below about -46 dBm0 it reads idle mark
 * (as in v8ans) */
#define SQUELCH_AMP 110
#define PREAMBLE    10      /* the ten ONEs before the sync character */

static void go(v8orig_t *v, v8o_state s) { v->state = s; v->t = 0; }

/* JM follows ANSam with no gap (8.2.2, 8.2.3), and the V.21(H) receiver
 * reads 2100 Hz as characters: one in progress when JM begins takes the ten
 * ONEs before JM's sync, and that JM is not seen as one (a third JM was
 * then needed). So while the 2100 Hz tone is heard the receiver is held in
 * idle mark. A 5 ms block is the tone when its 2100 Hz component holds half
 * of its power and is above -43 dBm0 (modem/anstone's test); the hold lasts
 * two blocks more, over a phase reversal and the block where JM begins.
 * 1 while the receiver is to be held. */
#define G_BLOCK 40
#define G_HOLD  2
static int ansam_gate(v8orig_t *v, int16_t x)
{
    v->g_i += ((int32_t)x * sintab[((v->g_ph + 0x40000000u) >> 22) & SIN_MASK]) >> 15;
    v->g_q += ((int32_t)x * sintab[(v->g_ph >> 22) & SIN_MASK]) >> 15;
    v->g_pow += (int32_t)x * x;
    v->g_ph += (uint32_t)(((uint64_t)2100 << 32) / FS);
    if (++v->g_n == G_BLOCK) {
        int64_t a2 = (int64_t)v->g_i * v->g_i + (int64_t)v->g_q * v->g_q;
        /* -43 dBm0 (RMS 112): a block amplitude of 112 * sqrt(2) * 40 / 2,
         * three quarters of it */
        int64_t min_a = (int64_t)112 * 1414 * G_BLOCK * 3 / (2 * 4 * 1000);
        if (a2 >= min_a * min_a && 4 * a2 >= (int64_t)G_BLOCK * v->g_pow) v->g_hold = G_HOLD + 1;
        else if (v->g_hold > 0) v->g_hold--;
        v->g_i = v->g_q = 0; v->g_pow = 0; v->g_n = 0;
    }
    return v->g_hold > 0;
}

void v8orig_init(v8orig_t *v, const v8orig_cfg *cfg)
{
    uint8_t oct[V8_MAX_OCTETS];
    memset(v, 0, sizeof *v);
    v->cfg = *cfg;
    if (v->cfg.te_ms <= 0) v->cfg.te_ms = 1000;
    if (v->cfg.te_ms < 500) v->cfg.te_ms = 500;          /* 8.1.1: Te is at least 0.5 s */
    if (v->cfg.tone_ms <= 0) v->cfg.tone_ms = 10000;
    if (v->cfg.jm_ms <= 0) v->cfg.jm_ms = 5000;
    anst_init(&v->tone, 0);
    v21mod_init(&v->mod, V21L_MARK, V21L_SPACE, FS, 300, v->cfg.amp);
    v21demod_init(&v->dem, V21H_MARK, V21H_SPACE, FS, 300);
    v8rx_init(&v->rx, FS);
    v->rx.jm_flip = 1;              /* an x2 server's JM inverts after each (modem/v8rx.h) */
    v->squelch = v21demod_energy_of(&v->dem, SQUELCH_AMP);
    if (v->cfg.send_ci) {
        /* CI: ten ONEs, the CI sync, the call function octet (7.1) */
        v8_encode(&v->cfg.local, oct, V8_MAX_OCTETS);
        v->nbits = v8_frame(V8_SYNC_CI, oct, 1, v->bits, V8O_MAXBITS);
    }
    v->state = V8O_LISTEN;
    v->result = V8O_RES_NONE;
    v->answer_tone = ANST_NONE;
}

/* the next bit of the sequence in v->bits has to go out: is this a place
 * where CJ may start (8.1.2: "complete the current octet and associated start
 * and stop bits")? In the ONEs before the sync every bit is one. */
static int at_octet_boundary(const v8orig_t *v)
{
    return v->pos <= PREAMBLE || (v->pos - PREAMBLE) % 10 == 0;
}

static void start_cm(v8orig_t *v)
{
    uint8_t oct[V8_MAX_OCTETS];
    int n = v8_encode(&v->cfg.local, oct, V8_MAX_OCTETS);
    v->nbits = v8_frame(V8_SYNC_CM_JM, oct, n, v->bits, V8O_MAXBITS);
    v->pos = 0;
    v->at_cm = v->n + 1;            /* the first sample of CM is the next one */
    go(v, V8O_CM);
}

static void start_cj(v8orig_t *v)
{
    static const uint8_t zero[3] = { 0, 0, 0 };
    int i, k = 0;
    /* three zero octets, each framed: start bit, eight 0, stop bit */
    for (i = 0; i < 3; i++) {
        int b;
        v->bits[k++] = 0;
        for (b = 0; b < 8; b++) v->bits[k++] = (uint8_t)((zero[i] >> b) & 1);
        v->bits[k++] = 1;
    }
    v->nbits = k;
    v->pos = 0;
    v->at_cj = v->n;
    go(v, V8O_CJ);
}

v8o_state v8orig_tick(v8orig_t *v, int16_t rx, int16_t *tx)
{
    int done;
    *tx = 0;
    switch (v->state) {
    case V8O_LISTEN:
        v->t++;
        if (v->cfg.send_ci && v->ci_on) {
            /* sending CI: the answer tone is not listened for under it */
            *tx = v21mod_tick(&v->mod, v->bits[v->pos], &done);
            if (done && ++v->pos >= v->nbits) {
                v->pos = 0;
                v->ci_sent++;
                if (++v->ci_seqs >= CI_SEQS) { v->ci_on = 0; v->ci_seqs = 0; v->ci_off = 0; anst_init(&v->tone, 0); }
            }
        } else {
            anst_result r = anst_tick(&v->tone, rx);
            if (r == ANST_ANSAM || r == ANST_ANS) {
                v->answer_tone = r;
                v->at_tone = v->n + 1;
                if (r == ANST_ANS) { v->result = V8O_RES_ANS; go(v, V8O_DONE); }
                else go(v, V8O_TE);
            } else if (v->cfg.send_ci && ++v->ci_off >= MS(CI_OFF_MS)) {
                v->ci_on = 1; v->pos = 0;
            }
        }
        if (v->state == V8O_LISTEN && v->t >= MS(v->cfg.tone_ms)) { v->result = V8O_RES_NO_TONE; go(v, V8O_DONE); }
        break;
    case V8O_TE:
        if (++v->t >= MS(v->cfg.te_ms)) start_cm(v);
        break;
    case V8O_CM: {
        int level = v21demod_tick(&v->dem, rx), held = ansam_gate(v, rx), ev;
        if (v->dem.energy < v->squelch || held) level = 1;
        ev = v8rx_tick(&v->rx, level);
        if ((ev & V8RX_MENU) && !v->jm_seen && v8_decode(v->rx.msg, v->rx.nmsg, &v->jm) == 0) v->jm_seen = 1;
        *tx = v21mod_tick(&v->mod, v->bits[v->pos], &done);
        v->t++;
        if (done) {
            if (++v->pos >= v->nbits) v->pos = 0;
            if (v->jm_seen && at_octet_boundary(v)) { v->n++; start_cj(v); return v->state; }
        }
        if (v->t >= MS(v->cfg.jm_ms)) { v->result = V8O_RES_NO_JM; go(v, V8O_DONE); }
        break;
    }
    case V8O_CJ:
        *tx = v21mod_tick(&v->mod, v->bits[v->pos], &done);
        if (done && ++v->pos >= v->nbits) {
            v->selected = v8_selected_caller(&v->jm);
            v->result = v->selected ? V8O_RES_OK : V8O_RES_NO_COMMON;
            v->at_post = v->n + 1;
            go(v, V8O_POST);
        }
        break;
    case V8O_POST:
        if (++v->t >= MS(V8O_POST_MS)) go(v, V8O_DONE);
        break;
    case V8O_DONE:
        break;
    }
    v->n++;
    return v->state;
}
