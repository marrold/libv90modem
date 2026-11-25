/* v90d/v34 - non-linear encoder, V.34 9.7, and its inverse.
 *
 *   x'(n) = Phi(n) x(n),  Phi = 1 + zeta / 6 + zeta^2 / 120,
 *   zeta = Theta |x(n)|^2 / (average energy of x),  Theta = 0.3125
 *
 * The "average energy of the signal x(n)" is taken as the nominal mean
 * energy of the data constellation (v34_params_t.energy_q8), a constant
 * both ends can compute; see README.md. */
#include "v34.h"
#include "../modem/divrcp.h"
#include "../modem/fixdiv.h"

/* Phi of zeta z (Q16), z at most 8 */
static int64_t phi_of_z(int64_t z)
{
    if (z > 8 * 65536) z = 8 * 65536;
    return 65536 + z / 6 + ((z * z) >> 16) / 120;
}

/* Phi in Q16 for the energy e (Q14 units^2) of a point, the mean being energy_q8 */
int64_t v34_nl_phi_q16(uint32_t energy_q8, int64_t e)
{
    /* zeta = (5 / 16) e / (energy_q8 * 2^6), in Q16; one division, by
     * divlu where the quotient fits 32 bits (every point's energy) */
    return phi_of_z(div_s64_u32(e * 320, energy_q8));
}

static int64_t phi_q16(const v34_params_t *p, int64_t e) { return v34_nl_phi_q16(p->energy_q8, e); }

static int16_t sat(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static int64_t rsh16(int64_t v) { return (v + 32768) >> 16; }

v34_pt_t v34_nl_encode(const v34_params_t *p, v34_pt_t x)
{
    v34_pt_t y = x;
    if (p->cfg.nonlinear) {
        int64_t phi = phi_q16(p, (int64_t)x.re * x.re + (int64_t)x.im * x.im);
        y.re = sat(rsh16(x.re * phi)); y.im = sat(rsh16(x.im * phi));
    }
    return y;
}

v34_pt_t v34_nl_decode(const v34_params_t *p, v34_pt_t xp)
{
    v34_pt_t y = xp;
    if (p->cfg.nonlinear) {
        /* |x'|^2 = |x|^2 Phi(|x|^2)^2: fixed-point iteration on |x|^2, which
         * closes the error by a factor of three or more each round for any
         * point a constellation holds (up to four times the mean energy) */
        int64_t ep = (int64_t)xp.re * xp.re + (int64_t)xp.im * xp.im, e, phi, nphi;
        /* the nine divisions by energy_q8 through its reciprocal, once a call */
        uint64_t rc = divrcp_rcp(p->energy_q8);
        uint32_t ar = (uint32_t)(xp.re < 0 ? -xp.re : xp.re) << 16, ai = (uint32_t)(xp.im < 0 ? -xp.im : xp.im) << 16;
        uint32_t ph, qr, qi;
        int64_t yr, yi;
        int k;
        /* eight rounds of e = |x'|^2 / Phi(e)^2 from e = |x'|^2, and Phi of
         * the last e. A round whose Phi is the last round's gives the same e
         * again, and so every round after it: the rest is skipped (a Phi
         * alternating between two values still runs all eight). */
        phi = phi_of_z(divrcp_sdiv(ep * 320, p->energy_q8, rc));
        for (k = 0; k < 8; k++) {
            /* (ep 2^28) / ((phi^2) >> 4): ep <= 2^31 and (phi^2) >> 4 >= 2^28,
             * so the high word ep >> 4 is below the divisor and the quotient
             * fits 32 bits */
            e = divlu((uint32_t)(ep >> 4), (uint32_t)ep << 28, (uint32_t)((phi * phi) >> 4));
            nphi = phi_of_z(divrcp_sdiv(e * 320, p->energy_q8, rc));
            if (nphi == phi) break;
            phi = nphi;
        }
        /* x 2^17 / phi, truncated, as 2 (a / phi) + (twice the remainder >=
         * phi), a = |x| 2^16 <= 2^31; then the sign: C's truncation of
         * x 2^17 / phi, in 32-bit divisions (phi < 2^18) */
        ph = (uint32_t)phi;
        qr = ar / ph; qi = ai / ph;
        yr = 2 * (int64_t)qr + (2 * (ar - qr * ph) >= ph);
        yi = 2 * (int64_t)qi + (2 * (ai - qi * ph) >= ph);
        if (xp.re < 0) yr = -yr;
        if (xp.im < 0) yi = -yi;
        y.re = sat((yr + (xp.re < 0 ? -1 : 1)) / 2);
        y.im = sat((yi + (xp.im < 0 ? -1 : 1)) / 2);
    }
    return y;
}
