/* V.32bis internals shared between the sources. */
#ifndef V90D_V32_PRIV_H
#define V90D_V32_PRIV_H
#include "v32.h"

extern const int8_t  v32_c4800[4][2], v32_c7200[16][2], v32_c9600[32][2],
                     v32_c12000[64][2], v32_c14400[128][2];
extern const int16_t v32_txh[10][V32_TXL][2], v32_ech[10][V32_TXL][2];
extern const int32_t v32_tx_rms_q12;
extern const int16_t v32_rxg[V32_RXP][V32_RXN][2];

const int8_t (*v32_constel(int rate))[2];

/* Y1Y2 label of a synchronising state (Figure 2-5) and back */
extern const uint8_t v32_abcd_label[4], v32_label_abcd[4];

/* ---- shared by the answering (v32.c) and calling (v32_call.c) sequencers ---- */
#define MS(ms) ((uint32_t)(ms) * 8)
/* samples per symbol = 10/3 */
#define SYM2SAMP(n) ((int32_t)(n) * 10 / 3)

#define TRN1_MIN   1280          /* 5.2.3: at least 1280 symbols */
#define TRN1_MAX   8192          /* 5.2.3: at most 8192 symbols */
#define C_TRN_MIN  2560          /* the calling modem's TRN, at least (v32_call.c) */
#define T_REVERSAL MS(4000)      /* CA / second AC unanswered */
#define T_TRAIN    MS(10000)     /* no S / R2 / E */
#define RT_BAD_MS  2000
#define RT_MAX     3
#define CD_LOSS_MS 400
/* three times |y|^2 of the receive filter output is compared with the
 * tone detectors' -43 dBm0 threshold of v32_rx.c (2512^2): 4.8 dB below it */
#define CD_OFF     ((int64_t)2512 * 2512)

/* mean-square error (as v32rx_t.mse) up to which a rate is taken, and where
 * a rate in use is given up */
extern const int32_t v32_rate_mse[V32_NRATES], v32_retrain_mse[V32_NRATES];
/* the highest rate of `common` the receiver's error allows, else the lowest */
int     v32_pick_rate(const v32_t *m, unsigned common);
/* 10 log10(a / b) in tenths of a dB */
int32_t v32_db10(int64_t a, int64_t b);

/* call mode (v32_call.c): its start, and what v32_tick runs for it */
void v32c_start(v32_t *m);
void v32c_tx_control(v32_t *m);
void v32c_rx_control(v32_t *m);

#endif
