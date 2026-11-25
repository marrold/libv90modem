/* The calling side's sequences against what real modems sent when they
 * called v90d (test/data/realcheck.txt: extracts of the recordings, made
 * with tools/v90m-fields). Up to now the caller had been tried against the
 * library's own answerer only, where a misreading of the spec shared by both
 * sides would pass every test; three modems of two makers are an outside
 * reference. For each sequence:
 *
 *   - the parser (the answering side's) reads the fields this file names
 *     for that recording, read by hand off the decode and checked against
 *     the tables of V.8, V.34, V.90 and V.92;
 *   - the builder the caller sends it with (modem/info.c, v90/v90_seq.c,
 *     v92/v92_seq.c, modem/v34mp.c, modem/v8msg.c), given those fields,
 *     gives the same bits as the real modem, CRC included, except where a
 *     modem departs from the tables (named below);
 *   - where the caller chooses the content (CM, INFO0a), what it sends has
 *     what the real modems' have and the tables require.
 *
 * See docs/history/PROGRESS.md, "realcheck". */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../modem/info.h"
#include "../modem/v8msg.h"
#include "../modem/v34mp.h"
#include "../modem/orig.h"
#include "../modem/p2call.h"
#include "../v90/v90_seq.h"
#include "../v92/v92_seq.h"

#define MAXB 2720
typedef struct { char rec[16], modem[16], seq[8]; int n; uint8_t b[MAXB]; } fx_t;
static fx_t fx[40];
static int nfx;

static int load(const char *path)
{
    static char line[MAXB + 128];
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f) && nfx < 40) {
        fx_t *x = &fx[nfx];
        char bits[MAXB + 1];
        double t;
        int k;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%15s %15s %7s %lf %d %2720s", x->rec, x->modem, x->seq, &t, &x->n, bits) != 6) continue;
        if (x->n < 1 || x->n > MAXB || (int)strlen(bits) != x->n) continue;
        for (k = 0; k < x->n; k++) x->b[k] = (uint8_t)(bits[k] == '1');
        nfx++;
    }
    fclose(f);
    return nfx;
}

static const fx_t *get(const char *rec, const char *seq, int nth)
{
    int i;
    for (i = 0; i < nfx; i++)
        if (!strcmp(fx[i].rec, rec) && !strcmp(fx[i].seq, seq) && nth-- == 0) return &fx[i];
    printf("  FIXTURE MISSING: %s %s\n", rec, seq);
    return NULL;
}

/* bits of a and b (n each) that differ, as a mask of the first 64 positions
 * from `from`, and the count over all */
static int ndiff(const uint8_t *a, const uint8_t *b, int n)
{
    int k, d = 0;
    for (k = 0; k < n; k++) d += (a[k] & 1) != (b[k] & 1);
    return d;
}

/* ---- V.8 CM ---- */
static int octets(const fx_t *x, uint8_t *o)
{
    int k, b;
    for (k = 0; k < x->n / 8; k++) { o[k] = 0; for (b = 0; b < 8; b++) o[k] = (uint8_t)(o[k] | x->b[8 * k + b] << b); }
    return x->n / 8;
}
static int has_octet(const uint8_t *o, int n, uint8_t v) { int k; for (k = 0; k < n; k++) if (o[k] == v) return 1; return 0; }

static void cm_check(const char *rec, unsigned modes, int pcm, int prot, int access)
{
    const fx_t *x = get(rec, "CM", 0);
    uint8_t o[V8_MAX_OCTETS], r[V8_MAX_OCTETS];
    v8_menu_t m;
    int n, nr, k;
    if (!x) { CHECK(0); return; }
    n = octets(x, o);
    CHECK_EQ_I(v8_decode(o, n, &m), 0);
    CHECK_EQ_I(m.call_function, V8_CALLF_DATA);
    CHECK_EQ_I(m.modes, modes);
    CHECK_EQ_I(m.n_mod_octets, 3);
    CHECK_EQ_I(m.has_pcm, pcm >= 0); if (pcm >= 0) CHECK_EQ_I(m.pcm, pcm);
    CHECK_EQ_I(m.has_prot, prot >= 0); if (prot >= 0) CHECK_EQ_I(m.prot, prot);
    CHECK_EQ_I(m.has_access, access >= 0); if (access >= 0) CHECK_EQ_I(m.access, access);
    /* our encoder: the same octets (the order of the categories after the
     * modulation octets is free: V.8 clause 6 sets none, and the modems
     * differ in it) */
    nr = v8_encode(&m, r, V8_MAX_OCTETS);
    CHECK_EQ_I(nr, n);
    for (k = 0; k < n; k++) CHECK(has_octet(r, nr, o[k]));
    printf("  %-9s CM", rec);
    for (k = 0; k < n; k++) printf(" %02x", o[k]);
    printf(": modes 0x%04x, pcm %d, protocol %d, access %d; ours from it", m.modes, pcm, prot, access);
    for (k = 0; k < nr; k++) printf(" %02x", r[k]);
    printf("\n");
}

