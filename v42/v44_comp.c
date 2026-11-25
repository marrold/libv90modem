/* ITU-T V.44 (11/2000) data compression, stream method, from the
 * Recommendation text. Clause numbers in comments refer to V.44. */
#include <string.h>
#include "v44_comp.h"
#include "comp_out.h"

#if V44COMP_MAX_CODEWORDS < 256 || V44COMP_MAX_CODEWORDS > 65535
#error "V44COMP_MAX_CODEWORDS must be 256..65535"
#endif
#if V44COMP_MAX_HISTORY < 512 || V44COMP_MAX_HISTORY > 65535
#error "V44COMP_MAX_HISTORY must be 512..65535"
#endif

#define N5 V44COMP_N5

#define ESCAPE_STEP 51          /* 7.14 */
/* 7.11.5: the test is left open. Every TEST_WINDOW characters the bits the
 * codes took (compressed mode) or would have taken (transparent mode) are
 * compared with eight bits per character: compressed mode is left when a
 * window came out longer than it went in, and entered when a window would
 * have come out an eighth shorter. */
#define TEST_WINDOW 512

/* encoder: what is held (st) */
enum { ST_EMPTY = 0, ST_ROOT, ST_NODE };
/* encoder: what the character after a flush is appended to (pend) */
enum { PEND_NONE = 0, PEND_ROOT, PEND_NODE };
/* encoder: why string matching is being brought to an end (term) */
enum { TERM_NONE = 0, TERM_FLUSH, TERM_ETM, TERM_HIST };
/* decoder: previous code, for Table 2 */
enum { PREV_NONE = 0, PREV_ORDINAL, PREV_CODEWORD, PREV_EXTLEN };

static int params_ok(int n2, int n7, int n8)
{
    return n2 >= V44COMP_MIN_CODEWORDS && n2 <= V44COMP_MAX_CODEWORDS
        && n7 >= V44COMP_MIN_STRING && n7 <= V44COMP_MAX_STRING
        && n8 >= V44COMP_MIN_HISTORY && n8 <= V44COMP_MAX_HISTORY;
}

/* Table 11: N1 is derived from N2: the bits that hold codeword N2 - 1 */
static int bits_for(unsigned n2)
{
    int b = 6;
    while ((1ul << b) < n2) b++;
    return b;
}

/* Table 4: width of the last subfield of a long string-extension length */
static int ext_bits(int n7)
{
    return n7 <= 46 ? 5 : n7 <= 78 ? 6 : n7 <= 142 ? 7 : 8;
}

/* ------------------------------------------------- encoder: transfer */
COMP_OUT_WRITER(v44comp_enc_t, V44COMP_OVF)

/* The most one step puts out (comp_out.h): the end of a string extension
 * where matching is brought to an end (enc_finish_step): up to 7 bits
 * held, the codeword after every STEPUP from 6 bits up to N1, each with
 * its prefix bit, a string-extension length (at most 6 + 8 bits, Table 4),
 * REINIT for the node-tree it filled, ETM or FLUSH, and 0 bits to the
 * octet boundary. A character in transparent mode is two octets (7.14),
 * entering compressed mode two (6.5.2). */
#define N1_MAX COMP_OUT_BITS(V44COMP_MAX_CODEWORDS)
#define STEP_MAX ((7 + (N1_MAX - 6 + 3) * (N1_MAX + 1) + 14 + 7) / 8)
COMP_OUT_FITS(v44comp_step_fits_ovf, STEP_MAX, V44COMP_OVF);

/* 6.6: the least significant bit of a code follows the most significant
 * bit of the one before; bit 1 of an octet goes first. In transparent mode
 * the encoder carries on to see what the codes would cost (6.5), but
 * nothing of it is transferred. */
static void put_bits(v44comp_enc_t *e, uint32_t v, int n)
{
    e->win_bits += (uint32_t)n;
    if (!e->compressed) return;
    out_bits(e, v, n);
}

/* 0 bits up to the octet boundary (6.6) */
static void pad_to_octet(v44comp_enc_t *e)
{
    if (e->nbits > 0) e->win_bits += (uint32_t)(8 - e->nbits);
    out_pad(e);
}

/* 6.6.1, Table 5: prefix "1", the code in C2 bits */
static void put_control(v44comp_enc_t *e, unsigned code)
{
    put_bits(e, 1u | (code << 1), 1 + e->c2);
    e->after_cw = 0;
}

