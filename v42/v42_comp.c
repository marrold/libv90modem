/* ITU-T V.42 bis (01/90) data compression, from the Recommendation text.
 * Clause numbers in comments refer to V.42 bis. */
#include <string.h>
#include "v42_comp.h"
#include "comp_out.h"

#define N5 V42COMP_N5
#define N6 V42COMP_N6

/* Table 2: control codewords (compressed mode), command codes (transparent) */
enum { CW_ETM = 0, CW_FLUSH = 1, CW_STEPUP = 2 };
enum { CC_ECM = 0, CC_EID = 1, CC_RESET = 2 };

#define ESCAPE_STEP 51          /* 9.2 b) */
#define TEST_WINDOW 64          /* characters per compressibility test (7.8) */

#define ROOT(c) ((uint16_t)((c) + N6))      /* 6.2 */

/* ------------------------------------------------------------ dictionary */
static int dict_init(v42comp_dict_t *d, int n2, int n7)
{
    if (n2 < V42COMP_MIN_CODEWORDS || n2 > V42COMP_MAX_CODEWORDS) return -1;
    if (n7 < V42COMP_MIN_STRING || n7 > V42COMP_MAX_STRING) return -1;
    memset(d, 0, sizeof *d);
    d->n2 = (uint16_t)n2;
    d->n7 = (uint16_t)n7;
    d->c1 = N5;                 /* 6.2: each tree is a lone root, C1 = N5 */
    return 0;
}

/* node for string(node) + c, or 0 */
static uint16_t dict_find(const v42comp_dict_t *d, uint16_t node, uint8_t c)
{
    uint16_t n = d->child[node];
    int guard = d->n2;
    while (n != 0 && guard-- > 0) {
        if (d->ch[n] == c) return n;
        n = d->sibling[n];
    }
    return 0;
}

static void dict_detach(v42comp_dict_t *d, uint16_t n)
{
    uint16_t p = d->parent[n], k;
    int guard = d->n2;
    if (d->child[p] == n) {
        d->child[p] = d->sibling[n];
    } else {
        for (k = d->child[p]; k != 0 && guard-- > 0; k = d->sibling[k])
            if (d->sibling[k] == n) { d->sibling[k] = d->sibling[n]; break; }
    }
    d->parent[n] = 0;
    d->sibling[n] = 0;
}

/* 6.4: attach string(node) + c at C1; then 6.5: move C1 to the next entry
 * that is empty or a leaf, detaching the leaf. Returns the new codeword. */
static uint16_t dict_add(v42comp_dict_t *d, uint16_t node, uint8_t c)
{
    uint16_t code = d->c1;
    int guard = d->n2;

    d->parent[code] = node;
    d->ch[code] = c;
    d->child[code] = 0;
    d->sibling[code] = d->child[node];
    d->child[node] = code;
    d->added++;
    do {
        if (++d->c1 > d->n2 - 1) d->c1 = N5;            /* 6.5 a), b) */
    } while (d->parent[d->c1] != 0 && d->child[d->c1] != 0 && guard-- > 0);  /* c) */
    if (d->parent[d->c1] != 0) {                        /* d) leaf: recover */
        dict_detach(d, d->c1);
        d->recovered++;
    }
    return code;
}

/* One step of the string matching procedure (6.3) with the dictionary
 * update that follows it (6.4, 6.5), as in Figures I-3 and I-7. Returns the
 * codeword of the string that just ended (the longest match, to be sent in
 * compressed mode), or 0 if the string was extended or nothing is owed. */
static uint16_t match_step(v42comp_dict_t *d, uint8_t c)
{
    uint16_t n, old, made = 0;

    if (d->str == 0) {
        d->str = ROOT(c); d->str_len = 1; d->exc = 0;
        return 0;
    }
    n = dict_find(d, d->str, c);
    if (d->exc) {
        /* Figure I-7: the string went out early (flush or mode change);
         * this character is the unmatched one. */
        d->exc = 0;
        old = 0;
    } else {
        /* 6.3 b): extend, unless the match is the entry made last time */
        if (n != 0 && n != d->last_new) {
            d->str = n; d->str_len++;
            return 0;
        }
        old = d->str;
    }
    if (n == 0 && d->str_len < d->n7)                   /* 6.4 a), b) */
        made = dict_add(d, d->str, c);
    d->last_new = made;
    d->str = ROOT(c); d->str_len = 1;
    return old;
}

static int bits_for(unsigned n2)
{
    int b = V42COMP_N3 + 1;
    while ((1u << b) < n2) b++;
    return b;
}

/* --------------------------------------------------------------- encoder */
int v42comp_enc_init(v42comp_enc_t *e, int n2, int n7)
{
    if (dict_init(&e->d, n2, n7) != 0) return -1;
    memset((char *)e + sizeof e->d, 0, sizeof *e - sizeof e->d);
    e->n1 = bits_for((unsigned)n2);
    e->c2 = V42COMP_N3 + 1;             /* 7.2 b) */
    e->c3 = 2 * V42COMP_N4;             /* 7.2 c) */
    e->policy = V42COMP_AUTO;
    return 0;
}

