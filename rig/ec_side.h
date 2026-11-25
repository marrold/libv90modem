/* One end of a V.42 link on a simulated call's bit pump, for tools/rec_x2
 * and rig/bench_v90 -e (`make cost-x2-v42`): the modem's getbit and putbit
 * go to a v42ec_t, which starts when the modem reports data mode, gets a
 * tick every millisecond (8 samples), and is kept full of an English
 * sentence; what it receives is counted and hashed. The recorder and the
 * bench call the same code at the same samples, so the answerer's end does
 * the same in both and the bench's transmit hash equals the recorder's.
 *
 * comp: EC_NONE (V.42, no compression), EC_V42BIS (2048 codewords, strings
 * of 32), EC_V44 (2048 / 255 / 4096): v42ec_cfg_default's, with what the
 * mode does not use turned off on both ends. */
#ifndef V90D_RIG_EC_SIDE_H
#define V90D_RIG_EC_SIDE_H
#include <string.h>
#include "v42.h"

enum { EC_OFF = -1, EC_NONE, EC_V42BIS, EC_V44 };

typedef struct {
    v42ec_t ec;
    int originator, comp, on;
    size_t wpos;                /* octets of the endless sentence written */
    unsigned long got;          /* octets received */
    uint32_t h;                 /* FNV-1a of them */
} ec_side;

static const char ec_sentence[] = "the quick brown fox jumps over the lazy dog. ";

/* "none", "v42bis" or "v44"; EC_OFF for anything else */
static int ec_mode(const char *s)
{
    return !strcmp(s, "none") ? EC_NONE : !strcmp(s, "v42bis") ? EC_V42BIS : !strcmp(s, "v44") ? EC_V44 : EC_OFF;
}

static void ec_side_init(ec_side *s, int originator, int comp)
{
    memset(s, 0, sizeof *s);
    s->originator = originator; s->comp = comp; s->h = 2166136261u;
}

static int ec_side_get(ec_side *s) { return s->on ? v42ec_tx_bit(&s->ec) : 1; }
static void ec_side_put(ec_side *s, int b) { if (s->on) v42ec_rx_bit(&s->ec, b & 1); }

/* every sample, before the modem's tick: start once the modem is in data
 * mode; then each millisecond a tick, as much of the sentence as the end
 * takes, and a read */
static void ec_side_sample(ec_side *s, long n, int data_mode)
{
    size_t k = sizeof ec_sentence - 1, w;
    uint8_t buf[256];
    if (!s->on) {
        v42ec_cfg_t c;
        if (!data_mode) return;
        v42ec_cfg_default(&c, s->originator);
        if (s->comp == EC_NONE) c.lapm.comp_tx = c.lapm.comp_rx = 0;
        if (s->comp != EC_V44) c.lapm.v44_tx = c.lapm.v44_rx = 0;
        v42ec_init(&s->ec, &c);
        s->on = 1;
    }
    if (n % 8) return;
    v42ec_tick(&s->ec, 1);
    for (;;) {
        size_t off = s->wpos % k;
        w = v42ec_write(&s->ec, (const uint8_t *)ec_sentence + off, k - off);
        s->wpos += w;
        if (w < k - off) break;
    }
    while ((w = v42ec_read(&s->ec, buf, sizeof buf)) > 0) {
        size_t i;
        for (i = 0; i < w; i++) s->h = (s->h ^ buf[i]) * 16777619u;
        s->got += w;
    }
}
#endif