/* Table 5: prefix "0", or "0" "0" immediately after a codeword */
static void put_ordinal(v44comp_enc_t *e, unsigned ch)
{
    int np;
    if (ch > 127 && e->c5 == 7) {               /* 7.11.1 */
        e->c5 = 8;
        put_control(e, V44COMP_STEPUP);
    }
    np = e->after_cw ? 2 : 1;
    put_bits(e, ch << np, np + e->c5);
    e->after_cw = 0;
}

static void put_codeword(v44comp_enc_t *e, unsigned cw)
{
    while (cw >= e->c3) {                       /* 7.11.2 a) to d) */
        put_control(e, V44COMP_STEPUP);
        e->c2++;
        e->c3 <<= 1;
    }
    put_bits(e, 1u | (cw << 1), 1 + e->c2);
    e->after_cw = 1;
}

/* 6.6.2, Tables 3 and 4, behind the prefix "0" "1" of Table 5. Subfields
 * go in the order written, each least significant bit first. */
static void put_extlen(v44comp_enc_t *e, int len)
{
    if (len == 1)
        put_bits(e, 2u | (1u << 2), 3);                         /* "1" */
    else if (len <= 4)
        put_bits(e, 2u | ((uint32_t)(len - 1) << 3), 5);        /* "0", two bits */
    else if (len <= 12)
        put_bits(e, 2u | ((uint32_t)(len - 5) << 6), 9);        /* "0" "00" "0", three bits */
    else
        put_bits(e, 2u | (1u << 5) | ((uint32_t)(len - 13) << 6), 6 + ext_bits(e->n7));
    e->after_cw = 0;
}

/* ----------------------------------------------- encoder: dictionary */
/* 7.5.1. Characters in the history that have not been looked at yet are
 * moved to its first positions and are encoded with the new dictionary. */
static void dict_reset(v44comp_enc_t *e)
{
    memset(e->root, 0, sizeof e->root);
    e->c1 = N5;
    e->c2 = 6;
    e->c3 = 64;
    e->c5 = 7;
    e->after_cw = 0;
    e->st = ST_EMPTY;
    e->pend = PEND_NONE;
    if (e->scan < e->c4) memmove(e->hist, e->hist + e->scan, (size_t)(e->c4 - e->scan));
    e->c4 = (uint16_t)(e->c4 - e->scan);
    e->scan = 0;
}

/* 7.12: in compressed mode the peer is told with REINIT (in the codeword
 * size in force before it) */
static void dict_reinit(v44comp_enc_t *e)
{
    put_control(e, V44COMP_REINIT);
    dict_reset(e);
    e->reinits++;
}

/* A new string-segment of len characters at history position pos, below
 * segment parent, or below the root of ch when parent is 0. It takes the
 * next codeword. 7.11.3: when that was the last one, the dictionary is
 * reinitialized. Returns 1 when that happened. */
static int add_node(v44comp_enc_t *e, uint16_t parent, uint8_t ch, uint16_t pos, int len)
{
    uint16_t n = e->c1++;

    e->pos[n] = pos;
    e->len[n] = (uint8_t)len;
    e->down[n] = 0;
    if (parent) { e->side[n] = e->down[parent]; e->down[parent] = n; }
    else        { e->side[n] = e->root[ch];     e->root[ch] = n; }
    e->nodes++;
    if (e->c1 >= e->n2) { dict_reinit(e); return 1; }
    return 0;
}

/* Among the segments at one level, starting with first: the one of len
 * characters that matches. A segment longer than one character continues
 * the history behind its parent, and so does the input when this is asked,
 * which leaves the length to compare; a segment of one character is
 * compared with c. */
static uint16_t find_seg(const v44comp_enc_t *e, uint16_t first, int len, uint8_t c)
{
    uint16_t k;
    for (k = first; k != 0; k = e->side[k])
        if (e->len[k] == len && (len > 1 || e->hist[e->pos[k]] == c)) return k;
    return 0;
}

/* String matching moves down to segment n (6.3.1). scan is put behind it:
 * characters that were compared further along a longer segment which then
 * failed are looked at again from here. */
