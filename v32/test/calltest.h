/* Shared by the end-to-end tests: the answering modem (v32/) and the calling
 * test peer (sim/v32call) on a simulated 2-wire line (sim/v32line), each
 * sending a known pseudo-random stream once its transmitter is up. */
#ifndef V32_CALLTEST_H
#define V32_CALLTEST_H
#include <string.h>
#include "../v32.h"
#include "../../sim/v32call.h"
#include "../../sim/v32line.h"

/* data source: 64 zeros, a one, then a pseudo-random stream; the checker
 * finds the one after the zeros and counts mismatches from there */
typedef struct { uint32_t l; int n; } src_t;
typedef struct { uint32_t l; int zeros, synced; long bits, errs; } chk_t;

static int src_bit(void *u)
{
    src_t *s = u;
    if (s->n < 65) return s->n++ == 64;
    s->l = s->l * 1103515245u + 12345u;
    return (s->l >> 16) & 1;
}

static void chk_bit(void *u, int b)
{
    chk_t *c = u;
    if (!c->synced) {
        if (!b) c->zeros++;
        else { if (c->zeros >= 48) c->synced = 1; c->zeros = 0; }
        return;
    }
    c->l = c->l * 1103515245u + 12345u;
    c->bits++;
    c->errs += b != (int)((c->l >> 16) & 1);
}

typedef struct {
    v32_t      m;
    v32call_t  c;
    v32line_t  line;
    src_t      a_src, c_src;       /* what the answerer / the caller sends */
    chk_t      a_chk, c_chk;       /* what the answerer / the caller receives */
    int16_t    a_rx;
    long       n;
    int        c_level_div;
    int        no_auto_retrain;    /* set before call_init */
    /* set before call_init: samples of driver buffering between the answerer
     * and the line (all put on its transmit side), told to it as echo_delay */
    int        io_delay;
    int16_t    io_buf[512]; int io_pos;
    int        io_tell, io_told;   /* io_tell: tell it io_told instead (a wrong figure) */
} call_t;

static int  a_get(void *u) { return src_bit(&((call_t *)u)->a_src); }
static void a_put(void *u, int b) { chk_bit(&((call_t *)u)->a_chk, b); }
static int  c_get(void *u) { return src_bit(&((call_t *)u)->c_src); }
static void c_put(void *u, int b) { chk_bit(&((call_t *)u)->c_chk, b); }
static double call_peer(void *u, double rx) { return v32call_tick(&((call_t *)u)->c, rx); }

static inline void call_reset_data(call_t *k)
{
    memset(&k->a_src, 0, sizeof k->a_src); memset(&k->c_src, 0, sizeof k->c_src);
    memset(&k->a_chk, 0, sizeof k->a_chk); memset(&k->c_chk, 0, sizeof k->c_chk);
    k->a_src.l = k->c_chk.l = 1; k->c_src.l = k->a_chk.l = 7;
}

/* levels: line RMS in 16-bit sample units */
static inline void call_init(call_t *k, const v32line_cfg *lc, unsigned a_rates, unsigned c_rates, int a_level, double c_level)
{
    v32_cfg ac; vc_cfg cc;
    call_reset_data(k);
    ac.level = a_level; ac.rates = a_rates; ac.getbit = a_get; ac.putbit = a_put; ac.user = k; ac.echo_delay = k->io_tell ? k->io_told : k->io_delay; ac.no_auto_retrain = k->no_auto_retrain;
    v32_init(&k->m, &ac);
    cc.level = c_level; cc.rates = c_rates; cc.trn_len = 1280; cc.getbit = c_get; cc.putbit = c_put; cc.user = k;
    v32call_init(&k->c, &cc);
    v32line_init(&k->line, lc);
    k->a_rx = 0; k->n = 0;
    memset(k->io_buf, 0, sizeof k->io_buf); k->io_pos = 0;
}

static inline void call_tick(call_t *k)
{
    int16_t tx = v32_tick(&k->m, k->a_rx);
    if (k->io_delay > 0) {
        int16_t out = k->io_buf[(k->io_pos - k->io_delay) & 511];
        k->io_buf[k->io_pos] = tx; k->io_pos = (k->io_pos + 1) & 511;
        tx = out;
    }
    k->a_rx = v32line_tick(&k->line, tx, call_peer, k);
    k->n++;
}

/* run until both ends are in data mode (returns 1) or `max` samples have gone */
static inline int call_connect(call_t *k, long max)
{
    while (max-- > 0) {
        call_tick(k);
        if (k->m.connected && !k->m.retraining && k->c.connected && k->c.state == VCS_DATA) return 1;
        if (k->m.state == V32S_FAILED || k->c.state == VCS_FAILED) return 0;
    }
    return 0;
}

static inline void call_run(call_t *k, long n) { while (n-- > 0) call_tick(k); }

static inline void line_defaults(v32line_cfg *lc)
{
    memset(lc, 0, sizeof *lc);
    lc->loss_db = 6; lc->delay = 40;
    lc->near_db = 300; lc->far_db = 300; lc->c_near_db = 300; lc->c_far_db = 300;
    lc->noise_a_dbm0 = -300; lc->noise_c_dbm0 = -300;
    lc->seed = 1;
}

#endif
