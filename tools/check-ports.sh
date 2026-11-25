#!/bin/sh
# The whole test suite (what `make check-arm` runs: every test, the API
# test, the subdirectories' tests) built for each target and run there
# (qemu-user, wine, or the host itself); then each test program's output
# compared with the reference target's (the first, host gcc), program by
# program. The signal path is fixed point, so every target must print the
# same thing; only lines that time the host (wall clock, CPU) are left out
# of the comparison, and those that print the size of structures in memory
# (TIMING, tools/out-filter.sh). One allowance: a C library whose printf
# rounds numbers up where the correct rounding is down (uClibc-ng; found by
# tools/printf-probe.c on the target itself) may print such a number one
# unit larger in its last digit (tools/ports-cmp.awk); those lines are
# counted and reported, and nothing else may differ.
#
#   tools/check-ports.sh [OUT [target ...]]
#
# OUT (default build/ports) gets, per target, its build tree, its log
# (OUT/<target>.log) and each program's output (OUT/<target>/out/). It may
# not be under /tmp; TMPDIR is set to OUT/tmp, so the compilers' temporary
# files go there too.
#   PAR=4      targets built and run at once
#   JOBS=2     make -j for each target (so at most PAR * JOBS compilers)
#   PROG_PAR=8 test programs each target runs at once (tools/par-run.sh's
#              PAR; so at most PAR * PROG_PAR programs)
#   QUICK="t1 t2 ..."  targets that skip the slow programs listed in
#              SLOW (default tools/ports-slow.txt; the comparison then
#              covers the rest and says how many were skipped)
#   SKIP="arm_test_orig_seeds ..."  programs no target but the reference
#              runs (as a quick target's are skipped and counted)
#   COMPARE_ONLY=1  compare what an earlier run left in OUT, run nothing
#   TOOLCHAINS (default ../toolchains) holds the cross compilers (Bootlin
#              "stable-2023.08-1", musl where there is one); a target whose
#              compiler or runner is missing is reported and skipped.
#
# The reference target always runs (whole), even when not named.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC=${TOOLCHAINS:-$ROOT/../toolchains}
PAR=${PAR:-4}
JOBS=${JOBS:-2}
QUICK=${QUICK:-}
SLOW=${SLOW:-$ROOT/tools/ports-slow.txt}
SUBS=$(sed -n 's/^SUBS *:= *//p' "$ROOT/Makefile")
# lines that measure the host, not the modem (TIMING, out_filter)
. "$ROOT/tools/out-filter.sh"

tc() { set -- "$TC"/$1/bin/$2; [ -x "$1" ] && echo "$1" || echo "$TC/missing/$2"; }

# name, compiler, runner (qemu options joined by ':'; - for none)
TARGETS="
host-gcc       gcc                                                     -
host-clang     clang                                                   -
x86_64-musl    $(tc 'x86-64--musl--*' x86_64-linux-gcc)                qemu-x86_64-static
i686-musl      $(tc 'x86-i686--musl--*' i686-linux-gcc)                qemu-i386-static
armv5-uclibc   $(tc 'armv5-eabi--uclibc--*' arm-linux-gcc)             qemu-arm-static
armv7hf-musl   $(tc 'armv7-eabihf--musl--*' arm-linux-gcc)             qemu-arm-static
aarch64-musl   $(tc 'aarch64--musl--*' aarch64-linux-gcc)              qemu-aarch64-static
aarch64be-musl $(tc 'aarch64be--musl--*' aarch64_be-linux-gcc)         qemu-aarch64_be-static
mips-musl      $(tc 'mips32--musl--*' mips-linux-gcc)                  qemu-mips-static
mipsel-musl    $(tc 'mips32el--musl--*' mipsel-linux-gcc)              qemu-mipsel-static
ppc-musl       $(tc 'powerpc-e500mc--musl--*' powerpc-linux-gcc)       qemu-ppc-static:-cpu:e500mc
ppc64-musl     $(tc 'powerpc64-power8--musl--*' powerpc64-linux-gcc)   qemu-ppc64-static:-cpu:power8
riscv64-musl   $(tc 'riscv64-lp64d--musl--*' riscv64-linux-gcc)        qemu-riscv64-static
s390x-glibc    $(tc 's390x-z13--glibc--*' s390x-linux-gcc)             qemu-s390x-static
m68k-uclibc    $(tc 'm68k-68xxx--uclibc--*' m68k-linux-gcc)            qemu-m68k-static:-cpu:m68040
win32-mingw    i686-w64-mingw32-gcc                                    $ROOT/tools/winerun
"
REF=$(echo "$TARGETS" | awk 'NF {print $1; exit}')

