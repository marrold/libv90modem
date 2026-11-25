/* V.22bis receiver.
 *
 * Front end: a bank of fractional-delay complex band-pass matched filters
 * (v22_tabs.h) evaluated twice per symbol at the instants chosen by the timing
 * loop, then derotated to baseband. Back end, per symbol: AGC, Gardner timing
 * detector, T/2-spaced LMS equaliser, decision-directed carrier loop, slicer,
 * differential decoder, descrambler.
 *
 * A normalised two-symbol-lag correlation classifies the unscrambled handshake
 * signals without needing any loop to be locked: it is +1 for the double dibit
 * 00,11 pattern (period 2 symbols), -1 for unscrambled binary 1 (-90 deg per
 * symbol, so 180 deg per two symbols), near 0 for scrambled data.
 */
#include "v22bis.h"
#include "v22_tabs.h"
#include "sintab.h"
#include "fixmath.h"

#define U           V22_UNIT
#define TARGET_PW   (10 * U * U)            /* mean |point|^2, both rates */
#define HALF_STEP   (20 * V22_RXP * 256 / 3) /* T/2 = 20/3 samples, in 1/(24*256) sample */
#define SAMPLE_STEP (V22_RXP * 256)

/* carrier detect on the smoothed raw envelope power: ON above -43 dBm0, OFF
 * below -48 dBm0 (V.22bis 3.3); envelope amplitude = sine peak */
/* carrier loop frequency limit: +/- 12 Hz in units of 2^32 per symbol */
#define FREQ_MAX 85899345

#define CD_ON   (158 * 158)
#define CD_OFF  (89 * 89)

static void q_reset(v22rx_t *r)
{
    int i;
    for (i = 0; i < 3; i++) r->q_cur[i] = r->q_prev[i] = r->q_tot[i] = 0;
    r->q_n = 0; r->q_have_prev = 0; r->q_chunks = 0;
}

static void reset_tracking(v22rx_t *r)
{
    int i;
    for (i = 0; i < V22_EQN; i++) { r->xr[i] = r->xi[i] = 0; r->wr[i] = r->wi[i] = 0; }
    r->wr[V22_EQN / 2] = 1 << 24;
    r->theta = 0; r->freq = 0;
    r->t_integ = 0;
    r->on_r = r->on_i = r->mid_r = r->mid_i = 0;
    r->quadrant = 0;
    r->ones_run = 0; r->zeros_run = 0;
    r->nsym = 0; r->ndata = 0;
    r->mse = 0; r->mse_slow = 0; r->gated = 0; r->mse_long = 0;
    r->pw_on = TARGET_PW;
    r->agc_count = 0; r->agc_acc = 0;
    r->gain = 0;
}

void v22rx_init(v22rx_t *r, int fc, v22_putbit_fn putbit, void *user)
{
    int i;
    r->bank = (fc == V22_FC_LOW) ? v22_rx_lo : v22_rx_hi;
    for (i = 0; i < 128; i++) r->ring[i] = 0;
    r->rpos = 0;
    r->tnext = HALF_STEP;
    r->cph = 0;
    r->dph = (uint32_t)(((uint64_t)(uint32_t)fc << 32) / 8000u);
    r->half = 0;
    r->pw_raw = 0;
    r->carrier = 0;
    for (i = 0; i < 4; i++) r->hr[i] = r->hi[i] = 0;
    r->hpos = 0;
    r->c2r = 0; r->e2 = 0;
    r->sig = V22_SIG_NONE; r->sig_cand = V22_SIG_NONE; r->sig_count = 0;
    r->mode16 = 0;
    scr_init(&r->dscr);
    r->zr = r->zi = 0;
    r->putbit = putbit; r->user = user;
    r->data_on = 0; r->clamp = 0;
    r->p_head = r->p_n = 0;
    r->dx[0] = r->dx[1] = r->dy[0] = r->dy[1] = 0;
    r->rep = r->brk = r->susp = 0;
    r->s1_strong = 0; r->sig_age = 0;
    q_reset(r);
    reset_tracking(r);
}

