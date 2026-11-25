/* V.32bis receiver.
 *
 * Input: line samples with our own echo already removed (v32_ec.c).
 * Front end: a bank of fractional-delay complex band-pass matched filters
 * (root raised cosine around 1800 Hz) evaluated twice per symbol at the
 * instants the timing loop chooses, then brought to baseband.
 * Timing: band-edge recovery. Two narrow resonators at 600 and 3000 Hz (the
 * edges of the band, carrier -/+ half the symbol rate) run on the samples;
 * the product of one with the conjugate of the other is a 2400 Hz tone whose
 * phase at our sampling instants is the timing error. It needs no decisions
 * and no carrier lock, and S (ABAB..) puts pure tones on both edges.
 * Per symbol: T/2-spaced LMS equaliser, second-order carrier loop, then
 *   - training (5.2): S-bar and TRN against the known sequence,
 *   - 4-point decisions for the end of TRN and the rate signals (5.3),
 *   - data: Viterbi decoder (trellis rates) or 4-point decisions (4800).
 * Detectors on the raw T/2 samples serve the start-up: the 1800 Hz tone AA /
 * CC with its phase reversal (the sum over two symbols cancels our own AC
 * echo exactly), and the two-symbol periodicity of S.
 */
#include "v32_priv.h"
#include "sintab.h"
#include "fixmath.h"

#define U           V32_UNIT
#define TARGET_PW   ((int64_t)40 * U * U)       /* mean |point|^2 */
#define S_PW        ((int64_t)30 * U * U)       /* the same gain seen on S: 3/4 of the power */
#define HALF_STEP   (5 * V32_RXP * 256 / 3)     /* T/2 = 5/3 samples, in 1/(48*256) sample */
#define SAMPLE_STEP (V32_RXP * 256)
#define FREQ_MAX    17895697                    /* carrier loop: +/- 10 Hz, 2^32 per symbol cycle */
/* -43 dBm0: sine of RMS 111, envelope 157, with the 4 fractional bits of y */
#define CD_MIN      ((int64_t)2512 * 2512)
#define TRN_REF     1024                        /* TRN symbols trained against the reference (of >= 1280) */
#define EQ_CENTRE   (V32_EQN / 2)

/* resonator pole 1 - 2^-8 (about 10 Hz wide: the data's own noise on the band edges is large); e^{jw} for 600 and 3000 Hz at 8000 Hz, Q15 */
#define RES_SHIFT 8
#define L_COS 29197
#define L_SIN 14876
#define U_COS (-23170)
#define U_SIN 23170

static void reset_train(v32rx_t *r)
{
    int i;
    for (i = 0; i < V32_EQN; i++) { r->xr[i] = r->xi[i] = 0; r->wr[i] = r->wi[i] = 0; }
    r->theta = 0; r->freq = 0;
    r->t_integ = 0;
    r->gain = 0; r->agc_acc = 0; r->agc_n = 0;
    r->o1r = r->o1i = r->o2r = r->o2i = 0; r->sbar_neg = 0; r->s_lost = 0;
    r->nsym = 0; r->mse = 0;
    r->ref_idx = 0; r->ref_scr.reg = 0;
    r->dscr_dir.reg = 0; r->dscr.reg = 0;
    r->abcd = 0; r->trn_ones = 0; r->bits = 0; r->nbits = 0;
    r->rate_word = 0; r->rate_seen = 0; r->frame_bit = -1; r->e_word = 0;
    r->rate = 0; r->yprev = 0; r->ndata = 0; r->clamp = 0;
}

void v32rx_init(v32rx_t *r, int answer, v32_putbit_fn putbit, void *user)
{
    int i;
    r->answer = answer;
    r->mode = V32_RX_TONE;
    for (i = 0; i < 64; i++) r->ring[i] = 0;
    r->rpos = 0;
    r->tnext = HALF_STEP;
    r->cph = 0;
    r->dph = (uint32_t)(((uint64_t)1800 << 32) / 8000u);
    r->half = 0; r->nsamp = 0;
    r->lr = r->li = r->ur = r->ui = 0; r->te = 0;
    for (i = 0; i < 8; i++) r->yr[i] = r->yi[i] = 0;
    r->ypos = 0;
    r->pw_raw = 0; r->tone_p = r->tone_q = 0; r->tone_run = 0; r->pure_run = 0;
    r->rev_count = 0; r->rev_t = 0; r->rev_hold = 0;
    r->c4 = 0; r->sq = 0; r->s_run = 0;
    r->zr = r->zi = 0;
    r->putbit = putbit; r->user = user;
    r->ac_p = r->ac_m = 0; r->ac_run = r->ac_pure = 0;
    r->ac_rev = 0; r->ac_rev_t = 0; r->ac_hold = 0;
    v32_vit_init(&r->vit, 1);
    reset_train(r);
}