/* ---- INFO sequences: payload = information bits and CRC ---- */
static void info_rebuild(const fx_t *x, const uint8_t *frame, int nframe, int expect_diff)
{
    int d;
    CHECK_EQ_I(nframe - INFO_HDR_BITS - 4, x->n);
    d = ndiff(frame + INFO_HDR_BITS, x->b, x->n);
    if (expect_diff < 0) CHECK_EQ_I(d, 0); else CHECK(d > 0);
}

static void info0_check(const char *rec, const char *seq, int all_rates, int v92)
{
    const fx_t *x = get(rec, seq, 0);
    info0_t q; uint8_t fr[INFO_MAX_BITS + 8];
    if (!x) { CHECK(0); return; }
    CHECK_EQ_I(x->n, INFO_PAYLOAD(INFO0A_BITS));
    CHECK(info_check(x->b, x->n));
    info0a_parse(x->b, &q);
    CHECK_EQ_I(q.sr2743, all_rates); CHECK_EQ_I(q.sr2800, all_rates); CHECK_EQ_I(q.sr3429, all_rates);
    CHECK_EQ_I(q.lo3000, 1); CHECK_EQ_I(q.hi3000, 1); CHECK_EQ_I(q.lo3200, 1); CHECK_EQ_I(q.hi3200, 1);
    CHECK_EQ_I(q.allow3429, all_rates); CHECK_EQ_I(q.pwr_reduce, 1); CHECK_EQ_I(q.max_sr_diff, 0);
    CHECK_EQ_I(q.cme, 0); CHECK_EQ_I(q.c1664, 1);
    /* bit 26: V.92 (Table 16/V.92); bit 27, short Phase 2: never; in INFO0c
     * the two are the transmit clock source of Table 12/V.34, 0 = internal */
    CHECK_EQ_I(q.v92, v92); CHECK_EQ_I(q.short_p2, 0);
    CHECK_EQ_I(q.ack, 0);                       /* the first one of the call */
    info_rebuild(x, fr, info0a_build(&q, fr), -1);
    printf("  %-9s %s: rates all %d, V.92 %d; ours from it the same bits\n", rec, seq, all_rates, v92);
}

/* kind, bits 12:17, md, uinfo, up_sr, freq_off; dev: the modem sends bits
 * 40:49 of Table 18/V.92 other than ones (Conexant: its frequency offset) */