int v22rx_s1_certain(const v22rx_t *r) { return r->sig == V22_SIG_S1 && r->s1_strong >= 8; }

void v22rx_clamp(v22rx_t *r, int on)
{
    r->clamp = on;
    r->p_n = 0;
    r->rep = r->brk = r->susp = 0;
}

void v22rx_retrain(v22rx_t *r, int lost)
{
    r->mode16 = 0;
    r->ones_run = r->zeros_run = 0;
    if (lost) { r->freq = 0; r->t_integ = 0; }
}

int v22rx_s1_snr(const v22rx_t *r)
{
    int64_t cr = r->q_tot[0], ci = r->q_tot[1], e = r->q_tot[2], mag, noise, ratio;
    if (r->q_chunks < 2) return -1;
    while (e >= ((int64_t)1 << 30)) { e >>= 1; cr >>= 1; ci >>= 1; }
    mag = (int64_t)isqrt_u64((uint64_t)(cr * cr + ci * ci));
    noise = e - mag;
    if (noise < 1) noise = 1;
    ratio = mag / noise;
    return ratio > 100000 ? 100000 : (int)ratio;
}

int v22rx_level(const v22rx_t *r)
{
    if (!r->carrier) return 0;
    return (int)isqrt_u64((uint64_t)r->pw_raw / 2);    /* envelope^2 / 2 = RMS^2 */
}

/* deliver queued bits, oldest first, until `keep` are left */
static void pend_deliver(v22rx_t *r, int keep)
{
    while (r->p_n > keep) {
        int bit = (r->pend[r->p_head >> 3] >> (r->p_head & 7)) & 1;
        r->p_head = (r->p_head + 1) & 511; r->p_n--;
        if (r->data_on && r->putbit) r->putbit(r->user, bit);
    }
}

static void rx_bit(v22rx_t *r, int ds)
{
    int b = v22_descramble(&r->dscr, ds);
    if (b) { if (r->ones_run < 1000000) r->ones_run++; r->zeros_run = 0; }
    else   { if (r->zeros_run < 1000000) r->zeros_run++; r->ones_run = 0; }
    if (!r->data_on || !r->putbit) return;
    if (r->clamp) {
        /* circuit 104 clamped to binary 1 (6.4 a); the clock stays at 2400
         * bit/s through a retrain, also while the line runs at 1200 */
        r->putbit(r->user, 1);
        if (!r->mode16) r->putbit(r->user, 1);
    }
    else if (r->mode16) {
        /* queue; see V22_RX_HOLD */
        int at = (r->p_head + r->p_n) & 511;
        if (b) r->pend[at >> 3] |= (uint8_t)(1 << (at & 7)); else r->pend[at >> 3] &= (uint8_t)~(1 << (at & 7));
        r->p_n++;
        if (r->susp && r->p_n >= 500) r->susp = 0;       /* S1 never lasts that long: it was data */
        if (!r->susp) pend_deliver(r, V22_RX_HOLD);
    }
    else r->putbit(r->user, b);
}

/* nearest point: 16-way, or the four "01" elements used at 1200 bit/s */
static void slice(const v22rx_t *r, int32_t zr, int32_t zi, int *x, int *y)
{
    if (r->mode16) {
        *x = zr >= 0 ? (zr >= 2 * U ? 3 : 1) : (zr < -2 * U ? -3 : -1);
        *y = zi >= 0 ? (zi >= 2 * U ? 3 : 1) : (zi < -2 * U ? -3 : -1);
    } else {
        /* multiply by (2 + j): rotates (3,1) onto the 45 degree diagonal, so
         * the four decision regions become the four quadrants */
        int32_t a = 2 * zr - zi, b = 2 * zi + zr;
        if (a >= 0 && b >= 0)      { *x = 3;  *y = 1; }
        else if (a < 0 && b >= 0)  { *x = -1; *y = 3; }
        else if (a < 0 && b < 0)   { *x = -3; *y = -1; }
        else                       { *x = 1;  *y = -3; }
    }
}