void v32rx_set_mode(v32rx_t *r, v32_rxmode mode)
{
    if (mode == V32_RX_S) reset_train(r);
    r->mode = mode;
    r->nsym = 0;
}

int v32rx_level(const v32rx_t *r)
{
    return (int)(isqrt_u64((uint64_t)r->pw_raw / 2) >> 4);     /* envelope^2 / 2 = RMS^2 */
}

/* ---------------- bits ---------------- */

/* a rate sequence or E has been completed on a frame boundary */
static void frame(v32rx_t *r, uint16_t w)
{
    if ((w & V32_RW_SYNC) == V32_RW_SYNC) {             /* E: B0-B3 ones */
        unsigned rates = v32_word_rates(w);
        int k, rate = -1;
        for (k = 0; k < V32_NRATES; k++) if (rates & (1u << k)) rate = k;
        r->e_word = w;
        if (rate < 0) return;                           /* E with no rate: the caller is clearing */
        r->rate = rate;
        v32_vit_init(&r->vit, rate ? rate : 1);
        r->mode = V32_RX_DATA;
        r->ndata = 0;
        r->clamp = 128;                                 /* 6.1: circuit 104 unclamped 128 T after E */
    } else if ((w & V32_RW_SYNC) == (V32_RW_BASE & V32_RW_SYNC)) {
        if (w == r->rate_word) { if (r->rate_seen < 30000) r->rate_seen++; }
        else { r->rate_word = w; r->rate_seen = 1; }
    }
}

/* one 4800 bit/s symbol decided as state s (0..3 = A..D) */
static void dd4_bits(v32rx_t *r, int s)
{
    int label = v32_abcd_label[s], q, i;
    /* TRN: the scrambled dibit is the label itself (Table 4) */
    for (i = 1; i >= 0; i--) {
        if (v32_descramble(&r->dscr_dir, !r->answer, (label >> i) & 1)) { if (r->trn_ones < 100000) r->trn_ones++; }
        else r->trn_ones = 0;
    }
    /* rate signals: Table 2 */
    q = v32_decode_bits(0, label, &r->yprev);
    for (i = 1; i >= 0; i--)
        r->bits = (r->bits << 1) | (uint64_t)v32_descramble(&r->dscr, !r->answer, (q >> i) & 1);
    if (r->nbits < 64) r->nbits += 2;
    if (r->frame_bit < 0) {
        /* 5.3.1: two consecutive identical sequences with the synchronising
         * bits right; asked here of a third one before them as well, since
         * the search runs through all of TRN */
        uint16_t w0 = (uint16_t)r->bits, w1 = (uint16_t)(r->bits >> 16), w2 = (uint16_t)(r->bits >> 32);
        uint16_t sync = V32_RW_BASE & V32_RW_SYNC;
        if (r->nbits >= 48 && w0 == w1 && (w0 & V32_RW_SYNC) == sync && (w2 & V32_RW_SYNC) == sync) {
            r->rate_word = w0; r->rate_seen = 2; r->frame_bit = 0;
        }
    } else {
        r->frame_bit += 2;
        if (r->frame_bit >= 16) { r->frame_bit = 0; frame(r, (uint16_t)r->bits); }
    }
}

static void data_bits(v32rx_t *r, int label)
{
    int n = 2 + r->rate, q = v32_decode_bits(r->rate, label, &r->yprev), i;
    for (i = n - 1; i >= 0; i--) {
        int b = v32_descramble(&r->dscr, !r->answer, (q >> i) & 1);
        if (!r->clamp && r->putbit) r->putbit(r->user, b);
    }
    if (r->clamp) r->clamp--;
}

