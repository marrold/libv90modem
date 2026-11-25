/* The 32-bit accumulator bound of the interpolators (audit F21): a branch of
 * a bank laid on 64 int16 samples sums 64 products, each at most
 * 32768 |h[i]| in magnitude, so every partial sum is bounded by 32768 times
 * the row's sum of |h|. Where that is below 2^31 for every row of a bank, an
 * int32 accumulator cannot overflow on any input, and the sum does not
 * depend on the order of the products (modem/interp2.h, interp2_32).
 *
 * v34_interp: qamrx's interpolator, the one user of interp2_32. pcmtx_interp
 * (pcmtx's own bank) is printed for the record: pcmtx and v90arx accumulate
 * in 64 bits (interp2_64), which needs no bound. */
#include <stdint.h>
#include "test.h"
#include "v34_tabs.h"
#include "pcmtx_tab.h"

typedef char same_taps[PCMTX_IN == V34_IN ? 1 : -1];

/* the largest row sum of |h| over the n rows of a bank, and its row */
static int32_t max_row_abs(const int16_t (*h)[V34_IN], int n, int *row)
{
    int32_t best = -1;
    int r, i;
    for (r = 0; r < n; r++) {
        int32_t s = 0;
        for (i = 0; i < V34_IN; i++) {
            int32_t v = h[r][i];
            s += v < 0 ? -v : v;
        }
        if (s > best) { best = s; *row = r; }
    }
    return best;
}

int main(void)
{
    int row = -1;
    int32_t s;
    int64_t worst;

    s = max_row_abs(v34_interp, V34_IP + 1, &row);
    worst = (int64_t)s * 32768;
    printf("v34_interp: largest row sum |h| %ld (row %d), x 32768 = %lld, 2^31 = %lld\n",
           (long)s, row, (long long)worst, (long long)1 << 31);
    CHECK_EQ_I(s, 46728);
    CHECK(worst < ((int64_t)1 << 31));

    s = max_row_abs(pcmtx_interp, PCMTX_IP + 1, &row);
    worst = (int64_t)s * 32768;
    printf("pcmtx_interp (int64 accumulators, for the record): largest row sum |h| %ld (row %d), x 32768 = %lld\n",
           (long)s, row, (long long)worst);
    CHECK(s > 0);

    T_REPORT("test_interp_bound");
}