static void enter_node(v44comp_enc_t *e, uint16_t n)
{
    uint16_t k;
    unsigned lim = 0;

    e->st = ST_NODE;
    e->cur = n;
    e->slen = (uint16_t)(e->slen + e->len[n]);
    e->mpos = (uint16_t)(e->mpos + e->len[n]);
    e->scan = e->mpos;
    e->epos = (uint16_t)(e->pos[n] + e->len[n]);
    e->j = 0;
    e->best = 0; e->bestlen = 0;
    for (k = e->down[n]; k != 0; k = e->side[k])
        if (e->len[k] > lim) lim = e->len[k];
    e->lim = (uint16_t)(lim > 1 ? lim : 0);
}

/* The string ends at cur with j more characters matched in the history
 * behind it: codeword, string-extension length, and a segment for the
 * extension (6.3.2; 6.3.1 NOTE 1 when they are part of a longer segment) */
static void end_extension(v44comp_enc_t *e)
{
    put_codeword(e, e->cur);
    put_extlen(e, e->j);
    e->st = ST_EMPTY;
    add_node(e, e->cur, 0, e->mpos, e->j);
}

/* ------------------------------------------------- encoder: matching */
/* Look at the next character of the history (6.3). */
static void enc_step(v44comp_enc_t *e)
{
    uint8_t c;
    uint16_t n;

    if (e->st == ST_EMPTY) {
        if (e->pend != PEND_NONE) {
            /* 6.3.2, 7.13: the character after a flush starts a new string
             * and the dictionary is updated as if it were unmatched, also
             * where such a segment exists (the decoder makes a string
             * whatever the character is, Table 2) */
            int kind = e->pend;
            e->pend = PEND_NONE;
            if (kind == PEND_ROOT) add_node(e, 0, e->pend_ch, e->scan, 1);
            else if (e->pend_slen < e->n7) add_node(e, e->pend_node, 0, e->scan, 1);
        }
        e->rootch = e->hist[e->scan];
        e->st = ST_ROOT;
        e->slen = 1;
        e->scan++;
        e->mpos = e->scan;
        return;
    }
    c = e->hist[e->scan];
    if (e->st == ST_ROOT) {
        n = find_seg(e, e->root[e->rootch], 1, c);
        if (n) { enter_node(e, n); return; }
        /* 6.3.1, Table 1: string matching failed. Ordinal; the unmatched
         * character is appended to the root and is looked at again as a
         * first character. */
        put_ordinal(e, e->rootch);
        e->st = ST_EMPTY;
        add_node(e, 0, e->rootch, e->scan, 1);
        return;
    }
    if (e->j == 0 && !(e->lim && c == e->hist[e->epos])) {
        n = find_seg(e, e->down[e->cur], 1, c);
        if (n) { enter_node(e, n); return; }
        e->lim = 0;             /* cur is the longest string match: 6.3.2 */
    }
    if (e->lim) {
        /* Segments of up to lim characters branch from cur. They are all
         * beginnings of what follows cur's own segment in the history, so
         * one comparison there serves them all. */
        if (c == e->hist[e->epos + e->j]) {
            e->j++; e->scan++;
            n = find_seg(e, e->down[e->cur], e->j, c);
            if (n) { e->best = n; e->bestlen = e->j; }
            if (e->j == e->lim) enter_node(e, e->best);
            return;
        }
        if (e->best) { enter_node(e, e->best); return; }
        /* part of a segment and no complete one: 6.3.1 NOTE 1 */
    } else if (e->slen + e->j < e->n7 && c == e->hist[e->epos + e->j]) {
        /* string extension goes on (6.3.2) */
        e->j++; e->scan++;
        if (e->slen + e->j == e->n7) end_extension(e);
        return;
    }
    if (e->j > 0) {
        end_extension(e);
    } else {
        /* Table 1, string extension failed: the unmatched character is
         * appended to the last matched segment, unless the string would
         * pass N7 (Table 2 NOTE) */
        put_codeword(e, e->cur);
        e->st = ST_EMPTY;
        if (e->slen < e->n7) add_node(e, e->cur, 0, e->scan, 1);
    }
}

/* 6.5.2 */
static void enter_compressed(v44comp_enc_t *e)
{
    dict_reset(e);
    out_octet(e, e->esc);
    out_octet(e, V44COMP_ECM);
    e->compressed = 1;
    e->bitbuf = 0; e->nbits = 0;
    e->win_chars = 0; e->win_bits = 0;
    e->mode_switches++;
}