void v42comp_enc_policy(v42comp_enc_t *e, int policy)
{
    e->policy = policy;
}

COMP_OUT_WRITER(v42comp_enc_t, V42COMP_OVF)

/* The most one step puts out (comp_out.h): a character before which the
 * test leaves compressed mode (7.8.2): up to 7 bits held, the string's
 * codeword after every STEPUP from 9 bits up to N1, ETM, 0 bits to the
 * octet boundary, then the character and EID in transparent mode. Flushing (7.9) and entering
 * compressed mode (7.8.1) put out less. */
#define N1_MAX COMP_OUT_BITS(V42COMP_MAX_CODEWORDS)
#define STEP_MAX ((7 + (N1_MAX - (V42COMP_N3 + 1) + 2) * N1_MAX + 7) / 8 + 2)
COMP_OUT_FITS(v42comp_step_fits_ovf, STEP_MAX, V42COMP_OVF);

size_t v42comp_enc_pending(const v42comp_enc_t *e)
{
    return out_pending(e);
}

/* 7.5: codewords are packed least significant bit first */
static void put_bits(v42comp_enc_t *e, unsigned v, int n)
{
    e->win_bits += (uint32_t)n;
    out_bits(e, v, n);
}

/* 7.4 and Figure I-4: step the codeword size up as far as needed */
static void put_codeword(v42comp_enc_t *e, unsigned code)
{
    while (code >= e->c3) {
        put_bits(e, CW_STEPUP, e->c2);
        e->c2++;
        e->c3 *= 2;
    }
    put_bits(e, code, e->c2);
}

/* 7.8.1 */
static void enter_compressed(v42comp_enc_t *e)
{
    out_octet(e, e->esc);
    out_octet(e, CC_ECM);
    e->compressed = 1;
    e->bitbuf = 0; e->nbits = 0;
    if (e->d.str != 0) e->d.exc = 1;
    e->mode_switches++;
}

/* 7.8.2 */
static void enter_transparent(v42comp_enc_t *e)
{
    if (e->d.str != 0 && !e->d.exc) put_codeword(e, e->d.str);
    put_bits(e, CW_ETM, e->c2);
    out_pad(e);
    e->compressed = 0;
    if (e->d.str != 0) e->d.exc = 1;
    e->mode_switches++;
}

/* 7.8: the test itself is left open by the Recommendation. Here: over each
 * window of input, compare the bits the codewords took (compressed mode) or
 * would have taken (transparent mode, one codeword per string ended) with
 * eight bits per character. */
static void test_compression(v42comp_enc_t *e)
{
    uint32_t raw;

    if (e->policy == V42COMP_ALWAYS) {
        if (!e->compressed) enter_compressed(e);
        return;
    }
    if (e->policy == V42COMP_NEVER) {
        if (e->compressed) enter_transparent(e);
        return;
    }
    if (e->win_chars < TEST_WINDOW) return;
    raw = e->win_chars * 8;
    if (!e->compressed) {
        if (e->win_bits < raw - raw / 16) enter_compressed(e);
    } else {
        if (e->win_bits > raw) enter_transparent(e);
    }
    e->win_chars = 0;
    e->win_bits = 0;
}

static void enc_char(v42comp_enc_t *e, uint8_t c)
{
    uint16_t old;

    test_compression(e);
    e->in_chars++;
    e->win_chars++;
    old = match_step(&e->d, c);
    if (e->compressed) {
        if (old != 0) put_codeword(e, old);
    } else {
        if (old != 0) {
            int b = bits_for((unsigned)old + 1);
            e->win_bits += (uint32_t)(b > e->c2 ? b : e->c2);
        }
        out_octet(e, c);                                /* 7.5 */
    }
    /* 9.2 and Figure I-8 */
    if (c == e->esc) {
        if (!e->compressed) out_octet(e, CC_EID);
        e->esc = (uint8_t)(e->esc + ESCAPE_STEP);
    }
}

size_t v42comp_encode(v42comp_enc_t *e, const uint8_t *in, size_t inlen,
                      uint8_t *out, size_t outmax, size_t *outlen)
{
    size_t used = 0;

    out_begin(e, out, outmax, *outlen);
    while (used < inlen && e->ovf_len == 0 && e->out_len < e->out_max)
        enc_char(e, in[used++]);
    *outlen = out_end(e);
    return used;
}

int v42comp_flush(v42comp_enc_t *e, uint8_t *out, size_t outmax, size_t *outlen)
{
    out_begin(e, out, outmax, *outlen);
    if (e->ovf_len == 0 && e->compressed && e->d.str != 0 && !e->d.exc) {
        /* 7.9 a) to c) */
        put_codeword(e, e->d.str);
        e->d.exc = 1;
        if (e->nbits > 0) {
            put_bits(e, CW_FLUSH, e->c2);
            out_pad(e);
        }
    }
    *outlen = out_end(e);
    return e->ovf_len == 0;
}