static void info1a_check(const char *rec, int nth, int kind, unsigned b12_17, int md, int uinfo, int up_sr, int freq_off, int dev)
{
    const fx_t *x = get(rec, "INFO1a", nth);
    info1a_t q; uint8_t fr[INFO_MAX_BITS + 8];
    int k, n, d;
    unsigned raw;
    if (!x) { CHECK(0); return; }
    CHECK_EQ_I(x->n, INFO_PAYLOAD(INFO1A_BITS));
    CHECK(info_check(x->b, x->n));
    info1a_parse(x->b, &q);
    CHECK_EQ_I(info1a_kind(&q), kind);
    raw = 0; for (k = 0; k < 6; k++) raw |= (unsigned)x->b[k] << k;
    CHECK_EQ_I(raw, b12_17);
    CHECK_EQ_I(q.md_len, md); CHECK_EQ_I(q.uinfo, uinfo); CHECK_EQ_I(q.up_sr, up_sr);
    CHECK_EQ_I(x->b[20], 0); CHECK_EQ_I(x->b[21], 0);           /* 32:33 reserved, 0 */
    raw = 0; for (k = 0; k < 3; k++) raw |= (unsigned)x->b[25 + k] << k;
    CHECK_EQ_I(raw, 6);                                         /* 37:39: PCM downstream */
    raw = 0; for (k = 0; k < 10; k++) raw |= (unsigned)x->b[28 + k] << k;
    if (kind != INFO1A_PCM) CHECK_EQ_I(q.freq_off, freq_off);
    n = info1a_build(&q, fr);
    CHECK_EQ_I(n, INFO1A_BITS);
    d = ndiff(fr + INFO_HDR_BITS, x->b, 28);                    /* bits 12:39 */
    CHECK_EQ_I(d, 0);
    if (kind == INFO1A_PCM) {
        /* Table 18: 40:49 "set to 1 to avoid generating a tone"; ours are */
        for (k = 0; k < 10; k++) CHECK_EQ_I(fr[INFO_HDR_BITS + 28 + k], 1);
        CHECK_EQ_I(raw == 0x3ff, !dev);
    }
    info_rebuild(x, fr, n, dev ? 1 : -1);
    printf("  %-9s INFO1a: kind %d, bits 12:17 0x%02x, md %d, UINFO %d, up_sr %d, bits 40:49 0x%03x; ours from it %s\n", rec, kind,
           b12_17, md, uinfo, up_sr, raw, dev ? "differs in 40:49 and the CRC only" : "the same bits");
}

/* ---- Phase 3 / 4 sequences, from the frame sync on ---- */
static void dil_fields(const v90_dil_t *d, int n, int lsp, int ltp, const int *h, int ref)
{
    int k;
    CHECK_EQ_I(d->n, n); CHECK_EQ_I(d->lsp, lsp); CHECK_EQ_I(d->ltp, ltp);
    for (k = 0; k < 8; k++) { CHECK_EQ_I(d->h[k], h[k]); CHECK_EQ_I(d->ref[k], ref); }
}
static const int H_CX[8] = { 22, 22, 22, 22, 22, 22, 14, 8 }, H_USR[8] = { 10, 10, 10, 10, 10, 10, 10, 10 };

static void ja_check(const char *rec, int n, int lsp, int ltp, const int *h, int ref, int train0)
{
    const fx_t *x = get(rec, "JA", 0);
    static uint8_t o[V90_DIL_MAX_BITS + 16];
    v90_dil_t d; int r;
    if (!x) { CHECK(0); return; }
    r = v90_dil_parse(x->b, x->n, &d);
    CHECK_EQ_I(r, x->n);
    dil_fields(&d, n, lsp, ltp, h, ref);
    CHECK_EQ_I(d.train[0], train0);
    CHECK_EQ_I(v90_dil_build(&d, o), x->n);
    CHECK_EQ_I(ndiff(o, x->b, x->n), 0);
    printf("  %-9s Ja (V.90, %d bits): DIL N %d, Lsp %d, Ltp %d, H1 %d, REF %d; ours from it the same bits\n", rec, x->n, n, lsp, ltp, h[0], ref);
}

/* fill: the modem's fill after the CRC is that many bits short of the table's */
static void ja92_check(const char *rec, int n, int lsp, int ltp, const int *h, int ref, int short_fill)
{
    const fx_t *x = get(rec, "JA92", 0);
    static uint8_t o[V92_JA_MAX_BITS + 16];
    v92_ja_t j; int r, m;
    if (!x) { CHECK(0); return; }
    r = v92_ja_parse(x->b, x->n, &j);
    CHECK_EQ_I(r, x->n);
    dil_fields(&j.dil, n, lsp, ltp, h, ref);
    CHECK_EQ_I(j.rates, 0x7ffff);                   /* 24000 to 48000: all of Table 20 */
    m = v92_ja_build(&j, o);
    CHECK_EQ_I(m % 12, 0);                          /* 8.5.4: a multiple of 12 bits */
    CHECK_EQ_I(m, x->n + short_fill);
    CHECK_EQ_I(ndiff(o, x->b, x->n), 0);
    printf("  %-9s Ja (V.92, %d bits): DIL N %d, rates 0x7ffff; ours from it the same bits%s\n", rec, x->n, n,
           short_fill ? ", then the zero fill to 12 bits the modem leaves out" : "");
}