/* 6.5.1 b) to d). The dictionary of transparent mode is the encoder's own
 * business; it starts empty. */
static void enter_transparent(v44comp_enc_t *e)
{
    put_control(e, V44COMP_ETM);
    pad_to_octet(e);
    e->compressed = 0;
    dict_reset(e);
    e->win_chars = 0; e->win_bits = 0;
    e->mode_switches++;
}

/* All of the history has been looked at and matching has to stop (flush,
 * history full, leaving compressed mode): transfer what is held (6.3.1,
 * 6.3.2), then do what it stopped for. */
static void enc_finish_step(v44comp_enc_t *e)
{
    switch (e->st) {
    case ST_ROOT:
        put_ordinal(e, e->rootch);
        e->pend = PEND_ROOT; e->pend_ch = e->rootch;
        e->st = ST_EMPTY;
        break;
    case ST_NODE:
        if (e->j == 0) {
            put_codeword(e, e->cur);
            e->pend = PEND_NODE; e->pend_node = e->cur; e->pend_slen = e->slen;
            e->st = ST_EMPTY;
        } else if (e->best) {
            enter_node(e, e->best);     /* and on from there */
            return;
        } else {
            end_extension(e);
        }
        break;
    default:
        break;
    }
    switch (e->term) {
    case TERM_FLUSH:                    /* 7.13 d), e) */
        put_control(e, V44COMP_FLUSH);
        pad_to_octet(e);
        e->dirty = 0;
        break;
    case TERM_ETM:
        enter_transparent(e);
        break;
    default:                            /* 7.11.4; not twice if the last
                                           code filled the node-tree too */
        if (e->c4 >= e->n8) dict_reinit(e);
        break;
    }
    e->term = TERM_NONE;
}

/* 7.11.5, before a character is taken. Returns 1 when compressed mode is
 * to be left first. */
static int test_compression(v44comp_enc_t *e)
{
    uint32_t raw;

    if (e->policy == V44COMP_ALWAYS) {
        if (!e->compressed) enter_compressed(e);
        return 0;
    }
    if (e->policy == V44COMP_NEVER) {
        if (e->compressed) { e->term = TERM_ETM; return 1; }
        return 0;
    }
    if (e->win_chars < TEST_WINDOW) return 0;
    raw = e->win_chars * 8;
    e->win_chars = 0;
    if (!e->compressed) {
        /* Compressed mode would begin with an empty dictionary (6.5.2), so
         * the trial encoding of each window begins with one as well. */
        uint32_t bits = e->win_bits;
        e->win_bits = 0;
        if (bits < raw - raw / 8) enter_compressed(e);
        else dict_reset(e);
        return 0;
    }
    if (e->win_bits > raw) { e->term = TERM_ETM; return 1; }
    e->win_bits = 0;
    return 0;
}

/* The pump behind C-DATA and C-FLUSH. Returns 1 when there is nothing
 * left to do, 0 when the output buffer filled up first. */
static int enc_run(v44comp_enc_t *e, const uint8_t *in, size_t inlen, size_t *used, int flush)
{
    for (;;) {
        if (e->ovf_len != 0 || e->out_len >= e->out_max) return 0;
        if (e->scan < e->c4) { enc_step(e); continue; }
        if (e->term == TERM_NONE && e->c4 >= e->n8) e->term = TERM_HIST;       /* 7.11.4 */
        if (e->term != TERM_NONE) { enc_finish_step(e); continue; }
        if (*used < inlen) {
            uint8_t c;
            if (test_compression(e)) continue;
            c = in[(*used)++];
            e->in_chars++; e->win_chars++;
            e->dirty = 1;
            if (!e->compressed) {
                /* 6.6: as it is, octet aligned; 7.14 */
                out_octet(e, c);
                if (c == e->esc) {
                    out_octet(e, V44COMP_EID);
                    e->esc = (uint8_t)(e->esc + ESCAPE_STEP);
                }
            }
            e->hist[e->c4++] = c;
            continue;
        }
        if (flush && e->dirty) {
            if (e->compressed) { e->term = TERM_FLUSH; continue; }
            /* 7.13, transparent mode: everything went out as it came. What
             * a flush would have cost goes into the comparison. */
            e->win_bits += (uint32_t)(1 + e->c2 + 4);
            e->dirty = 0;
        }
        return 1;
    }
}

