/* Minimal C test harness: no external deps. Count checks, report, exit nonzero
 * on any failure. Portable C99; builds on x86 and the ARM target alike. */
#ifndef V90D_TEST_H
#define V90D_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* TEST_SHORT: some long tests run a shorter version on 32-bit ARM, the box's
 * architecture, where `make check-arm` runs them under qemu. The port matrix
 * (tools/check-ports.sh) builds every target with -DTEST_FULL, so that all
 * of them run the same, whole tests and must print the same.
 * -DTEST_FORCE_SHORT gives the same short set on any target (a host build
 * to compare the ARM outputs with). */
#if !defined(TEST_SHORT) && ((defined(__arm__) && !defined(TEST_FULL)) || defined(TEST_FORCE_SHORT))
#define TEST_SHORT 1
#endif
/* TEST_DATA_DIR: where the tests' data files are (test/data); the Makefile
 * gives the absolute path, so that a test finds them from any directory */
#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "test/data"
#endif
/* TEST_EMULATED=1: the program runs under an emulator (tools/check-ports.sh
 * says so for its qemu targets), where wall-clock time measures the
 * emulator; a check on it still counts, but cannot fail */
#ifndef TEST_EMULATED
#define TEST_EMULATED 0
#endif

static int t_checks = 0, t_fails = 0;

#define CHECK(cond) do { \
    t_checks++; \
    if (!(cond)) { t_fails++; \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ_I(got, want) do { \
    long long _g = (long long)(got), _w = (long long)(want); \
    t_checks++; \
    if (_g != _w) { t_fails++; \
        printf("  FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #got, _g, _w); } \
} while (0)

/* near: |got-want| <= tol */
#define CHECK_NEAR(got, want, tol) do { \
    double _g = (double)(got), _w = (double)(want), _t = (double)(tol); \
    t_checks++; \
    if (fabs(_g - _w) > _t) { t_fails++; \
        printf("  FAIL %s:%d: %s = %g, want %g +/- %g\n", __FILE__, __LINE__, #got, _g, _w, _t); } \
} while (0)

/* A program split into cases (the Makefile's SPLIT_TESTS): `prog --cases`
 * prints the count and exits, `prog N` runs case N alone, no argument runs
 * them all. t_case_arg returns the case to run alone, 0 for all of them;
 * T_CASE(only, k) says whether case k runs. The cases run one after another
 * print what the whole program prints (tools/split-check.sh), less the
 * T_REPORT line, which each prints for itself. */
static inline int t_case_arg(int argc, char **argv, int ncases)
{
    long n;
    char *end;
    if (argc < 2) return 0;
    if (argc == 2 && strcmp(argv[1], "--cases") == 0) {
        printf("%d\n", ncases);
        exit(0);
    }
    n = strtol(argv[1], &end, 10);
    if (argc > 2 || *end != 0 || n < 1 || n > ncases) {
        printf("usage: %s [--cases | 1..%d]\n", argv[0], ncases);
        exit(2);
    }
    return (int)n;
}
#define T_CASE(only, k) ((only) == 0 || (only) == (k))

#define T_REPORT(name) do { \
    printf("%s: %d checks, %d failures\n", (name), t_checks, t_fails); \
    return t_fails ? 1 : 0; \
} while (0)

#endif
