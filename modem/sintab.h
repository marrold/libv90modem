/* Q15 sine table, 1024 entries over one full period. Generated offline;
 * embedded as const data so the signal path stays integer-only. One copy,
 * in sintab.c: sintab[i] = round(32767 sin(2 pi i / 1024)). */
#ifndef V90D_SINTAB_H
#define V90D_SINTAB_H
#include <stdint.h>
#define SIN_N 1024
#define SIN_MASK 1023
extern const int16_t sintab[SIN_N];
#endif