/* ------------------------------------------------------ encoder: API */
int v44comp_enc_init(v44comp_enc_t *e, int n2, int n7, int n8)
{
    if (!params_ok(n2, n7, n8)) return -1;
    memset(e, 0, sizeof *e);
    e->n2 = (uint16_t)n2;
    e->n7 = (uint16_t)n7;
    e->n8 = (uint16_t)n8;
    e->n1 = bits_for((unsigned)n2);
    dict_reset(e);
    e->compressed = 1;                  /* 7.5 */
    e->esc = 0;
    e->policy = V44COMP_AUTO;
    return 0;
}

void v44comp_enc_policy(v44comp_enc_t *e, int policy)
{
    e->policy = policy;
}

size_t v44comp_enc_pending(const v44comp_enc_t *e)
{
    return out_pending(e);
}

size_t v44comp_encode(v44comp_enc_t *e, const uint8_t *in, size_t inlen,
                      uint8_t *out, size_t outmax, size_t *outlen)
{
    size_t used = 0;

    out_begin(e, out, outmax, *outlen);
    enc_run(e, in, inlen, &used, 0);
    *outlen = out_end(e);
    return used;
}

int v44comp_flush(v44comp_enc_t *e, uint8_t *out, size_t outmax, size_t *outlen)
{
    size_t used = 0;
    int done;

    out_begin(e, out, outmax, *outlen);
    done = enc_run(e, NULL, 0, &used, 1);
    *outlen = out_end(e);
    return done && e->ovf_len == 0;
}

void v44comp_enc_put(v44comp_enc_t *e, int type, unsigned value,
                     uint8_t *out, size_t outmax, size_t *outlen)
{
    out_begin(e, out, outmax, *outlen);
    switch (type) {
    case V44COMP_CODE_ORDINAL:  put_ordinal(e, value & 0xFF); break;
    case V44COMP_CODE_CODEWORD: put_codeword(e, value); break;
    case V44COMP_CODE_EXTLEN:   put_extlen(e, (int)value); break;
    default:
        if (value == V44COMP_ETM) {
            enter_transparent(e);
        } else {
            put_control(e, value);
            if (value == V44COMP_FLUSH) pad_to_octet(e);
        }
        break;
    }
    *outlen = out_end(e);
}

/* --------------------------------------------------------------- decoder */
/* 7.5.2 */
static void dec_reset(v44comp_dec_t *x)
{
    x->c1 = N5;
    x->c2 = 6;
    x->c4 = 0;
    x->c5 = 7;
    x->done = 0;
    x->prev = PREV_NONE;
    x->prev_len = 0;
    x->after_cw = 0;
}

int v44comp_dec_init(v44comp_dec_t *x, int n2, int n7, int n8)
{
    if (!params_ok(n2, n7, n8)) return -1;
    memset(x, 0, sizeof *x);
    x->n2 = (uint16_t)n2;
    x->n7 = (uint16_t)n7;
    x->n8 = (uint16_t)n8;
    x->n1 = bits_for((unsigned)n2);
    dec_reset(x);
    x->compressed = 1;                  /* 7.5 */
    return 0;
}

int v44comp_dec_pending(const v44comp_dec_t *x)
{
    return !x->error && x->done < x->c4;
}

/* 6.4.2, Table 2: a string of len characters ending at history position
 * last takes the next codeword. A full string collection takes no more
 * (the peer reinitializes when its node-tree is full, 7.11.3). */
static void new_string(v44comp_dec_t *x, unsigned last, unsigned len)
{
    if (x->c1 >= x->n2) return;
    x->last[x->c1] = (uint16_t)last;
    x->slen[x->c1] = (uint8_t)len;
    x->c1++;
}

/* copy n characters from history position src to the end of the history,
 * one at a time: the source may run into what is being written (II.2) */
static void copy_hist(v44comp_dec_t *x, unsigned src, unsigned n)
{
    while (n-- > 0) x->hist[x->c4++] = x->hist[src++];
}