/* the reference symbol for training: S, S-bar, then TRN (5.2) */
static int ref_state(v32rx_t *r)
{
    int idx = r->ref_idx++, q;
    if (idx < 0) return idx & 1;                       /* S ends .. A B */
    if (idx < 16) return 2 + (idx & 1);                /* C D */
    if (idx == 16) r->ref_scr.reg = 0;
    q = v32_scramble(&r->ref_scr, !r->answer, 1) << 1;
    q |= v32_scramble(&r->ref_scr, !r->answer, 1);
    return idx - 16 < 256 ? ((q & 2) ? 2 : 0) : v32_label_abcd[q];
}

/* nearest of A B C D: times (2 + j) the decision regions are the quadrants */
static int slice4(int32_t zr, int32_t zi)
{
    int32_t a = 2 * zr - zi, b = 2 * zi + zr;
    if (a >= 0) return b >= 0 ? 2 : 1;
    return b >= 0 ? 3 : 0;
}

/* ---------------- one symbol ---------------- */
static void symbol(v32rx_t *r)
{
    int64_t ar = 0, ai = 0;
    int32_t zr, zi, c, s, pr, pi_, er, ei, fr, fi, im, perr, e2;
    int dx, dy, k, st = -1, out = -1, sh, kp, kf, rnd;

    for (k = 0; k < V32_EQN; k++) {
        ar += (int64_t)r->wr[k] * r->xr[k] - (int64_t)r->wi[k] * r->xi[k];
        ai += (int64_t)r->wr[k] * r->xi[k] + (int64_t)r->wi[k] * r->xr[k];
    }
    zr = (int32_t)(ar >> 24); zi = (int32_t)(ai >> 24);
    if (zr > 16 * U) zr = 16 * U; else if (zr < -16 * U) zr = -16 * U;
    if (zi > 16 * U) zi = 16 * U; else if (zi < -16 * U) zi = -16 * U;

    /* carrier loop: z . exp(-j theta) */
    s = sintab[(r->theta >> 22) & SIN_MASK];
    c = sintab[((r->theta >> 22) + SIN_N / 4) & SIN_MASK];
    pr  = (zr * c + zi * s) >> 15;
    pi_ = (zi * c - zr * s) >> 15;
    r->zr = pr; r->zi = pi_;

    switch (r->mode) {
    case V32_RX_TRAIN:
        st = ref_state(r);
        v32_state_point(st, &dx, &dy);
        sh = 6; kp = 20861; kf = 326;
        break;
    case V32_RX_DD4:
        st = slice4(pr, pi_);
        v32_state_point(st, &dx, &dy);
        sh = 8; kp = 20861; kf = 326;
        break;
    default:
        if (r->rate == 0) {
            st = slice4(pr, pi_);
            v32_state_point(st, &dx, &dy);
            out = v32_abcd_label[st];
        } else {
            int hard;
            out = v32_vit_step(&r->vit, pr, pi_, &hard);
            v32_point(r->rate, hard, &dx, &dy);
        }
        sh = 10; kp = 5215; kf = 20;
        break;
    }

    er = pr - dx * U; ei = pi_ - dy * U;
    e2 = ((er >> 3) * (er >> 3) + (ei >> 3) * (ei >> 3));        /* 4096 per unit^2 */
    r->mse += (e2 - r->mse) >> 5;

    /* carrier loop update: phase error in radians, Q12 */
    im = pi_ * dx - pr * dy;
    perr = im * 8 / (dx * dx + dy * dy);
    if (perr > 2048) perr = 2048; else if (perr < -2048) perr = -2048;
    r->freq += perr * kf;
    if (r->freq > FREQ_MAX) r->freq = FREQ_MAX; else if (r->freq < -FREQ_MAX) r->freq = -FREQ_MAX;
    r->theta += (uint32_t)(perr * kp + r->freq);

    /* equaliser update, error rotated back in front of the carrier loop */
    fr = (er * c - ei * s) >> 15;
    fi = (ei * c + er * s) >> 15;
    rnd = 1 << (sh - 1);
    for (k = 0; k < V32_EQN; k++) {
        r->wr[k] -= (fr * r->xr[k] + fi * r->xi[k] + rnd) >> sh;
        r->wi[k] -= (fi * r->xr[k] - fr * r->xi[k] + rnd) >> sh;
    }

    r->nsym++;
    switch (r->mode) {
    case V32_RX_TRAIN:
        r->abcd = st;
        if (r->ref_idx >= 16 + TRN_REF) {
            /* from here on by decisions: the caller's TRN may end at 1280 */
            r->mode = V32_RX_DD4; r->nsym = 0;
            r->dscr_dir.reg = r->ref_scr.reg; r->dscr.reg = r->ref_scr.reg;
            r->yprev = v32_abcd_label[st];
            r->trn_ones = 0; r->bits = 0; r->nbits = 0; r->frame_bit = -1;
        }
        break;
    case V32_RX_DD4:
        r->abcd = st;
        dd4_bits(r, st);
        break;
    default:
        r->ndata++;
        if (out >= 0) data_bits(r, out);
        break;
    }
}