# --one NAME: build and run one target (what the parallel step below runs)
if [ "${1:-}" = --one ]; then
    OUT=$2; name=$3
    # the programs one target runs at once (par-run's PAR, not the targets')
    PAR=${PROG_PAR:-8}; export PAR
    set -- $(echo "$TARGETS" | awk -v n="$name" '$1 == n')
    cc=$2; run=$3; B=$OUT/$name
    skip=-
    case " $QUICK " in *" $name "*) [ "$name" != "$REF" ] && skip=$SLOW ;; esac
    if [ -n "${SKIP:-}" ] && [ "$name" != "$REF" ]; then
        mkdir -p "$B"
        { [ "$skip" = - ] || cat "$skip"; printf '%s\n' $SKIP; } > "$B/skip.txt"
        skip=$B/skip.txt
    fi
    if ! command -v "$cc" >/dev/null 2>&1; then echo "skip: no compiler $cc" > "$OUT/$name.status"; exit 0; fi
    r=${run%%:*}
    if [ "$run" != - ] && ! command -v "$r" >/dev/null 2>&1; then echo "skip: no runner $r" > "$OUT/$name.status"; exit 0; fi
    # -DTEST_FULL: the whole tests on 32-bit ARM too (test/test.h);
    # -DTEST_EMULATED=1 under qemu: no wall-clock limit (test/test.h). On
    # 32-bit x86 the test harness's doubles (the simulated line, sim/) are
    # computed in SSE2, not in the x87's 80 bits, which round differently
    # and so feed the modem a different line; the modem itself is integer
    # code that these flags do not change (see docs/history/PROGRESS.md, "ports-2").
    # On m68k the frame pointer is kept: GCC 12.3 for m68k, with it omitted
    # (-O1 and up), reads an incoming argument 8 bytes off around a 64-bit
    # division (a DImode push from an sp-relative source after a subq). The
    # two sites in the tree are written so that it compiles them right
    # anyway; the flag guards against new ones (docs/history/PROGRESS.md, "m68k")
    extra="-DTEST_FULL"
    case "$cc" in *mingw*) extra="$extra -D__USE_MINGW_ANSI_STDIO=1" ;; esac
    case "$name" in i686-*|win32-*) extra="$extra -msse2 -mfpmath=sse" ;; esac
    case "$name" in m68k-*) extra="$extra -fno-omit-frame-pointer" ;; esac
    case "$run" in qemu-*) extra="$extra -DTEST_EMULATED=1" ;; esac
    rm -rf "$B/out"; mkdir -p "$B/out"
    Q="$ROOT/tools/portrun $B $B/out $skip $run"
    set -- -j"$JOBS" BUILD="$B" ARM_CC="$cc" QEMU="$Q" CFLAGS="-std=c99 -O2 -Wall -Wextra -Werror $extra"
    st=0
    {
        echo "== $name: $cc, runner $run, skip list $skip"
        # how its printf rounds (tools/printf-probe.c): kept for the comparison
        $cc -std=c99 -O2 $extra $(case "$cc" in *mingw*) ;; *) echo -static ;; esac) -o "$B/printf-probe" "$ROOT/tools/printf-probe.c" \
            && $([ "$run" = - ] || echo "$run" | tr ':' ' ') "$B/printf-probe" | tr -d '\r' > "$B/printf.txt" || st=1
        echo "printf: $(cat "$B/printf.txt")"
        make -C "$ROOT" "$@" test-arm || st=1
        make -C "$ROOT" "$@" test-api-arm || st=1
        for d in $SUBS; do
            echo "== $d"
            make -C "$ROOT/$d" "$@" BUILD="$B/$d" test-arm || st=1
        done
    } > "$OUT/$name.log" 2>&1
    echo "$st" > "$OUT/$name.status"
    exit 0
