/* Test peer: V.90 Phase 2, analogue modem side (V.90 9.2.2). Built from the
 * same tone / INFO / probing blocks as the digital side. */
#ifndef V90D_SIM_P2A_H
#define V90D_SIM_P2A_H
#include <stdint.h>
#include "../modem/dpsk.h"
#include "../modem/probe.h"
#include "../modem/info.h"

typedef struct {
    int level;          /* nominal RMS */
    int v90;            /* INFO1a asks for V.90 (else V.34) */
    int up_sr;          /* upstream symbol rate index requested, -1 = best offered */
    int uinfo;
    info0_t info0a;
    /* V.34 in INFO1a (Table 11) only: what it asks of the digital modem's
     * transmitter. The symbol rate is the same in both directions. */
    int v34_hi_carrier; /* bit 25 */
    int v34_preemph;    /* bits 26:29 */
    /* V.92: INFO0a bit 26 is info0a.v92. pcm_up: select PCM upstream with
     * the INFO1a of Table 18/V.92 when INFO0d and INFO1d allow it (else what
     * v90 says); pcm_force: send that INFO1a whatever they say */
    int pcm_up, pcm_force;
    int sections, ltot_code, lmax_code, md_len;     /* Table 18 bits 12:24 */
    /* misbehaving callers, for the Phase 2 bounds of the digital modem:
     * info0_repeat: once INFO0 is exchanged, send INFO0a (acknowledged) this
     * many times more before the Tone A reversal; -1 = for ever. no_rev2:
     * never send the second Tone A reversal (the answer to the digital
     * modem's Tone B reversal); keep Tone A and start again from INFO0 2500 ms
     * later. 0 in both is the well-behaved caller. */
    int info0_repeat, no_rev2;
} p2a_cfg;

typedef enum {
    P2A_INFO0, P2A_WAIT_BREV1, P2A_REPLY, P2A_L1, P2A_L2, P2A_TONE50, P2A_REV_TAIL,
    P2A_WAIT_BREV2, P2A_PROBE_RX, P2A_WAIT_INFO1D, P2A_INFO1A, P2A_DONE, P2A_FAILED
} p2a_state;

typedef struct {
    p2a_cfg   cfg;
    p2a_state state;
    int32_t   now, t;
    dpsktx_t  tx; dpskrx_t rx;
    probetx_t ptx; proberx_t prx;
    int       probing;
    info0_t   info0d; int got0d, resend, n_info0a;
    info1d_t  info1d;
    info1a_t  info1a;
    int32_t   act_at, a_rev_line, probe_from, tone_since;
    int32_t   rtd;
    int       tone_blocks;
    int32_t   done_at;
    int       rep_left;     /* cfg.info0_repeat still to send */
    int32_t   restart_at;   /* cfg.no_rev2: INFO0 again at this sample; 0 = none */
} p2a_t;

void    p2a_init(p2a_t *p, const p2a_cfg *cfg);
/* Phase 2 of a retrain (9.5.2), after the 70 ms of silence: Tone A at once,
 * no INFO0 exchange, then as 9.2.2.1.3 */
void    p2a_init_retrain(p2a_t *p, const p2a_cfg *cfg);
int16_t p2a_tick(p2a_t *p, int16_t rx);

#endif