static int npoints(const uint8_t *mask) { int k, c = 0; for (k = 0; k < 128; k++) c += (mask[k >> 3] >> (k & 7)) & 1; return c; }

static void cp_check(const char *rec, const char *seq, int is_cp, int drn, int ack, unsigned rates, int ld, int rms, const int *idx, int cd, int pts, int fill_short)
{
    const fx_t *x = get(rec, seq, 0);
    static uint8_t o[V90_CP_MAX_BITS + 16];
    v90_cp_t c; int r, k, m;
    if (!x) { CHECK(0); return; }
    r = v90_cp_parse(x->b, x->n, &c);
    CHECK_EQ_I(r, x->n);
    CHECK_EQ_I(c.is_cp, is_cp); CHECK_EQ_I(c.drn, drn); CHECK_EQ_I(c.silence, 0); CHECK_EQ_I(c.sr, 1);
    CHECK_EQ_I(c.ack, ack); CHECK_EQ_I(c.alaw, 0); CHECK_EQ_I(c.rates, rates); CHECK_EQ_I(c.ld, ld);
    CHECK_EQ_I(c.rms_ratio, rms);
    /* a1 = 1.0: T(z) = 1 - z^-1, the DC null of V.90 5.4.5.6 */
    CHECK_EQ_I(c.a1, 64); CHECK_EQ_I(c.a2, 0); CHECK_EQ_I(c.b1, 0); CHECK_EQ_I(c.b2, 0);
    for (k = 0; k < 6; k++) CHECK_EQ_I(c.idx[k], idx[k]);
    CHECK_EQ_I(c.codec_differs, cd);
    CHECK_EQ_I(npoints(c.mask[0]), pts);
    m = v90_cp_build(&c, o);
    CHECK_EQ_I(m, x->n + fill_short);
    CHECK_EQ_I(ndiff(o, x->b, x->n), 0);
    printf("  %-9s %-3s (%d bits): drn %d, Sr 1, ld %d, a1 1.0, %d points in constellation 0; ours from it the same bits%s\n", rec, seq,
           x->n, drn, ld, pts, fill_short ? " and two more fill bits (Table 14 has three)" : "");
}

static void cpu_check(const char *rec, const char *seq, int unit, int type, int drn, int ld, int rms, const int *idx, int cd, int pts, int fill_short)
{
    const fx_t *x = get(rec, seq, 0);
    static uint8_t o[V92_CPU_MAX_BITS + 16];
    v92_cpu_t c; int r, k, m;
    if (!x) { CHECK(0); return; }
    r = v92_cpu_parse(x->b, x->n, unit, &c);
    CHECK_EQ_I(r, x->n);
    CHECK_EQ_I(c.type, type); CHECK_EQ_I(c.drn, drn); CHECK_EQ_I(c.sr, 1); CHECK_EQ_I(c.ack, 0);
    CHECK_EQ_I(c.alaw, 0); CHECK_EQ_I(c.ld, ld); CHECK_EQ_I(c.rms_ratio, rms);
    CHECK_EQ_I(c.a1, 64); CHECK_EQ_I(c.a2, 0); CHECK_EQ_I(c.b1, 0); CHECK_EQ_I(c.b2, 0);
    for (k = 0; k < 6; k++) CHECK_EQ_I(c.idx[k], idx[k]);
    CHECK_EQ_I(c.codec_differs, cd);
    CHECK_EQ_I(npoints(c.mask[0]), pts);
    m = v92_cpu_build(&c, unit, o);
    CHECK_EQ_I(m % unit, 0);                        /* Table 23: fill to 12 symbols */
    CHECK_EQ_I(m, x->n + fill_short);
    CHECK_EQ_I(ndiff(o, x->b, x->n), 0);
    printf("  %-9s %-5s (%d bits): drn %d, ld %d, %d points in constellation 0; ours from it the same bits\n", rec, seq, x->n, drn, ld, pts);
}

