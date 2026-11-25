/* Test peer: V.34 Phase 2, CALL modem side (V.34 11.2.1.1), built from the
 * modem's own tone / INFO / probing blocks. The sequencing is that of the
 * V.90 digital modem (modem/p2d), which plays this part in V.90; the INFO
 * sequences are V.34's: INFO0c (Table 14) and INFO1c (Table 15).
 * The test decides what INFO1c says. */
#ifndef V90D_SIM_P2C_H
#define V90D_SIM_P2C_H
#include <stdint.h>
#include "../modem/dpsk.h"
#include "../modem/probe.h"
#include "../modem/info.h"

typedef struct {
    int level;              /* nominal RMS */
    info0_t info0c;         /* capabilities sent (ack is filled in) */
    /* INFO1c: what this modem's receiver asks of the answer modem's transmitter */
    int min_pwr_red, add_pwr_red, md_len;
    int hi_carrier_mask;    /* bit k: high carrier at symbol rate index k */
    int preemph[6];         /* pre-emphasis filter index per symbol rate */
    int max_rate[6];        /* projected rate per symbol rate in 2400 bit/s units; -1 = from
                               the probing, 0 = cannot be used */
    int k56;                /* INFO1c's frequency offset (bits 79:88) as a K56flex client sends
                               it (k56-facts.md "Phase 2"): P2C_K56_ACCEPT 0x001, mu-law and its
                               codec check passed; P2C_K56_DECLINE 0x200; 0 = the offset measured */
    int info0_repeat;       /* a misbehaving caller: once INFO0 is exchanged, send INFO0c
                               (acknowledged) this many times more before waiting for the
                               Tone A reversal; -1 = for ever, 0 = well behaved */
} p2c_cfg;

#define P2C_K56_ACCEPT  1
#define P2C_K56_DECLINE 2

typedef enum {
    P2C_INFO0, P2C_WAIT_REV1, P2C_REPLY1, P2C_WAIT_REV2, P2C_RECOVER, P2C_PROBE_RX,
    P2C_WAIT_REV3, P2C_REPLY3, P2C_L1, P2C_L2, P2C_INFO1C, P2C_WAIT_INFO1A,
    P2C_DONE, P2C_FAILED
} p2c_state;

typedef struct {
    p2c_cfg   cfg;
    p2c_state state;
    int32_t   now, t, level;
    dpsktx_t  tx; dpskrx_t rx;
    probetx_t ptx; proberx_t prx;
    int       probing;
    info0_t   info0a; int got0a, resend, n_info0c;
    info1d_t  info1c;
    info1a_t  info1a;
    int32_t   act_at, b_rev_line, rev2_at, probe_from;
    int32_t   rtd;
    int       tone_blocks;
    int       snr[PROBE_NBIN + 1];
    /* what the test looks at: sample times of events as this end saw them */
    int32_t   t_rev1, t_rev2, t_rev3, t_tone_a2, t_info1a;
    int       mute;         /* test hook: transmit nothing while set */
    int       rep_left;     /* cfg.info0_repeat still to send */
} p2c_t;

void    p2c_init(p2c_t *p, const p2c_cfg *cfg);
/* Phase 2 of a retrain (11.5.1): Tone B at once, no INFO0 exchange */
void    p2c_init_retrain(p2c_t *p, const p2c_cfg *cfg);
int16_t p2c_tick(p2c_t *p, int16_t rx);

#endif