static int32_t sat32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; }

static void symbol(v22rx_t *r)
{
    static const int dib[4] = { 1 /* 0 deg: 01 */, 0 /* +90: 00 */, 2 /* +180: 10 */, 3 /* +270: 11 */ };
    int64_t ar = 0, ai = 0;
    int32_t zr, zi, c, s, pr, pi_, er, ei, fr, fi;
    int x, y, k, q, d, px, py, t, adapt, wild;

    /* ---- equaliser ---- */
    for (k = 0; k < V22_EQN; k++) {
        ar += (int64_t)r->wr[k] * r->xr[k] - (int64_t)r->wi[k] * r->xi[k];
        ai += (int64_t)r->wr[k] * r->xi[k] + (int64_t)r->wi[k] * r->xr[k];
    }
    zr = (int32_t)(ar >> 24); zi = (int32_t)(ai >> 24);

    /* ---- carrier loop derotation: z * exp(-j theta) ---- */
    s = sintab[(r->theta >> 22) & SIN_MASK];
    c = sintab[((r->theta >> 22) + SIN_N / 4) & SIN_MASK];
    /* In 32 bits, as always: z is within 16 bits once the gain has settled,
     * but before that (and with a wild input) the products overflow. They
     * wrap, as they always did on every target, now with defined behaviour
     * (unsigned arithmetic; the conversion back is two's complement on
     * every target this is built for). */
    pr = (int32_t)((uint32_t)zr * (uint32_t)c + (uint32_t)zi * (uint32_t)s) >> 15;
    pi_ = (int32_t)((uint32_t)zi * (uint32_t)c - (uint32_t)zr * (uint32_t)s) >> 15;
    r->zr = pr; r->zi = pi_;

    slice(r, pr, pi_, &x, &y);
    er = pr - x * U; ei = pi_ - y * U;
    /* a point far outside the constellation (gain not settled yet) must not
     * overflow the squares below */
    if (er > 32767) er = 32767; else if (er < -32767) er = -32767;
    if (ei > 32767) ei = 32767; else if (ei < -32767) ei = -32767;
    r->mse += (((er * er + ei * ei) >> 8) - r->mse) >> 5;

    /* Burst gate. Once converged, a symbol whose error is far above what
     * the line has been giving is a noise burst, not information about the
     * line: nothing adapts on it (the carrier keeps turning at its learnt
     * rate). Without this, one 12 ms burst in a hundred left equaliser and
     * carrier loop in a state decision-directed adaptation never leaves. If
     * the "burst" lasts a second it is the line that changed: adapt again. */
    {
        int32_t e2 = (er * er + ei * ei) >> 8;
        wild = r->ndata > 600 && e2 > 8 * r->mse_slow + 256 && r->gated < 600;
        if (wild) r->gated++;
        else { r->gated = 0; r->mse_slow += (e2 - r->mse_slow) >> 9; }
        /* every symbol counts here, gated or not: this is what "loss of
         * equalisation" (6.4) is judged on. One symbol counts for at most
         * two units squared, so that a noise burst, however strong, weighs
         * no more than a lost constellation does. */
        r->mse_long += ((e2 > 8192 ? 8192 : e2) - r->mse_long) >> 9;
    }

    /* ---- carrier loop update ---- */
    if (!wild) {
        int32_t im = pi_ * x - pr * y;                 /* Im(z conj(d)) / U */
        int32_t perr = im * 4 / (x * x + y * y);       /* radians, Q12 */
        r->freq += perr * 326;
        if (r->freq > FREQ_MAX) r->freq = FREQ_MAX; else if (r->freq < -FREQ_MAX) r->freq = -FREQ_MAX;
        r->theta += (uint32_t)((int64_t)perr * 20861 + r->freq);
    } else {
        r->theta += (uint32_t)r->freq;
    }

    /* ---- equaliser update on scrambled data only ---- */
    /* the timing loop gets the first 80 ms of data to itself: a T/2 equaliser
     * adapting at the same time can hold the timing at its unstable point */
    adapt = (r->sig == V22_SIG_OTHER && r->ndata > 48 && !wild);
    if (adapt) {
        /* error rotated back in front of the carrier loop */
        fr = (er * c - ei * s) >> 15;
        fi = (ei * c + er * s) >> 15;
        for (k = 0; k < V22_EQN; k++) {
            /* 64-bit products, as a click on a high gain overflows 32 bits; the
             * taps saturate */
            r->wr[k] = sat32((int64_t)r->wr[k] - (((int64_t)fr * r->xr[k] + (int64_t)fi * r->xi[k] + 16) >> 5));   /* rounded: truncation is a steady drift */
            r->wi[k] = sat32((int64_t)r->wi[k] - (((int64_t)fi * r->xr[k] - (int64_t)fr * r->xi[k] + 16) >> 5));
        }
    }

    /* ---- may this be S1 starting? (see V22_RX_HOLD) ---- */
    if (r->mode16 && !r->clamp) {
        if (x == r->dx[1] && y == r->dy[1]) {
            r->brk = 0;
            if (++r->rep >= 4) r->susp = 1;
        }
        else if (!r->susp) r->rep = 0;
        else if (++r->brk >= 3) { r->susp = 0; r->rep = 0; }    /* one wrong decision does not end it */
    }
    r->dx[1] = r->dx[0]; r->dy[1] = r->dy[0]; r->dx[0] = x; r->dy[0] = y;

    /* ---- decode ---- */
    q = (x > 0) ? (y > 0 ? 0 : 3) : (y > 0 ? 1 : 2);
    d = dib[(q - r->quadrant) & 3];
    r->quadrant = q;
    rx_bit(r, d >> 1); rx_bit(r, d & 1);
    if (r->mode16) {
        px = x; py = y;
        for (k = 0; k < q; k++) { t = px; px = py; py = -t; }    /* back to quadrant 1 */
        rx_bit(r, py == 3); rx_bit(r, px == 3);
    }
    r->nsym++;
    if (r->sig == V22_SIG_OTHER) r->ndata++;
}