/* --------------------------------------------------------------- decoder */
int v42comp_dec_init(v42comp_dec_t *x, int n2, int n7)
{
    if (dict_init(&x->d, n2, n7) != 0) return -1;
    memset((char *)x + sizeof x->d, 0, sizeof *x - sizeof x->d);
    x->n1 = bits_for((unsigned)n2);
    x->c2 = V42COMP_N3 + 1;
    return 0;
}

int v42comp_dec_pending(const v42comp_dec_t *x)
{
    return !x->error && x->compressed && x->nbits >= x->c2;
}

/* length of the string for a codeword, or 0 if the tree is broken */
static int string_len(const v42comp_dict_t *d, unsigned code)
{
    int len = 1;
    while (code >= N5) {
        code = d->parent[code];
        if (++len > d->n7) return 0;
    }
    return code >= N6 ? len : 0;
}

int v42comp_decode(v42comp_dec_t *x, const uint8_t *in, size_t inlen, size_t *consumed,
                   uint8_t *out, size_t outmax, size_t *outlen)
{
    v42comp_dict_t *d = &x->d;
    size_t used = 0, n = *outlen;

    *consumed = 0;
    if (x->error) return x->error;
    for (;;) {
        if (!x->compressed) {
            uint8_t o;
            if (used == inlen || n == outmax) break;
            o = in[used++];
            x->in_octets++;
            if (x->escaped) {
                x->escaped = 0;
                if (o == CC_ECM) {
                    /* 7.8.1: the string in hand becomes the one the first
                     * codeword's prefix character is appended to */
                    x->compressed = 1; x->bitbuf = 0; x->nbits = 0;
                } else if (o == CC_EID) {
                    match_step(d, x->esc);
                    out[n++] = x->esc; x->out_chars++;
                    x->esc = (uint8_t)(x->esc + ESCAPE_STEP);
                } else if (o == CC_RESET) {
                    unsigned long resets = x->resets + 1, oc = x->out_chars, io = x->in_octets;
                    v42comp_dec_init(x, d->n2, d->n7);
                    x->resets = resets; x->out_chars = oc; x->in_octets = io;
                } else {
                    x->error = V42COMP_ERR_COMMAND;     /* 5.8 d) */
                    break;
                }
            } else if (o == x->esc) {
                x->escaped = 1;
            } else {
                match_step(d, o);
                out[n++] = o; x->out_chars++;
            }
        } else {
            unsigned code;
            if (x->nbits < x->c2) {
                if (used == inlen) break;
                x->bitbuf |= (uint32_t)in[used++] << x->nbits;
                x->nbits += 8;
                x->in_octets++;
                continue;
            }
            code = x->bitbuf & ((1u << x->c2) - 1);
            if (code >= N6) {
                int len, i;
                unsigned k;
                uint16_t made = 0;
                uint8_t first;

                if (code >= d->n2) { x->error = V42COMP_ERR_RANGE; break; }
                if (code == d->c1) { x->error = V42COMP_ERR_C1; break; }        /* 5.8 b) */
                if (code >= N5 && d->parent[code] == 0) {
                    x->error = V42COMP_ERR_EMPTY; break;                        /* 5.8 c) */
                }
                if ((len = string_len(d, code)) == 0) { x->error = V42COMP_ERR_RANGE; break; }
                if (outmax - n < (size_t)len) break;     /* wait for room */
                x->bitbuf >>= x->c2; x->nbits -= x->c2;
                for (k = code, i = len - 1; i > 0; i--, k = d->parent[k])
                    out[n + (size_t)i] = d->ch[k];
                first = (uint8_t)(k - N6);
                out[n] = first;
                for (i = 0; i < len; i++)               /* 9.2 b) */
                    if (out[n + (size_t)i] == x->esc) x->esc = (uint8_t)(x->esc + ESCAPE_STEP);
                n += (size_t)len; x->out_chars += (unsigned long)len;
                /* clause 8: previous string plus the prefix character of
                 * this one becomes a new entry (6.4) */
                if (d->str != 0 && d->str_len < d->n7 && dict_find(d, d->str, first) == 0)
                    made = dict_add(d, d->str, first);
                d->last_new = made;
                if (code >= N5 && d->parent[code] == 0) {
                    /* entry recovered from under the decoded string: the
                     * peer encoder cannot have done that */
                    x->error = V42COMP_ERR_RANGE; break;
                }
                d->str = (uint16_t)code; d->str_len = (uint16_t)len; d->exc = 0;
            } else {
                x->bitbuf >>= x->c2; x->nbits -= x->c2;
                if (code == CW_STEPUP) {
                    if (x->c2 + 1 > x->n1) { x->error = V42COMP_ERR_STEPUP; break; }   /* 5.8 a) */
                    x->c2++;
                } else {
                    /* ETM and FLUSH: discard the fill bits (7.5) */
                    x->bitbuf = 0; x->nbits = 0;
                    if (code == CW_ETM) {
                        x->compressed = 0;
                        if (d->str != 0) d->exc = 1;    /* 7.8.2 b) */
                    }
                }
            }
        }
    }
    *consumed = used;
    *outlen = n;
    return x->error;
}
