/* modem/v90arx through the paths that change its taps or its lines other
 * than the LMS step of a symbol: a hold of one symbol and of many, the
 * timing loop held, the band edge switched on and off (a design at once,
 * whose taps replace the equaliser's), impulses (symbols the gate keeps out
 * of the adaptation), the clock 70 ppm off (a design every 512 symbols),
 * TRN1d's interval levels (ls_set) and the first design. Then a second
 * receiver taking fewer of TRN1d's symbols as known (ntrain 1800), held
 * from its first equalised symbol until the estimate's symbol is back
 * where EST left it, the line inverted just before: wrong decisions written
 * over symbols in the estimate's span while it made no step. The decisions,
 * their outputs and errors, and the taps at the end are hashed: the
 * receiver's arithmetic is to stay exactly what it is whatever is done to
 * make it cheaper (an LMS step that also forms the next symbol's sum must
 * not use one formed before such a change). */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "test.h"
#include "v90arx.h"
#include "v90_tx.h"
#include "v90_pcm.h"
#include "dschan.h"
#include "noise.h"

typedef struct {
    v90tx_t tx; rng_t rng;
    int32_t n, n_trn; int sel;
} src_t;

static int getbit(void *ctx) { src_t *s = (src_t *)ctx; return rng_uniform(&s->rng) < 0.5; }

static uint8_t next(void *ctx)
{
    src_t *s = (src_t *)ctx;
    int32_t n = s->n++;
    uint8_t o;
    if (n == 400) v90tx_select(&s->tx, V90TX_SD);
    o = v90tx_tick(&s->tx);
    if (v90tx_signal(&s->tx) == V90TX_TRN1D) {
        if (s->n_trn < 0) s->n_trn = n;
        if (!s->sel && (int32_t)v90tx_symbols(&s->tx) >= 2400 - 3) { v90tx_select(&s->tx, V90TX_B1D); s->sel = 1; }
    }
    return o;
}

static src_t S;
static dschan_t ch;
static v90arx_t rx;

static uint32_t fnv(uint32_t h, int32_t v)
{
    int i;
    uint32_t u = (uint32_t)v;
    for (i = 0; i < 4; i++) { h ^= (u >> (8 * i)) & 255u; h *= 16777619u; }
    return h;
}

static v90arx_cons_t cons;

/* the line and a receiver taking ntrain of TRN1d's symbols as known */
static void start(int32_t ntrain)
{
    dschan_cfg c;
    v90arx_cfg_t rc;
    uint8_t mask[16];
    int i;
    memset(&S, 0, sizeof S);
    rng_seed(&S.rng, 7);
    S.n_trn = -1;
    v90tx_init(&S.tx, 0, 90);
    v90tx_set_getbit(&S.tx, getbit, &S);
    memset(mask, 0, sizeof mask);
    for (i = 6; i < 128; i += 20) mask[i >> 3] |= (uint8_t)(1 << (i & 7));
    {
        v90_map_t map;
        memset(&map, 0, sizeof map);
        map.K = 15;
        for (i = 0; i < 6; i++) v90_map_set_points(&map, i, mask);
        (void)v90tx_set_map(&S.tx, V90TX_MAP_DATA, &map);
    }
    for (i = 0; i < 6; i++) v90arx_cons_mask(&cons, 0, i, mask);
    memset(&c, 0, sizeof c);
    c.loop_km = 2; c.level_db = -12; c.noise_dbm0 = -75; c.echo_db = -300; c.phase = 0.3; c.seed = 4;
    c.ppm = 70;
    dschan_init(&ch, &c, next, &S);
    memset(&rc, 0, sizeof rc);
    rc.uinfo = 90; rc.ntrain = ntrain;
    v90arx_init(&rx, &rc);
}

static void print_taps(void)
{
    uint32_t hf = 2166136261u, hb = 2166136261u, hh = 2166136261u;
    int i;
    for (i = 0; i < V90ARX_NF; i++) hf = fnv(hf, rx.f[i]);
    for (i = 0; i < V90ARX_NB; i++) hb = fnv(hb, rx.b[i]);
    for (i = 0; i < V90ARX_NCH; i++) hh = fnv(hh, rx.h[i]);
    printf("  taps at the end: forward %08" PRIx32 ", feedback %08" PRIx32 ", estimate %08" PRIx32 "\n", hf, hb, hh);
}

/* The second receiver. EST's last estimator step was on symbol k + LEAD
 * (k its last sample); TRAIN's first is DFF + cursor symbols before that
 * and climbs one a symbol. Held from TRAIN's first sample until the next
 * decision is symbol M = k + LEAD + 1 (the estimate makes no step
 * meanwhile); the line inverted over EST's last 20 steps and the design's,
 * the samples TRAIN's first symbols are decided on, so that the decisions
 * from ntrain on, written over the reference in the estimate's span, are
 * the reference's opposite; then 1.5 s on. */