/* ---------------- timing: band-edge phase at the instant p/48 sample back ---------------- */
static void timing(v32rx_t *r, int p)
{
    int64_t qr = (int64_t)r->ur * r->lr + (int64_t)r->ui * r->li;
    int64_t qi = (int64_t)r->ui * r->lr - (int64_t)r->ur * r->li;
    int32_t a, b, c, s, xr, xi, den, e;
    int idx = (p * 32) / 5;                     /* 0.3 cycle per sample, 1024ths of a cycle */
    int64_t m = (qr < 0 ? -qr : qr) + (qi < 0 ? -qi : qi);
    if (m < 16) { r->te = 0; return; }          /* nothing on the band edges */
    while (m >= ((int64_t)1 << 29)) { qr >>= 1; qi >>= 1; m >>= 1; }
    while (m < ((int64_t)1 << 28)) { qr *= 2; qi *= 2; m *= 2; }
    a = (int32_t)(qr >> 15); b = (int32_t)(qi >> 15);
    s = sintab[idx & SIN_MASK];
    c = sintab[(idx + SIN_N / 4) & SIN_MASK];
    xr = a * c + b * s;                         /* q . exp(-j phi), about 2^30 at most */
    xi = b * c - a * s;
    den = (xr < 0 ? -xr : xr) + (xi < 0 ? -xi : xi);
    if (den < 1024) { r->te = 0; return; }
    e = xr > 0 ? xi / (den >> 10) : (xi >= 0 ? 1024 : -1024);
    if (e > 1024) e = 1024; else if (e < -1024) e = -1024;
    r->te = e;
    if (r->mode == V32_RX_S) {
        r->tnext -= e * 2;                      /* S: pure tones, pull in fast */
    } else {
        /* a late instant shows as a positive phase: move earlier. The
         * integrator holds the clock-rate difference. */
        r->t_integ += e;
        if (r->t_integ > 400000) r->t_integ = 400000; else if (r->t_integ < -400000) r->t_integ = -400000;
        r->tnext -= e / 32 + (r->t_integ >> 14);
    }
}

/* ---------------- calling modem: the answerer's AC (6.1, 7.1) ----------------
 * AC (A C A C ..) at baseband changes sign every symbol: the lines at 600 and
 * 3000 Hz. Half the difference of T/2 samples a symbol apart keeps all of it
 * and none of an 1800 Hz tone (our own AA / CC echo, the canceller not trained
 * yet). Two neighbouring T/2 samples of it taken together do not depend on
 * the sampling phase: for AC their product with the pair a symbol back is
 * -|a|^2, with the pair two symbols back +|a|^2, and a reversal makes the
 * latter negative. A tone near 1800 Hz (V.21 at 1650, ANS at 2100) turns
 * little in a symbol, so its product a symbol back is positive; data gives
 * about none. S has these lines too, but never alone (ac_pure). */