static void suvu_check(const char *rec)
{
    const fx_t *x = get(rec, "SUVU", 0);
    uint8_t o[128]; v92_suvu_t s; int r;
    if (!x) { CHECK(0); return; }
    r = v92_suvu_parse(x->b, x->n, 24, &s);
    CHECK_EQ_I(r, x->n);
    CHECK_EQ_I(s.wait_cpu, 0); CHECK_EQ_I(s.level, V92_SUVU_NO_LEVEL); CHECK_EQ_I(s.silence, 0); CHECK_EQ_I(s.ack, 0);
    CHECK_EQ_I(v92_suvu_build(&s, 24, o), x->n);
    CHECK_EQ_I(ndiff(o, x->b, x->n), 0);
    printf("  %-9s SUVu (%d bits): no level measured, no wait, no silence; ours from it the same bits\n", rec, x->n);
}

static void mp_check(const char *rec, int max_ca, int max_ac, int trellis, int shaping, unsigned rates, const int *h)
{
    const fx_t *x = get(rec, "MP", 0);
    uint8_t o[V34_MP_MAX_BITS + 8]; v34_mp_t m; int r, k;
    if (!x) { CHECK(0); return; }
    r = v34_mp_parse(x->b, x->n, &m);
    CHECK_EQ_I(r, x->n);
    CHECK_EQ_I(m.type, 1); CHECK_EQ_I(m.max_ca, max_ca); CHECK_EQ_I(m.max_ac, max_ac); CHECK_EQ_I(m.aux, 0);
    CHECK_EQ_I(m.trellis, trellis); CHECK_EQ_I(m.nonlinear, 1); CHECK_EQ_I(m.shaping, shaping); CHECK_EQ_I(m.ack, 1);
    CHECK_EQ_I(m.rates, rates); CHECK_EQ_I(m.asym, 1);
    for (k = 0; k < 6; k++) CHECK_EQ_I(m.h[k / 2][k % 2], h[k]);
    CHECK_EQ_I(v34_mp_build(&m, o), x->n);
    CHECK_EQ_I(ndiff(o, x->b, x->n), 0);
    printf("  %-9s MP' type 1 (%d bits): %d / %d x 2400, rates 0x%04x; ours from it the same bits\n", rec, x->n, max_ca, max_ac, rates);
}

static void info1c_check(void)
{
    const fx_t *x = get("v34a", "INFO1c", 0);
    info1c_t q; uint8_t fr[INFO_MAX_BITS + 8];
    if (!x) { CHECK(0); return; }
    CHECK_EQ_I(x->n, INFO_PAYLOAD(INFO1C_BITS));
    CHECK(info_check(x->b, x->n));
    info1c_parse(x->b, &q);
    CHECK_EQ_I(q.min_pwr_red, 3); CHECK_EQ_I(q.add_pwr_red, 0); CHECK_EQ_I(q.md_len, 0);
    CHECK_EQ_I(q.sr[0].max_rate, 9); CHECK_EQ_I(q.sr[1].max_rate, 0); CHECK_EQ_I(q.sr[2].max_rate, 0);
    CHECK_EQ_I(q.sr[3].preemph, 1); CHECK_EQ_I(q.sr[3].max_rate, 11);
    CHECK_EQ_I(q.sr[4].preemph, 1); CHECK_EQ_I(q.sr[4].max_rate, 13);
    CHECK_EQ_I(q.sr[5].preemph, 1); CHECK_EQ_I(q.sr[5].max_rate, 0);
    CHECK_EQ_I(q.freq_off, -3);
    info_rebuild(x, fr, info1c_build(&q, fr), -1);
    printf("  v34a      INFO1c: power reduction 3, 3000 / 3200 at 11 / 13 x 2400, 0.06 Hz low; ours from it the same bits\n");
}

/* ---- what our caller chooses to send ---- */

/* the CM of orig with V.90 and V.92 offered: V.8 7.3 and 6.3, "If the PCM
 * modem availability category is present, the PSTN access category shall
 * also be present". All 93 calls of the two V.92 / V.90 makers have it,
 * 0x0d (analogue network, no cellular). */
