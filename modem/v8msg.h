/* V.8 menu messages (CM / JM / CJ): information-category coding (V.8 clause 5
 * and 6) and the async bit framing of a sequence.
 *
 * Octet convention: bit b0 is transmitted first and is the LSB of the byte, so
 * a V.8 octet is an ordinary LSB-first async character. The 10 "sync" bits of a
 * CM/JM sequence (0000001111) are then just the framed character 0xE0.
 */
#ifndef V90D_MODEM_V8MSG_H
#define V90D_MODEM_V8MSG_H
#include <stdint.h>

#define V8_SYNC_CM_JM 0xE0   /* 0 00000111 1 on the wire */
#define V8_SYNC_CI    0x00

/* call function = option bits b5..b7 of callf0, b5 as LSB */
#define V8_CALLF_DATA 6      /* b5=0 b6=1 b7=1 */

/* modulation modes: bit n = Table 4/V.8 item n */
#define V8_MOD_V34      (1u << 1)
#define V8_MOD_V34HD    (1u << 2)
#define V8_MOD_V32      (1u << 3)
#define V8_MOD_V22      (1u << 4)
#define V8_MOD_V17      (1u << 5)
#define V8_MOD_V29      (1u << 6)
#define V8_MOD_V27TER   (1u << 7)
#define V8_MOD_V26TER   (1u << 8)
#define V8_MOD_V26BIS   (1u << 9)
#define V8_MOD_V23      (1u << 10)
#define V8_MOD_V23HD    (1u << 11)
#define V8_MOD_V21      (1u << 12)

/* PCM modem availability (pcm0 option bits b5..b7) */
#define V8_PCM_ANALOG   1    /* V.90/V.92 analogue modem */
#define V8_PCM_DIGITAL  2    /* V.90/V.92 digital modem */
#define V8_PCM_V91      4
/* local menu only, never sent: V.34 is offered solely as the carrier of PCM
 * operation (no V.34 data mode); JM drops V.34 when PCM is not selected */
#define V8_PCM_NO_V34_DATA 0x80

#define V8_PROT_LAPM    1    /* prot0 option bits 1 0 0 */

/* PSTN access (access0 option bits b5..b7) */
#define V8_ACC_CALL_CELL 1
#define V8_ACC_ANS_CELL  2
#define V8_ACC_DIGITAL   4

#define V8_MAX_OCTETS 16

typedef struct {
    uint8_t  call_function;   /* V8_CALLF_* */
    uint16_t modes;           /* V8_MOD_* */
    uint8_t  n_mod_octets;    /* 0..3 modulation octets present */
    uint8_t  has_pcm, pcm;
    uint8_t  has_prot, prot;
    uint8_t  has_access, access;
} v8_menu_t;

/* Encode a menu into information octets (no sync). Returns octet count. */
int v8_encode(const v8_menu_t *m, uint8_t *oct, int max);

/* Decode information octets. Unknown categories/extensions are ignored
 * (V.8 clause 6). Returns 0 if ok, -1 if the first category is not callf. */
int v8_decode(const uint8_t *oct, int n, v8_menu_t *m);

/* Build one full sequence as a bit array: 10 ONEs, framed sync, framed octets.
 * Returns bit count. */
int v8_frame(uint8_t sync, const uint8_t *oct, int n, uint8_t *bits, int max);

/* Answer side: derive JM from the received CM and our local capabilities
 * (local->modes, local->pcm, local->prot, local->access). */
void v8_build_jm(const v8_menu_t *cm, const v8_menu_t *local, v8_menu_t *jm);

/* Modulation selected by a JM (V.8 7.4): V8_SEL_V90 if PCM operation applies,
 * else the Table 4 item number (1..12) of the lowest common mode, 0 if none. */
#define V8_SEL_V90 100
#define V8_SEL_V34 1            /* Table 4 item 1: V.34 */
int v8_selected(const v8_menu_t *jm);
/* The same, read by the calling (analogue) modem: PCM operation only when
 * the JM names a digital modem (V8_PCM_DIGITAL), since V.90 / V.92 analogue
 * operation needs one at the far end; a JM naming "PCM analogue" alone (an
 * analogue V.90 / V.92 modem answering) leaves V.34 or whatever else is
 * common. */
int v8_selected_caller(const v8_menu_t *jm);

#endif