#define AC_MIN (CD_MIN / 4)       /* -43 dBm0: the lines are 6 dB down through the two shaping filters */
#define DIF(v, k) (((v)[(r->ypos - (k)) & 7] - (v)[(r->ypos - (k) - 2) & 7]) >> 1)
static void ac_detect(v32rx_t *r)
{
    int32_t a0r = DIF(r->yr, 0), a0i = DIF(r->yi, 0), a1r = DIF(r->yr, 1), a1i = DIF(r->yi, 1);
    int32_t b0r = DIF(r->yr, 2), b0i = DIF(r->yi, 2), b1r = DIF(r->yr, 3), b1i = DIF(r->yi, 3);
    int32_t c0r = DIF(r->yr, 4), c0i = DIF(r->yi, 4), c1r = DIF(r->yr, 5), c1i = DIF(r->yi, 5);
    int64_t pw = ((int64_t)a0r * a0r + (int64_t)a0i * a0i + (int64_t)a1r * a1r + (int64_t)a1i * a1i) >> 1;
    int64_t m1 = ((int64_t)a0r * b0r + (int64_t)a0i * b0i + (int64_t)a1r * b1r + (int64_t)a1i * b1i) >> 1;
    int64_t m2 = ((int64_t)a0r * c0r + (int64_t)a0i * c0i + (int64_t)a1r * c1r + (int64_t)a1i * c1i) >> 1;
    int ok;
    if (r->ac_hold > 0) { r->ac_hold--; return; }
    if (r->ac_run >= 32 && m2 < -(r->ac_p >> 2)) {
        /* turned over: the newest pair is past the reversal */
        r->ac_rev++; r->ac_rev_t = r->nsamp; r->ac_hold = 10;
        return;
    }
    r->ac_p += (pw - r->ac_p) >> 3;
    r->ac_m += (m1 - r->ac_m) >> 3;
    ok = r->ac_p > AC_MIN && -r->ac_m * 4 > r->ac_p * 3;
    /* (not straight to zero: a reversal half way through the pair can spoil
     * the product a sample before the one two symbols back turns over) */
    if (ok) { if (r->ac_run < 1000000) r->ac_run++; } else r->ac_run = r->ac_run > 8 ? r->ac_run - 8 : 0;
    if (ok && r->ac_p * 4 > r->pw_raw * 3) { if (r->ac_pure < 1000000) r->ac_pure++; } else r->ac_pure = 0;
}
#undef DIF

