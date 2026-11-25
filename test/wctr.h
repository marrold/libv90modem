/* Test data: 16-bit counter words (test/v90link.h's vl_t.words, the V.34
 * engine's, x2's and K56flex's upstream tests). Host only. */
#ifndef V90D_TEST_WCTR_H
#define V90D_TEST_WCTR_H
#include <stdint.h>

/* 16-bit counter words, MSB first: a gap shows as words lost. The sink
 * locks on three words in a row, n, n + 1, n + 2, at any bit position; a word that is not the next one unlocks it, and the next lock
 * counts the words between as lost (a damaged word among them). From where
 * the new lock falls it also counts the bits missing from the stream (net;
 * fewer than none: bits inserted): a relock with any is a break, where a
 * damaged word with nothing missing is none */
typedef struct {
    uint16_t gen; int gbit; uint64_t sr; int nb, locked, ph, had; uint16_t want;
    int64_t words, lost;
    int64_t nbits, at;              /* bits in; the count at which the word wanted ends */
    int64_t breaks, gap_bits;       /* relocks with bits missing or inserted; the bits missing, net */
    int64_t again;                  /* words received twice (a relock behind the one wanted) */
    int64_t lead;                   /* bits in before the first word of the first lock */
    int lead_tail;                  /* ... and they are the tail of the word before it (a stream
                                       resumed mid-word), not other bits */
} wctr_t;
static int wctr_get(wctr_t *w)
{
    int bit = (w->gen >> (15 - w->gbit)) & 1;
    if (++w->gbit == 16) { w->gbit = 0; w->gen++; }
    return bit;
}
static void wctr_put(wctr_t *w, int bit)
{
    uint16_t a, b, c;
    w->sr = (w->sr << 1) | (uint64_t)(bit & 1);
    w->nbits++;
    if (w->nb < 48) w->nb++;
    if (w->locked) {
        if (++w->ph < 16) return;
        w->ph = 0;
        if ((uint16_t)(w->sr & 0xffff) == w->want) { w->words++; w->want++; w->at += 16; return; }
        w->locked = 0;
        return;
    }
    if (w->nb < 48) return;
    a = (uint16_t)(w->sr >> 32); b = (uint16_t)(w->sr >> 16); c = (uint16_t)w->sr;
    if ((uint16_t)(a + 1) != b || (uint16_t)(b + 1) != c) return;
    if (!w->had) {
        uint64_t m;
        w->lead = w->nbits - 48;
        m = w->lead > 0 && w->lead < 16 ? ((uint64_t)1 << w->lead) - 1 : 0;
        w->lead_tail = w->lead == 0 || (w->lead < 16 && ((w->sr >> 48) & m) == ((uint64_t)(uint16_t)(a - 1) & m));
    }
    if (w->had) {
        /* words skipped; fewer than none: words again (a repeat) */
        int32_t gap = (int32_t)(int16_t)(uint16_t)(a - w->want);
        int64_t miss = 16 * (int64_t)gap - ((w->nbits - 32) - w->at);
        if (gap >= 0) w->lost += gap; else w->again -= gap;
        if (miss) { w->breaks++; w->gap_bits += miss; }
    }
    w->had = w->locked = 1; w->ph = 0; w->want = (uint16_t)(c + 1); w->at = w->nbits + 16; w->words += 3;
}

#endif
