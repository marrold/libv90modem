/* x2's downstream mapper (docs/history/x2-facts.md, "Mapping", "Free
 * sign"): MD + B bits, already scrambled (the caller runs GPC, v90/v90_scr),
 * into one frame of 6 mu-law octets, and back.
 *
 * The first MD bits are signs; the next B, LSB first, a number x. Position
 * i (0..5) takes digit x mod n_i, then x /= n_i, and the digit selects a
 * level from the position's list (largest magnitude first). The MD signs go,
 * in position order, to the MD positions of the smallest levels (of equal
 * levels, the first position), each sent as a change: last ^= bit, the
 * octet positive when last is 1. The remaining position carries the free
 * sign: in levels A negative when the other five sum to more than 805 in
 * D-Modem's 14-bit units; in the data levels negative when the running sum
 * of all values sent (this frame's five included) is above zero.
 *
 * The data levels are the bank of the rate index and pad mode
 * (x2tab.h: rate index 1..15, B = 19 to 37, pad mode 0..2; each position's
 * own list), levels A index 1's (B = 19, 9 levels in each position). MD is
 * 5. Bits are one per byte (0/1), element 0 first. */
#ifndef V90D_MODEM_X2MAP_H
#define V90D_MODEM_X2MAP_H
#include <stdint.h>

#define X2MAP_N 6           /* octets per frame */

typedef struct {
    int     index;          /* rate index, 1..15 (levels A: 1) */
    int     pad_mode;       /* 0 none, 1 3 dB, 2 6 dB: the data levels only */
    int     levels_a;       /* levels A (sync, records) instead of the data
                               levels; the owner may change it between
                               frames, last and rds carry on */
    int     last;           /* the differential sign: 1 positive */
    int64_t rds;            /* the running sum of the linear values sent */
    /* the bank of index, pad_mode and levels_a as they were when last
     * seen (x2map_init, x2map_frame): its bits per frame (0: no bank), and
     * the lists of position 0 and of positions 1..5. x2map_frame takes
     * them again when the owner has changed one of the three. */
    int     c_index, c_pad_mode, c_levels_a, c_bits;
    int     c_n[2];
    const uint8_t *c_recv[2], *c_sent[2];
} x2map_t;

/* last positive, the running sum zero: the first sync frame, and again
 * after a rate change's scrambler reset. Every x2map_t must be given this
 * before any other call: it sets the bank cache (c_*) that x2map_frame and
 * x2map_bits rely on; after it the owner may write index, pad_mode and
 * levels_a directly */
void x2map_init(x2map_t *m, int index, int pad_mode, int levels_a);

/* bits per frame, B + MD (24 at index 1, 42 at 15); 0 for a bank that
 * does not exist (an index outside 1..15 or a pad mode outside 0..2; levels
 * A at any index but 1) */
int x2map_bits(const x2map_t *m);

/* one frame from x2map_bits(m) scrambled bits; six 0xff (zero) for a bank
 * that does not exist */
void x2map_frame(x2map_t *m, const uint8_t *bits, uint8_t out[X2MAP_N]);

/* the lists of position pos (0..5) in m's levels: n levels, the largest
 * first, as the client receives them (*recv) and the octets sent for them
 * across the pad (*sent; levels A: the same). Returns n, or 0 for a bank
 * that does not exist (the pointers are not set). */
int x2map_list(const x2map_t *m, int pos, const uint8_t **recv, const uint8_t **sent);

/* the inverse (for the tests and the simulated client): the bits of one
 * frame from the octets as the client receives them (behind the pad of
 * pad_mode, in the data levels: the levels of the bank's lists; two levels
 * may be sent as one octet, so the octets sent are not taken). Returns the
 * number of bits, or -1 when an octet is not a level of its position, or
 * the digits are past B bits. The free sign is not checked. Uses last
 * only; keep a separate x2map_t. */
int x2map_unframe(x2map_t *m, const uint8_t in[X2MAP_N], uint8_t *bits);

#endif