/* ---------------- one T/2 sample (raw, 4 fractional bits) ---------------- */
static void half_symbol(v32rx_t *r, int32_t yr, int32_t yi, int p)
{
    int64_t pw, inst, q4, cc;
    int32_t vr = 0, vi = 0, wr = 0, wi = 0, gr, gi, o4r, o4i;
    int k, lax;

    pw = (int64_t)yr * yr + (int64_t)yi * yi;
    r->pw_raw += (pw - r->pw_raw) >> 4;

    /* ---- detectors ---- */
    o4r = r->yr[(r->ypos - 3) & 7]; o4i = r->yi[(r->ypos - 3) & 7];      /* 4 half symbols ago */
    r->ypos = (r->ypos + 1) & 7;
    r->yr[r->ypos] = yr; r->yi[r->ypos] = yi;
    for (k = 0; k < 4; k++) {
        vr += r->yr[(r->ypos - k) & 7] >> 2; vi += r->yi[(r->ypos - k) & 7] >> 2;
        wr += r->yr[(r->ypos - 4 - k) & 7] >> 2; wi += r->yi[(r->ypos - 4 - k) & 7] >> 2;
    }
    /* v, w: means over this and the previous two symbols: only 1800 Hz survives */
    inst = (int64_t)vr * wr + (int64_t)vi * wi;
    q4 = (int64_t)vr * vr + (int64_t)vi * vi;
    if (r->rev_hold > 0) r->rev_hold--;
    else {
        if (r->tone_run >= 32 && inst < -(r->tone_q >> 2)) {
            /* the tone turned over: three of the four newest samples are past it */
            r->rev_count++; r->rev_t = r->nsamp; r->rev_hold = 10;
        } else {
            r->tone_p += (inst - r->tone_p) >> 3;
            r->tone_q += (q4 - r->tone_q) >> 3;
        }
        lax = r->tone_p > CD_MIN && r->tone_p * 4 > r->tone_q * 3;
        if (lax) { if (r->tone_run < 1000000) r->tone_run++; } else r->tone_run = 0;
        if (lax && r->tone_p * 4 > r->pw_raw * 3) { if (r->pure_run < 1000000) r->pure_run++; } else r->pure_run = 0;
    }
    /* S / S-bar: period of two symbols, and half of the power at 1800 Hz
     * (all of it would be AA, none of it AC) */
    cc = (int64_t)yr * o4r + (int64_t)yi * o4i;
    r->c4 += (cc - r->c4) >> 4;
    r->sq += (q4 - r->sq) >> 4;
    /* (through both shaping filters the 600 and 3000 Hz lines of S are 6 dB
     * down: 1800 Hz is 0.8 of what arrives, less with noise) */
    if (r->pw_raw > CD_MIN && r->c4 * 5 > r->pw_raw * 3 && r->sq * 4 > r->pw_raw && r->sq * 10 < r->pw_raw * 9) {
        if (r->s_run < 1000000) r->s_run++;
    } else r->s_run = 0;
    if (!r->answer) ac_detect(r);

    if (r->mode == V32_RX_TONE) { r->half ^= 1; return; }

    /* ---- gain: set once, from S ---- */
    if (r->gain == 0) {
        if (r->agc_n >= 16) r->agc_acc += pw;           /* the first 8 symbols settle the filters */
        if (++r->agc_n == 48) {
            uint64_t avg = (uint64_t)(r->agc_acc >> 5);
            if (avg < 1) avg = 1;
            r->gain = (int32_t)isqrt_u64((((uint64_t)S_PW) << 32) / avg);
            if (r->gain < 1) r->gain = 1;
        }
        r->half ^= 1;
        return;
    }
    gr = (int32_t)(((int64_t)yr * r->gain) >> 16);
    gi = (int32_t)(((int64_t)yi * r->gain) >> 16);
    if (gr > 32 * U) gr = 32 * U; else if (gr < -32 * U) gr = -32 * U;
    if (gi > 32 * U) gi = 32 * U; else if (gi < -32 * U) gi = -32 * U;

    for (k = V32_EQN - 1; k > 0; k--) { r->xr[k] = r->xr[k - 1]; r->xi[k] = r->xi[k - 1]; }
    r->xr[0] = gr; r->xi[0] = gi;

    r->half ^= 1;
    if (r->half) return;                                /* mid-symbol sample */

    timing(r, p);

    if (r->mode == V32_RX_S) {
        /* S-bar: the on-time samples turn over against those two symbols back */
        int64_t pp = (int64_t)gr * r->o2r + (int64_t)gi * r->o2i;
        /* the calling modem: what looked like S may have been the end of the
         * answerer's AC, before its 16 T of silence and the real S (5.2):
         * four symbols with next to nothing on time and it was not S; wait
         * for S again. (Taken for S, the AC set the gain and the timing, and
         * the real S's start passed for S-bar: the receiver trained on S
         * and read R1 with an error of 25000, so R2 offered 4800 only.) */
        if (!r->answer) {
            if ((int64_t)gr * gr + (int64_t)gi * gi < (TARGET_PW >> 4)) {
                if (++r->s_lost >= 4) { r->mode = V32_RX_TONE; r->s_run = 0; r->s_lost = 0; return; }
            } else r->s_lost = 0;
        }
        r->o2r = r->o1r; r->o2i = r->o1i; r->o1r = gr; r->o1i = gi;
        r->nsym++;
        if (r->nsym > 4 && pp < -(TARGET_PW >> 3)) r->sbar_neg++; else r->sbar_neg = 0;
        if (r->sbar_neg < 2) return;
        /* this is the second symbol of S-bar; the equaliser's centre tap is
         * EQ_CENTRE / 2 symbols behind */
        r->mode = V32_RX_TRAIN; r->nsym = 0;
        r->ref_idx = 1 - EQ_CENTRE / 2;
    }
    symbol(r);
}

