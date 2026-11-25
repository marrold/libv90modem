/* The answering modem's phase table (modem/answer.c): every phase of
 * ans_phase, 0 to ANS_NPHASES - 1, has its scheme and the scheme its tick.
 * The table's size check in answer.c catches a phase added last without a
 * row; this catches one added anywhere else (a designated initialiser
 * leaves that row NULL). The table is read through ans_enter, which sets
 * answer_t.s from it. */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../modem/ans_scheme.h"   /* ans_enter: the phase table's row into answer_t.s */

static answer_t a;                  /* (large: not on the stack) */

int main(void)
{
    int p, filled = 0;
    printf("test_ans_table: every answering phase has a scheme row\n");
    for (p = 0; p < ANS_NPHASES; p++) {
        memset(&a, 0, sizeof a);
        ans_enter(&a, (ans_phase)p);
        CHECK_EQ_I(a.phase, p);
        if (a.s == NULL || a.s->tick == NULL) printf("  phase %d: no scheme row\n", p);
        CHECK(a.s != NULL);
        CHECK(a.s != NULL && a.s->tick != NULL);
        filled += a.s != NULL && a.s->tick != NULL;
    }
    printf("  %d phases, %d with a scheme and its tick\n", (int)ANS_NPHASES, filled);
    T_REPORT("test_ans_table");
}