static void train_held(void)
{
    uint32_t hd = 2166136261u;
    int32_t t, M = -1, lastm = -1, t_train = -1, ndec = 0;
    int i, st = 0, set = 0;
    printf("ntrain 1800, held from TRAIN's start to EST's last symbol, line inverted\n");
    start(1800);
    for (t = 0; t < 8000 * 4; t++) {
        int16_t x = dschan_tick(&ch, 0);
        int nd;
        if (!set && S.n_trn >= 0) { v90arx_set_cons(&rx, &cons, 2400); set = 1; }
        if (st == 0 && rx.state == V90ARX_EST && rx.nest >= V90ARX_EST0 - 20) x = (int16_t)(x == -32768 ? 32767 : -x);
        if (st == 1) {
            if (lastm + 1 == M) { v90arx_hold(&rx, 0); st = 2; printf("  released at symbol %" PRId32 "\n", M); }
        }
        nd = v90arx_tick(&rx, x, 0);
        if (nd) lastm = rx.out[nd - 1].m;
        if (st == 0 && rx.state == V90ARX_TRAIN) {
            /* (TRAIN began on this tick's last sample: no decision yet) */
            M = (int32_t)(rx.n - rx.n0) + V90ARX_LEAD;
            t_train = t;
            v90arx_hold(&rx, 1);
            st = 1;
            printf("  TRAIN at sample %" PRId32 " with %d decisions out, EST's last step on symbol %" PRId32 "\n", t, nd, M - 1);
        }
        if (st >= 1) {
            for (i = 0; i < nd; i++) {
                const v90arx_dec_t *o = &rx.out[i];
                hd = fnv(hd, o->m); hd = fnv(hd, o->ucode); hd = fnv(hd, o->sign);
                hd = fnv(hd, o->y); hd = fnv(hd, o->e);
                ndec++;
            }
        }
        if (st == 2 && t >= t_train + 8000 * 3 / 2) break;
    }
    printf("  state %d, %5" PRId32 " decisions from TRAIN on, mse %6" PRIu64 ", hash %08" PRIx32 "\n", rx.state, ndec, rx.mse >> 8, hd);
    print_taps();
    CHECK(st == 2);
    CHECK(rx.state == V90ARX_DD);
}

/* one stage: its ticks, from tick t0 to t1 */
typedef struct { const char *name; int32_t t0, t1; } stage_t;

int main(void)
{
    /* data mode starts some 2.3 s in (Sd, TRN1d); the stages after it */
    static const stage_t ST[] = {
        { "train, first design, ls_set", 0, 8000 * 3 },
        { "holds of 1 symbol (x8), of 200",  8000 * 3, 8000 * 7 / 2 },
        { "band edge on (design at once)", 8000 * 7 / 2, 8000 * 4 },
        { "timing held, impulses (gate)", 8000 * 4, 8000 * 9 / 2 },
        { "band edge off, holds, impulses", 8000 * 9 / 2, 8000 * 6 },
    };
    int32_t t;
    int i, s, set = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("v90arx: the paths that change the taps or the lines\n");
    start(2000);
    t = 0;
    for (s = 0; s < (int)(sizeof ST / sizeof ST[0]); s++) {
        uint32_t hd = 2166136261u;
        int32_t ndec = 0, ndsn = 0, nhold = 0;
        for (; t < ST[s].t1; t++) {
            int16_t x = dschan_tick(&ch, 0);
            int nd;
            int32_t u = t - ST[s].t0;
            if (!set && S.n_trn >= 0) { v90arx_set_cons(&rx, &cons, 2400); set = 1; }
            switch (s) {
            case 1:     /* a hold of one symbol, every 400 ticks; then 200 symbols */
                if (u < 3200) v90arx_hold(&rx, u % 400 == 0);
                else v90arx_hold(&rx, u < 3400);
                break;
            case 2:
                if (u == 0) v90arx_edge(&rx, 1);
                break;
            case 3:     /* an impulse every 400 ticks */
                v90arx_timing_hold(&rx, u < 3000);
                if (u % 400 == 0) x = (int16_t)(x + (u % 800 == 0 ? 200 : -200));
                break;
            case 4:
                if (u == 0) v90arx_edge(&rx, 0);
                v90arx_hold(&rx, u % 1000 >= 990);
                if (u % 700 == 5) x = (int16_t)(x + 200);
                break;
            default:
                break;
            }
            nd = v90arx_tick(&rx, x, 0);
            if (rx.dsn_ran) ndsn++;
            if (rx.hold) nhold++;
            for (i = 0; i < nd; i++) {
                const v90arx_dec_t *o = &rx.out[i];
                hd = fnv(hd, o->m); hd = fnv(hd, o->ucode); hd = fnv(hd, o->sign);
                hd = fnv(hd, o->y); hd = fnv(hd, o->e);
                ndec++;
            }
        }
        printf("  %-34s state %d, %5" PRId32 " decisions, mse %6" PRIu64 ", design steps %3" PRId32 ", held %4" PRId32 ", hash %08" PRIx32 "\n",
               ST[s].name, rx.state, ndec, rx.mse >> 8, ndsn, nhold, hd);
    }
    print_taps();
    CHECK(rx.state == V90ARX_DD);
    train_held();
    T_REPORT("v90arx_paths");
}