/* one T/2-spaced baseband sample (raw, before AGC) */
static void half_symbol(v22rx_t *r, int32_t yr, int32_t yi)
{
    int32_t gr, gi, o4r, o4i;
    int64_t p;
    v22_sig cls;

    /* ---- carrier detect and coarse AGC on raw power ---- */
    p = (int64_t)yr * yr + (int64_t)yi * yi;
    r->pw_raw += (p - r->pw_raw) >> 4;
    if (!r->carrier) {
        if (r->pw_raw > CD_ON) { r->carrier = 1; reset_tracking(r); }
    } else if (r->pw_raw < CD_OFF) {
        r->carrier = 0; r->sig = V22_SIG_NONE; r->sig_count = 0; r->gain = 0;
        if (!r->clamp) pend_deliver(r, 0);      /* the last bits before the line went quiet */
        r->susp = 0; r->rep = 0;
    }

    /* ---- two-symbol-lag correlation (4 half symbols) ---- */
    o4r = r->hr[r->hpos]; o4i = r->hi[r->hpos];
    r->hr[r->hpos] = yr; r->hi[r->hpos] = yi;
    r->hpos = (r->hpos + 1) & 3;
    r->c2r += (((int64_t)yr * o4r + (int64_t)yi * o4i) - r->c2r) >> 4;
    r->e2  += (((p + (int64_t)o4r * o4r + (int64_t)o4i * o4i) >> 1) - r->e2) >> 4;
    if (!r->carrier) cls = V22_SIG_NONE;
    else if (r->c2r * 5 > r->e2 * 3) cls = V22_SIG_S1;
    else if (r->c2r * 5 < -r->e2 * 3) cls = V22_SIG_USB1;
    else cls = V22_SIG_OTHER;
    if (r->carrier && r->c2r * 8 > r->e2 * 7) { if (r->s1_strong < 1000) r->s1_strong++; }
    else r->s1_strong = 0;
    if (r->sig_age < 1000000) r->sig_age++;
    if (cls == r->sig_cand) {
        if (++r->sig_count >= 8 && r->sig != cls) {
            /* (a "handshake signal" that lasted under 25 ms was random data
             * correlating by chance, see v22rx_s1_certain: nothing started,
             * so nothing is to be started afresh) */
            if (cls == V22_SIG_OTHER && (r->sig == V22_SIG_NONE || r->sig_age >= 30)) {
                /* scrambled data starts: the timing and equaliser loops only
                 * have something to work on from here, so start them fresh */
                int k;
                for (k = 0; k < V22_EQN; k++) r->wr[k] = r->wi[k] = 0;
                r->wr[V22_EQN / 2] = 1 << 24;
                if (r->sig != V22_SIG_S1) r->t_integ = 0;   /* S1 already set the timing */
                r->ndata = 0;
            }
            if (cls == V22_SIG_S1) q_reset(r);
            r->sig = cls; r->sig_age = 0;
        }
    }
    else { r->sig_cand = cls; r->sig_count = 0; }

    /* Line quality on S1, in blocks of 16 half symbols. A block is only
     * counted once the next one is complete: by the time the classifier
     * notices the end of S1, the newest samples are no longer S1. The
     * magnitude of the summed lag product is the repeating (signal) power
     * whatever the carrier offset; the rest of the power is noise. */
    if (r->sig == V22_SIG_S1) {
        r->q_cur[0] += (int64_t)yr * o4r + (int64_t)yi * o4i;
        r->q_cur[1] += (int64_t)yi * o4r - (int64_t)yr * o4i;
        r->q_cur[2] += (p + (int64_t)o4r * o4r + (int64_t)o4i * o4i) >> 1;
        if (++r->q_n == 16) {
            int i;
            if (r->q_have_prev) { for (i = 0; i < 3; i++) r->q_tot[i] += r->q_prev[i]; r->q_chunks++; }
            for (i = 0; i < 3; i++) { r->q_prev[i] = r->q_cur[i]; r->q_cur[i] = 0; }
            r->q_have_prev = 1; r->q_n = 0;
        }
    }

    if (!r->carrier) { r->half ^= 1; return; }

    /* first 16 half symbols of a new carrier: set the gain from measured power */
    if (r->agc_count < 16) {
        r->agc_acc += p;
        if (++r->agc_count == 16) {
            uint64_t avg = (uint64_t)(r->agc_acc >> 4);
            if (avg < 1) avg = 1;
            r->gain = (int32_t)isqrt_u64((((uint64_t)TARGET_PW) << 24) / avg);
        }
        r->half ^= 1;
        return;
    }

    gr = (int32_t)(((int64_t)yr * r->gain) >> 12);
    gi = (int32_t)(((int64_t)yi * r->gain) >> 12);

    /* equaliser delay line takes every half symbol */
    {
        int k;
        for (k = V22_EQN - 1; k > 0; k--) { r->xr[k] = r->xr[k - 1]; r->xi[k] = r->xi[k - 1]; }
        r->xr[0] = gr; r->xi[0] = gi;
    }

    if (!r->half) {                 /* mid-symbol sample */
        r->mid_r = gr; r->mid_i = gi;
        r->tnext += HALF_STEP;
        r->half = 1;
        return;
    }

    /* ---- on-time sample: timing detector (Gardner), fine AGC, then decide ---- */
    {
        int64_t e = (int64_t)(r->on_r - gr) * r->mid_r + (int64_t)(r->on_i - gi) * r->mid_i;
        int32_t en = (int32_t)(e / (TARGET_PW >> 10));         /* about +/-1024 full scale */
        int64_t pon = (int64_t)gr * gr + (int64_t)gi * gi;
        if (en > 2048) en = 2048; else if (en < -2048) en = -2048;
        /* the detector needs symbol transitions: a steady tone (unscrambled
         * ones) carries no timing information, so hold the loop there. S1
         * alternates every symbol and is the best signal to acquire on. */
        if (r->sig != V22_SIG_OTHER && r->sig != V22_SIG_S1) en = 0;
        /* the integrator only has to hold the clock-rate difference (within
         * +/- 0.06 %); it is kept out of the phase pull-in, where it would wind
         * up and then take hundreds of symbols to unwind */
        if (r->sig == V22_SIG_OTHER && r->ndata > 150) {
            r->t_integ += en * V22_TIMING_SIGN;
            if (r->t_integ > 48 * 64) r->t_integ = 48 * 64;
            else if (r->t_integ < -48 * 64) r->t_integ = -48 * 64;
        }
        /* proportional gain: fast for the first 0.5 s of data, then /6
         * (the detector's self-noise otherwise sets the error floor) */
        r->tnext += HALF_STEP + (r->t_integ >> 6)
                  + (r->ndata > 300 ? V22_TIMING_SIGN * en / 2 : V22_TIMING_SIGN * en * 3);
        r->on_r = gr; r->on_i = gi;

        r->pw_on += (pon - r->pw_on) >> 5;
        /* A level step of 9 dB or more is a different signal, not something
         * to track: start again as for a new carrier. The usual case is noise
         * above the carrier-detect threshold before the caller is heard; the
         * gain set on the noise is 20 dB too high for the signal, and the
         * fine loop below, asked to follow, ran the gain down to nothing and
         * left it there (start-up then failed between 14 and 22 dB SNR). */
        if (r->pw_on > 8 * (int64_t)TARGET_PW || r->pw_on < TARGET_PW / 8) {
            reset_tracking(r);
            r->half = 0;
            return;
        }
        r->gain += (int32_t)(((int64_t)r->gain * (TARGET_PW - r->pw_on) / TARGET_PW) >> 7);
        if (r->gain < 16) r->gain = 16;
    }
    r->half = 0;
    symbol(r);
}

