/* What a real V.92 modem sent (Conexant CX93001), against the data mode
 * decoder: test/data/cx93001_b1u.txt holds the decoder's input from the first
 * symbol after E2u, at start-up and after a rate renegotiation of one
 * recorded call (see ../v92/README.md, readings 57 and 59). A missing file
 * is a failure (TEST_DATA_DIR, test.h). */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../v92/v92_dec.h"

static long nbits, nbad, nmax;
static void put(void *u, int bit) { (void)u; if (nbits < nmax) { nbits++; if (!bit) nbad++; } }

static int ints(char *p, int *v, int max)
{
    int n = 0, used;
    while (n < max && sscanf(p, "%d%n", &v[n], &used) == 1) { p += used; n++; }
    return n;
}

int main(void)
{
    static char line[16384];
    static int v[1024], D[1024];
    static v92_map_t map;
    static v92_dec_t dec;
    static const char path[] = TEST_DATA_DIR "/cx93001_b1u.txt";
    FILE *f = fopen(path, "r");
    int starts = 0, n = 0, i, k;

    if (!f) { printf("v92real: %s not found\n", path); CHECK(f != NULL); T_REPORT("v92real"); }
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "start ", 6)) {
            memset(&map, 0, sizeof map);
            (void)ints(line + 6, v, 3);
            map.K = (uint8_t)v[0]; map.trellis = (uint8_t)v[1];
        } else if (line[0] == 'M') {
            (void)ints(line + 1, v, V92_DFRAME);
            for (i = 0; i < V92_DFRAME; i++) map.M[i] = (uint8_t)v[i];
        } else if (line[0] == 'P') {
            k = ints(line + 1, v, V92_MAX_LC);
            map.lc[0] = (uint8_t)k;
            for (i = 0; i < k; i++) map.pt[0][i] = (uint16_t)v[i];
        } else if (line[0] == 'D') {
            int zero = 0, mode;
            n = ints(line + 1, D, 1024);
            CHECK(n >= 12 + 48 * 12);
            CHECK_EQ_I(v92_map_check(&map), 0);
            /* the first data frame: every Ki = 0, bar a symbol or two (reading 57) */
            for (i = 0; i < 12; i++) {
                int a = D[i] < 0 ? -D[i] : D[i], j;
                for (j = 0; j + 1 < map.lc[0] && 2 * a > map.pt[0][j] + map.pt[0][j + 1]; j++) ;
                if (v92_eta_to_k(&map, i, D[i] < 0 ? -j - 1 : j) == 0) zero++;
            }
            CHECK(zero >= 10);
            /* then B1u: 48 data frames of ones. With the encoder taken to
             * start from state zero the one after the renegotiation is not
             * received (reading 59); with no such assumption both are */
            for (mode = 0; mode < 2; mode++) {
                CHECK_EQ_I(v92_dec_init(&dec, &map, put, NULL), 0);
                if (mode) v92_dec_any_start(&dec);
                nbits = nbad = 0; nmax = 48L * map.K;
                for (i = 12; i < n; i++) v92_dec_put(&dec, D[i], NULL);
                printf("  start %d, %s: %ld bits of B1u, %ld wrong\n", starts, mode ? "any first state" : "first state zero", nbits, nbad);
                CHECK(nbits > 40L * map.K);
                if (mode) CHECK_EQ_I((int)nbad, 0);
                else if (starts == 0) CHECK_EQ_I((int)nbad, 0);
                else CHECK(nbad > 8);
            }
            starts++;
        }
    }
    fclose(f);
    CHECK_EQ_I(starts, 2);
    T_REPORT("v92real");
}