static void our_cm(void)
{
    static orig_t o;
    orig_cfg c;
    uint8_t oct[V8_MAX_OCTETS], real[V8_MAX_OCTETS];
    v8_menu_t m;
    const fx_t *x = get("usr7", "CM", 0);
    int n, k, nr;
    memset(&c, 0, sizeof c);
    c.level = 3000;
    c.modes = V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21 | ORIG_MODE_V90 | ORIG_MODE_V92;
    orig_init(&o, &c);
    n = v8_encode(&o.v8.cfg.local, oct, V8_MAX_OCTETS);
    CHECK_EQ_I(v8_decode(oct, n, &m), 0);
    printf("  ours      CM");
    for (k = 0; k < n; k++) printf(" %02x", oct[k]);
    printf("\n");
    CHECK_EQ_I(m.has_pcm, 1); CHECK_EQ_I(m.pcm, V8_PCM_ANALOG);
    CHECK(m.modes & V8_MOD_V34);
    CHECK(oct[1] & 0x20);                           /* modn0 b5: PCM category present */
    CHECK_EQ_I(m.has_access, 1);
    CHECK_EQ_I(m.access, 0);
    /* the octets the real ones have: call function, modn0, PCM, access */
    if (!x) { CHECK(0); return; }
    nr = octets(x, real);
    CHECK(nr >= 6);
    CHECK_EQ_I(oct[0], real[0]); CHECK_EQ_I(oct[1], real[1]);
    CHECK(has_octet(oct, n, 0x27)); CHECK(has_octet(oct, n, 0x0d));
    CHECK(has_octet(real, nr, 0x27)); CHECK(has_octet(real, nr, 0x0d));
}

/* INFO0a as our caller sends it in V.90 / V.92 (modem/p2call, the first
 * one, after the 75 ms of silence): Table 14 as the real ones fill it but
 * for the symbol rates we do not do, bit 26 with V.92, the same CRC */
static void our_info0a(int v92)
{
    static p2call_t p;
    p2call_cfg c;
    info0_t q;
    int i;
    memset(&c, 0, sizeof c);
    c.level = 3000; c.uinfo = 90; c.v92 = v92;
    p2call_init_v90(&p, &c);
    for (i = 0; i < 4000 && p.tx.nbits == 0; i++) (void)p2call_tick(&p, 0);
    CHECK_EQ_I(p.tx.nbits, INFO0A_BITS);
    if (p.tx.nbits != INFO0A_BITS) return;
    CHECK(info_check(p.tx.bits + INFO_HDR_BITS, INFO_PAYLOAD(INFO0A_BITS)));
    info0a_parse(p.tx.bits + INFO_HDR_BITS, &q);
    CHECK_EQ_I(q.v92, v92); CHECK_EQ_I(q.short_p2, 0); CHECK_EQ_I(q.ack, 0);
    CHECK_EQ_I(q.lo3000 & q.hi3000 & q.lo3200 & q.hi3200, 1);
    CHECK_EQ_I(q.pwr_reduce, 1); CHECK_EQ_I(q.c1664, 1); CHECK_EQ_I(q.cme, 0);
    printf("  ours      INFO0a%s: 3000 and 3200 both carriers, power reduction, 1664 points, V.92 %d\n",
           v92 ? " (V.92)" : "", v92);
}