fi

OUT=${1:-$ROOT/build/ports}; [ $# -gt 0 ] && shift
mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd -P) || exit 1
case "$OUT/" in /tmp/*|/var/tmp/*) echo "check-ports: OUT ($OUT) may not be under /tmp" >&2; exit 2 ;; esac
TMPDIR=$OUT/tmp; export TMPDIR; mkdir -p "$TMPDIR"

all=$(echo "$TARGETS" | awk 'NF {print $1}')
want=$(echo ${*:-$all})
case " $want " in *" $REF "*) ;; *) want="$REF $want" ;; esac
for name in $want; do
    echo "$all" | grep -qx "$name" || { echo "check-ports: no target $name" >&2; exit 2; }
    [ -n "${COMPARE_ONLY:-}" ] || rm -f "$OUT/$name.status"
done
if [ -z "${COMPARE_ONLY:-}" ]; then
    echo "check-ports: $want -> $OUT (PAR=$PAR, JOBS=$JOBS${QUICK:+, quick: $QUICK})"
    printf '%s\n' $want | xargs -P "$PAR" -I{} "$0" --one "$OUT" {}
fi

# one program's output as compared: no carriage returns (wine), no timing
norm() { out_filter "$1"; }

fail=0
rst=$(cat "$OUT/$REF.status" 2>/dev/null || echo missing)
[ "$rst" = 0 ] || { echo "$REF (reference): FAILED ($rst; $OUT/$REF.log)"; fail=1; }
refs=$(cd "$OUT/$REF/out" 2>/dev/null && find . -name '*.txt' | sed 's|^\./||; s|\.txt$||' | sort)
nref=$(echo "$refs" | grep -c .)
for name in $want; do
    st=$(cat "$OUT/$name.status" 2>/dev/null || echo missing)
    case "$st" in skip*) echo "$name: SKIPPED (${st#skip: })"; continue ;; esac
    [ "$name" = "$REF" ] && { echo "$name: the reference, $nref programs, exit $st"; continue; }
    same=0; diff=""; skipped=0; missing=""; bad=""; rounded=0; nround=0
    pf=$(cat "$OUT/$name/printf.txt" 2>/dev/null)
    [ "$pf" = "$(cat "$OUT/$REF/printf.txt")" ] || [ -z "$pf" ] || rounded=1
    for k in $refs; do
        f=$OUT/$name/out/$k.txt
        if [ ! -f "$f" ]; then
            if grep -q "^portrun: $k skipped" "$OUT/$name.log"; then skipped=$((skipped + 1)); else missing="$missing $k"; fi
            continue
        fi
        [ "$(cat "$OUT/$name/out/$k.status")" = 0 ] || bad="$bad $k"
        norm "$OUT/$REF/out/$k.txt" > "$f.ref"; norm "$f" > "$f.got"
        if cmp -s "$f.ref" "$f.got"; then same=$((same + 1)); rm -f "$f.ref" "$f.got"
        elif [ $rounded = 1 ] && r=$(awk -f "$ROOT/tools/ports-cmp.awk" "$f.ref" "$f.got"); then
            same=$((same + 1)); nround=$((nround + r)); echo "$r" > "$f.rounded"
        else diff="$diff $k"; fi
    done
    line="$name: exit $st, $same of $nref programs identical to $REF"
    [ $rounded = 1 ] && line="$line ($nround lines only with printf's rounding up, \"$pf\")"
    [ $skipped -gt 0 ] && line="$line, $skipped skipped (quick)"
    [ -n "$bad" ] && line="$line; FAILED:$bad"
    [ -n "$diff" ] && line="$line; DIFFER:$diff"
    [ -n "$missing" ] && line="$line; NOT RUN:$missing"
    echo "$line"
    { [ "$st" = 0 ] && [ -z "$diff$missing$bad" ]; } || fail=1
done
[ $fail -eq 0 ] && echo "ALL PORTS PASS" || { echo "PORTS FAILED (logs: $OUT/<target>.log; for a program p that differs: diff $OUT/<target>/out/<p>.txt.ref $OUT/<target>/out/<p>.txt.got)"; exit 1; }