/* 6.4.1 1) */
static int dec_ordinal(v44comp_dec_t *x, unsigned ch)
{
    if (x->c4 >= x->n8) return V44COMP_ERR_HISTORY;
    x->hist[x->c4++] = (uint8_t)ch;
    /* Table 2: appended to the previous character or string; nothing after
     * a string-extension length or at the start of a dictionary */
    if (x->prev == PREV_ORDINAL) new_string(x, x->c4 - 1u, 2);
    else if (x->prev == PREV_CODEWORD && x->prev_len < x->n7) new_string(x, x->c4 - 1u, x->prev_len + 1u);
    x->prev = PREV_ORDINAL;
    x->prev_len = 1;
    x->after_cw = 0;
    return 0;
}

static int dec_codeword(v44comp_dec_t *x, unsigned cw)
{
    unsigned p = x->c4, len;
    int grows = x->prev == PREV_ORDINAL || (x->prev == PREV_CODEWORD && x->prev_len < x->n7);

    if (cw > x->c1) return V44COMP_ERR_CODEWORD;        /* 6.4.1 6) */
    if (cw == x->c1) {
        /* 6.4.1 3), 4): the string the encoder made last and this decoder
         * is about to make: the previous string and its first character */
        if (!grows || x->c1 >= x->n2) return V44COMP_ERR_CODEWORD;
        len = x->prev_len + 1u;
        if (p + len > x->n8) return V44COMP_ERR_HISTORY;
        copy_hist(x, p - x->prev_len, len);
        new_string(x, p, len);
    } else {
        /* 6.4.1 2) */
        len = x->slen[cw];
        if (p + len > x->n8) return V44COMP_ERR_HISTORY;
        copy_hist(x, x->last[cw] + 1u - len, len);
        /* Table 2: its first character is appended to what came before */
        if (grows) new_string(x, p, x->prev_len + 1u);
    }
    x->prev = PREV_CODEWORD;
    x->prev_len = (uint16_t)len;
    x->prev_cw = (uint16_t)cw;
    x->after_cw = 1;
    return 0;
}

/* 6.4.1 5) */
static int dec_extlen(v44comp_dec_t *x, unsigned n)
{
    unsigned cw = x->prev_cw, len = x->slen[cw] + n;

    if (len > x->n7) return V44COMP_ERR_EXTEND;
    if (x->c4 + n > x->n8) return V44COMP_ERR_HISTORY;
    copy_hist(x, x->last[cw] + 1u, n);
    new_string(x, x->c4 - 1u, len);     /* Table 2 */
    x->prev = PREV_EXTLEN;
    x->prev_len = (uint16_t)len;
    x->after_cw = 0;
    return 0;
}

/* have n bits in the buffer; 0 when the input runs out first */
static int need_bits(v44comp_dec_t *x, int n, const uint8_t *in, size_t inlen, size_t *used)
{
    while (x->nbits < n) {
        if (*used == inlen) return 0;
        x->bitbuf |= (uint32_t)in[(*used)++] << x->nbits;
        x->nbits += 8;
        x->in_octets++;
    }
    return 1;
}

static void drop_bits(v44comp_dec_t *x, int n)
{
    x->bitbuf >>= n;
    x->nbits -= n;
}

/* One code in compressed mode (6.4, 6.6.3). Octets are taken from the
 * input only as the code needs them, so that what is left in the buffer
 * after a code is always less than an octet: the 0 bits a FLUSH or ETM is
 * followed by. Returns 1 when a code was processed, 0 when more input is
 * needed, or a C-ERROR code. */
