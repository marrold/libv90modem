/* The frame-sync hunter of the V.34-family framed sequences (audit Task 23c).
 *
 * MP, MP', Jd, CPd, SUVd ... all begin with 17 ones of frame sync and a
 * start bit (0).  seqhunt_bit takes the descrambled bits one at a time and
 * returns 1 on the start bit that ends a run of at least 17 ones, else 0.
 * A one only counts (h->ones, which callers read for their E = 20 ones
 * test); a zero ends the run, sync or not.  A caller that restarts the
 * hunt sets h->ones = 0 itself.
 *
 * The brief's type also had a buffer word and a bit count; no caller needs
 * them (each keeps its own byte buffer for the sequence), and a struct that
 * grows would move v90m_mem_size(), so only the run length is here. */
#ifndef V90M_SEQHUNT_H
#define V90M_SEQHUNT_H

typedef struct { int ones; } seqhunt_t;

static inline int seqhunt_bit(seqhunt_t *h, int bit)
{
    int sync;
    if (bit) { h->ones++; return 0; }
    sync = h->ones >= 17;
    h->ones = 0;
    return sync;
}

#endif
