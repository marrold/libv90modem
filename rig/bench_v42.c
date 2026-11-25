/* V.42 on the target: two endpoints (v42/v42.h) back to back, both
 * directions kept full of text, for an instruction count under qemu (`make
 * cost-v42`). The run is 30000 line bits each way to set up the link (the
 * detection phase, XID, SABME), then NBITS more; the difference of two runs
 * is what NBITS line bits cost both endpoints, the harness's loop included.
 * Every 56 bits (1 ms at 56 kbit/s) both get a tick, as much text as they
 * take and a read. It prints both states, the octets received and a hash
 * of them (FNV-1a; integer only: the host's and the target's agree).
 *
 *   bench_v42 NBITS COMP      COMP: 0 none, 1 V.42 bis, 2 V.44 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "v42.h"

static v42ec_t A, B;
static uint8_t text[4096];

static size_t fill(v42ec_t *e, size_t tp)
{
    while (v42ec_write_space(e) > 0) {
        size_t w = v42ec_write(e, text + tp, sizeof text - tp);
        tp = (tp + w) % sizeof text;
    }
    return tp;
}

static unsigned long drain(v42ec_t *e, uint32_t *h)
{
    uint8_t buf[512];
    size_t n = v42ec_read(e, buf, sizeof buf), i;
    for (i = 0; i < n; i++) *h = (*h ^ buf[i]) * 16777619u;
    return (unsigned long)n;
}

int main(int argc, char **argv)
{
    v42ec_cfg_t ca, cb;
    long nb, b;
    int comp, i;
    size_t tp = 0;
    unsigned long got = 0;
    uint32_t s = 1, h = 2166136261u;

    if (argc != 3) { fprintf(stderr, "usage: bench_v42 NBITS COMP (0 none, 1 V.42 bis, 2 V.44)\n"); return 2; }
    nb = atol(argv[1]); comp = atoi(argv[2]);
    for (i = 0; i < (int)sizeof text; i++) {
        s = s * 1103515245u + 12345u;
        text[i] = (uint8_t)("etaoin shrdlu cmfwyp "[(s >> 16) % 21]);
    }
    v42ec_cfg_default(&ca, 1); v42ec_cfg_default(&cb, 0);
    if (comp == 0) ca.lapm.comp_tx = ca.lapm.comp_rx = ca.lapm.v44_tx = ca.lapm.v44_rx = 0;
    if (comp == 1) ca.lapm.v44_tx = ca.lapm.v44_rx = 0;
    v42ec_init(&A, &ca); v42ec_init(&B, &cb);
    for (b = 0; b < 30000 + nb; b++) {
        int ta = v42ec_tx_bit(&A), tb = v42ec_tx_bit(&B);
        v42ec_rx_bit(&B, ta); v42ec_rx_bit(&A, tb);
        if (b % 56 == 0) {
            v42ec_tick(&A, 1); v42ec_tick(&B, 1);
            tp = fill(&A, tp); tp = fill(&B, tp);
            got += drain(&B, &h); got += drain(&A, &h);
        }
    }
    printf("state %d %d got %lu hash %08" PRIx32 "\n", v42ec_state(&A), v42ec_state(&B), got, h);
    return 0;
}