static int dec_code(v44comp_dec_t *x, const uint8_t *in, size_t inlen, size_t *used)
{
    unsigned v;
    int total, rc;

    if (!need_bits(x, 1, in, inlen, used)) return 0;
    if (x->bitbuf & 1) {
        /* control code or codeword, in C2 bits */
        if (!need_bits(x, 1 + x->c2, in, inlen, used)) return 0;
        v = (x->bitbuf >> 1) & ((1u << x->c2) - 1);
        if (v == V44COMP_STEPUP) {
            /* 7.11.1, 7.11.2: the code it is sent before tells which size
             * goes up: an ordinal (prefix "0") or a codeword or control
             * code (prefix "1") */
            if (!need_bits(x, 2 + x->c2, in, inlen, used)) return 0;
            drop_bits(x, 1 + x->c2);
            if (x->bitbuf & 1) {
                if (x->c2 >= x->n1) return V44COMP_ERR_STEPUP;
                x->c2++;
            } else {
                if (x->c5 >= 8) return V44COMP_ERR_ORDINAL;
                x->c5++;
            }
            x->after_cw = 0;
            return 1;
        }
        drop_bits(x, 1 + x->c2);
        switch (v) {
        case V44COMP_ETM:               /* 6.5.1 */
            x->bitbuf = 0; x->nbits = 0;
            x->compressed = 0;
            x->after_cw = 0;
            return 1;
        case V44COMP_FLUSH:             /* 7.13: octet alignment, nothing else */
            x->bitbuf = 0; x->nbits = 0;
            x->after_cw = 0;
            return 1;
        case V44COMP_REINIT:            /* 7.12 */
            dec_reset(x);
            x->reinits++;
            return 1;
        default:
            rc = dec_codeword(x, v);
            return rc ? rc : 1;
        }
    }
    if (!x->after_cw) {
        if (!need_bits(x, 1 + x->c5, in, inlen, used)) return 0;
        v = (x->bitbuf >> 1) & ((1u << x->c5) - 1);
        drop_bits(x, 1 + x->c5);
        rc = dec_ordinal(x, v);
        return rc ? rc : 1;
    }
    /* immediately after a codeword: "0" "0" ordinal, "0" "1" extension */
    if (!need_bits(x, 2, in, inlen, used)) return 0;
    if (!(x->bitbuf & 2)) {
        if (!need_bits(x, 2 + x->c5, in, inlen, used)) return 0;
        v = (x->bitbuf >> 2) & ((1u << x->c5) - 1);
        drop_bits(x, 2 + x->c5);
        rc = dec_ordinal(x, v);
        return rc ? rc : 1;
    }
    /* Tables 3 and 4 */
    if (!need_bits(x, 3, in, inlen, used)) return 0;
    if (x->bitbuf & 4) {
        v = 1; total = 3;
    } else {
        if (!need_bits(x, 5, in, inlen, used)) return 0;
        v = (x->bitbuf >> 3) & 3;
        if (v != 0) {
            v += 1; total = 5;
        } else {
            if (!need_bits(x, 6, in, inlen, used)) return 0;
            if (!(x->bitbuf & 32)) {
                if (!need_bits(x, 9, in, inlen, used)) return 0;
                v = 5 + ((x->bitbuf >> 6) & 7); total = 9;
            } else {
                int nb = ext_bits(x->n7);
                if (!need_bits(x, 6 + nb, in, inlen, used)) return 0;
                v = 13 + ((x->bitbuf >> 6) & ((1u << nb) - 1)); total = 6 + nb;
            }
        }
    }
    drop_bits(x, total);
    rc = dec_extlen(x, v);
    return rc ? rc : 1;
}

int v44comp_decode(v44comp_dec_t *x, const uint8_t *in, size_t inlen, size_t *consumed,
                   uint8_t *out, size_t outmax, size_t *outlen)
{
    size_t used = 0, n = *outlen;

    *consumed = 0;
    if (x->error) return x->error;
    for (;;) {
        /* C-DATA indication: what the last code put into the history */
        while (x->done < x->c4 && n < outmax) { out[n++] = x->hist[x->done++]; x->out_chars++; }
        if (x->done < x->c4) break;
        if (x->compressed) {
            int r = dec_code(x, in, inlen, &used);
            if (r < 0) x->error = r;
            if (r <= 0) break;
        } else {
            /* 6.5: characters and commands; the dictionary is left alone */
            uint8_t o;
            if (used == inlen || n == outmax) break;
            o = in[used++];
            x->in_octets++;
            if (x->escaped) {
                x->escaped = 0;
                if (o == V44COMP_ECM) {                 /* 6.5.2 */
                    dec_reset(x);
                    x->compressed = 1;
                    x->bitbuf = 0; x->nbits = 0;
                } else if (o == V44COMP_EID) {          /* 7.14 */
                    out[n++] = x->esc; x->out_chars++;
                    x->esc = (uint8_t)(x->esc + ESCAPE_STEP);
                } else {
                    /* EPM: parameter mode (7.4.2) is never agreed to in
                     * XID; anything else is reserved */
                    x->error = V44COMP_ERR_COMMAND;
                    break;
                }
            } else if (o == x->esc) {
                x->escaped = 1;
            } else {
                out[n++] = o; x->out_chars++;
            }
        }
    }
    *consumed = used;
    *outlen = n;
    return x->error;
}