int main(void)
{
    static const int idx0[6] = { 0, 0, 0, 0, 0, 0 }, idx_x2[6] = { 0, 1, 2, 3, 4, 5 }, idx_f1[6] = { 0, 1, 2, 3, 4, 1 };
    static const int idx_cxu[6] = { 1, 1, 1, 1, 0, 0 }, idx_usru[6] = { 0, 1, 2, 2, 2, 2 };
    static const int h_v34a[6] = { -110, -144, 79, 128, -75, -59 }, h_x2b[6] = { 0, 0, 0, 0, 0, 0 };
    if (load(TEST_DATA_DIR "/realcheck.txt") < 0) { printf("realcheck: %s not found\n", TEST_DATA_DIR "/realcheck.txt"); CHECK(0); T_REPORT("realcheck"); }
    printf("real callers' sequences: our parser, and our builders given what it read (%d extracts)\n", nfx);
    CHECK_EQ_I(nfx, 32);

    printf("V.8 CM (V.8 6, 7.3)\n");
    cm_check("cxa", 0x141a, V8_PCM_ANALOG, V8_PROT_LAPM, 0);
    cm_check("usr7", 0x001a, V8_PCM_ANALOG, -1, 0);
    cm_check("x2r2", 0x001a, V8_PCM_ANALOG, -1, 0);
    cm_check("v34a", 0x141a, -1, V8_PROT_LAPM, -1);
    our_cm();

    printf("Phase 2: INFO0a / INFO0c (Table 14/V.34, 16/V.92), INFO1a (10/V.90, 18/V.92), INFO1c (15/V.34)\n");
    info0_check("cxa", "INFO0a", 1, 1);
    info0_check("usr7", "INFO0a", 1, 1);
    info0_check("x2r2", "INFO0a", 1, 0);
    info0_check("v34a", "INFO0c", 1, 0);
    our_info0a(0);
    our_info0a(1);
    /* the Conexant puts its frequency offset (-3, 0x3fd) in 40:49 of Table
     * 18 too, where the table has ones; the digital modem does not read them */
    info1a_check("cxa", 0, INFO1A_PCM, 0x00, 0, 79, 6, 0, 1);
    info1a_check("usr7", 0, INFO1A_PCM, 0x04, 0, 78, 6, 0, 0);     /* Ltot code 1: 256 */
    info1a_check("ab_cc_v90", 0, INFO1A_V90, 0x00, 0, 79, 4, -3, 0);
    info1a_check("usr7", 1, INFO1A_V90, 0x00, 0, 78, 4, 0, 0);     /* the V.90 retrain after V.92 */
    info1a_check("x2r2", 0, INFO1A_V90, 0x00, 0, 78, 4, 0, 0);
    info1c_check();

    printf("Phase 3: Ja (8.3.1/V.90, 8.5.4/V.92)\n");
    ja_check("ab_cc_v90", 119, 124, 126, H_CX, 79, 1);
    ja_check("f1", 197, 66, 66, H_USR, 0, 116);
    ja_check("x2r2", 197, 66, 66, H_USR, 0, 116);
    ja92_check("cxa", 119, 124, 126, H_CX, 79, 0);
    /* the 5686 ends its descriptor after the single fill bit 238 + ...,
     * 2092 bits; Table 20 fills to the next multiple of 12 */
    ja92_check("usr7", 197, 66, 66, H_USR, 0, 8);

    printf("Phases 3 and 4: CPt, CP (Table 14/V.90); CPt, CPu (Table 23/V.92), SUVu (Table 27/V.92); MP (Table 18/V.34)\n");
    /* the Conexant's CPt has one fill bit, not Table 14's three */
    cp_check("ab_cc_v90", "CPT", 0, 9, 0, 0x1fff, 1, 8192, idx0, 0, 4, 2);
    cp_check("f1", "CPT", 0, 16, 0, 0x07ff, 3, 8190, idx0, 0, 9, 0);
    cp_check("f1", "CP", 1, 21, 1, 0x07ff, 3, 8190, idx_f1, 1, 65, 0);
    cp_check("x2r2", "CPT", 0, 16, 0, 0x07ff, 3, 8190, idx_x2, 1, 9, 0);
    cp_check("x2r2", "CP", 1, 21, 1, 0x07ff, 3, 8190, idx_x2, 1, 65, 0);
    /* (cxa's CPt is followed by E1u, whose zeros the parser takes as fill) */
    cpu_check("cxa", "CPT92", 12, V92_UP_CPT, 9, 1, 8192, idx0, 0, 4, 0);
    cpu_check("usr7", "CPT92", 12, V92_UP_CPT, 16, 3, 8190, idx0, 0, 9, 0);
    cpu_check("cxa", "CPU", 24, V92_UP_CPU, 18, 1, 8192, idx_cxu, 0, 46, 0);
    cpu_check("usr7", "CPU", 24, V92_UP_CPU, 21, 3, 8190, idx_usru, 1, 65, 0);
    suvu_check("cxa");
    suvu_check("usr7");
    mp_check("v34a", 12, 13, 0, 0, 0x3fff, h_v34a);
    mp_check("x2b", 12, 12, 2, 1, 0x0ffe, h_x2b);

    T_REPORT("realcheck");
}