void v32rx_tick(v32rx_t *r, int32_t sample)
{
    int32_t x = sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample, t;

    r->ring[r->rpos] = (int16_t)x;
    r->rpos = (r->rpos + 1) & 63;
    r->nsamp++;

    /* band-edge resonators: y = (1 - 2^-8) e^{jw} y + x */
    t = (int32_t)(((int64_t)r->lr * L_COS - (int64_t)r->li * L_SIN) >> 15);
    r->li = (int32_t)(((int64_t)r->li * L_COS + (int64_t)r->lr * L_SIN) >> 15);
    r->lr = t - (t >> RES_SHIFT) + x; r->li -= r->li >> RES_SHIFT;
    t = (int32_t)(((int64_t)r->ur * U_COS - (int64_t)r->ui * U_SIN) >> 15);
    r->ui = (int32_t)(((int64_t)r->ui * U_COS + (int64_t)r->ur * U_SIN) >> 15);
    r->ur = t - (t >> RES_SHIFT) + x; r->ui -= r->ui >> RES_SHIFT;

    r->cph += r->dph;
    r->tnext -= SAMPLE_STEP;
    if (r->tnext <= 0) {
        int p = (int)((-r->tnext) >> 8), i, idx = r->rpos - 1;
        const int16_t (*g)[2];
        int64_t ar = 0, ai = 0, br, bi;
        int32_t c, s, yr, yi;
        uint32_t ph;
        if (p >= V32_RXP) p = V32_RXP - 1;
        g = v32_rxg[p];
        for (i = 0; i < V32_RXN; i++) {
            int32_t v = r->ring[(idx - i) & 63];
            ar += v * g[i][0];
            ai += v * g[i][1];
        }
        br = ar >> 10; bi = ai >> 10;                   /* Q14 taps: 4 fractional bits kept */
        /* to baseband, with the carrier phase of the output instant (p/48 sample back) */
        ph = r->cph - (uint32_t)(((uint64_t)r->dph * (uint32_t)p) / V32_RXP);
        s = sintab[(ph >> 22) & SIN_MASK];
        c = sintab[((ph >> 22) + SIN_N / 4) & SIN_MASK];
        yr = (int32_t)((br * c + bi * s) >> 15);
        yi = (int32_t)((bi * c - br * s) >> 15);
        r->tnext += HALF_STEP;
        half_symbol(r, yr, yi, p);
        if (r->tnext <= 0) r->tnext = 1;
    }
}

/* ---------------- AA detector (see v32.h) ---------------- */
void v32_aadet_init(v32_aadet_t *d)
{
    d->ph = d->ph2 = 0;
    d->i1 = d->q1 = d->i2 = d->q2 = 0;
    d->j1 = d->r1 = d->j2 = d->r2 = 0;
    d->pw = d->pw2 = 0; d->run = 0;
}

int v32_aadet_tick(v32_aadet_t *d, int16_t rx)
{
    int32_t c, s;
    d->ph += (uint32_t)(((uint64_t)1800 << 32) / 8000u);
    s = sintab[(d->ph >> 22) & SIN_MASK];
    c = sintab[((d->ph >> 22) + SIN_N / 4) & SIN_MASK];
    /* to 0 Hz, then two one-pole low-passes of about 20 Hz: the echo of our
     * own 2100 Hz answer tone, 300 Hz away, is 46 dB down */
    d->i1 += (((rx * c) >> 7) - d->i1) >> 6;
    d->q1 += (((rx * s) >> 7) - d->q1) >> 6;
    d->i2 += (d->i1 - d->i2) >> 6;
    d->q2 += (d->q1 - d->q2) >> 6;
    d->pw = (int64_t)d->i2 * d->i2 + (int64_t)d->q2 * d->q2;    /* (A / 2)^2 x 2^16 */
    /* the same at 2400 Hz, as far above the answer tone as 1800 Hz is below
     * it: the splash of its phase reversals lands on both alike, and so does
     * noise; AA is at 1800 Hz only */
    d->ph2 += (uint32_t)(((uint64_t)2400 << 32) / 8000u);
    s = sintab[(d->ph2 >> 22) & SIN_MASK];
    c = sintab[((d->ph2 >> 22) + SIN_N / 4) & SIN_MASK];
    d->j1 += (((rx * c) >> 7) - d->j1) >> 6;
    d->r1 += (((rx * s) >> 7) - d->r1) >> 6;
    d->j2 += (d->j1 - d->j2) >> 6;
    d->r2 += (d->r1 - d->r2) >> 6;
    d->pw2 = (int64_t)d->j2 * d->j2 + (int64_t)d->r2 * d->r2;
    /* -43 dBm0: A = 157; and 12 dB over the other band */
    if (d->pw > ((int64_t)157 * 157 / 4) << 16 && d->pw > d->pw2 * 16) { if (d->run < 1000000) d->run++; } else d->run = 0;
    return d->run >= V32_AADET_RUN;
}