void v22rx_tick(v22rx_t *r, int16_t sample)
{
    r->ring[r->rpos] = sample;
    r->rpos = (r->rpos + 1) & 127;
    r->cph += r->dph;
    r->tnext -= SAMPLE_STEP;
    if (r->tnext <= 0) {
        int p = (int)((-r->tnext) >> 8), i, idx = r->rpos - 1;
        const int16_t (*g)[2];
        int64_t ar = 0, ai = 0;
        int32_t br, bi, c, s, yr, yi;
        uint32_t ph;
        if (p >= V22_RXP) p = V22_RXP - 1;
        g = r->bank[p];
        for (i = 0; i < V22_RXN; i++) {
            int32_t x = r->ring[(idx - i) & 127];
            ar += x * g[i][0];
            ai += x * g[i][1];
        }
        br = (int32_t)(ar >> 15); bi = (int32_t)(ai >> 15);
        /* derotate by the carrier phase at the output instant (p/24 sample back) */
        ph = r->cph - (uint32_t)(((uint64_t)r->dph * (uint32_t)p) / V22_RXP);
        s = sintab[(ph >> 22) & SIN_MASK];
        c = sintab[((ph >> 22) + SIN_N / 4) & SIN_MASK];
        yr = (br * c + bi * s) >> 15;
        yi = (bi * c - br * s) >> 15;
        half_symbol(r, yr, yi);
        /* half_symbol() schedules the next instant only when tracking; keep the
         * clock running otherwise */
        if (r->tnext <= 0) r->tnext += HALF_STEP;
    }
}
