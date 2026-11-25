#!/bin/sh
# A target's short suite against the host's: what `make check-arm` printed
# on the target (TARGETBUILD/arm_log, TEST_SHORT on 32-bit ARM) compared,
# program by program, with what the host printed built with
# -DTEST_FORCE_SHORT (HOSTBUILD/log), the same short set (test/test.h). The
# signal path is fixed point, so both must print the same; the outputs are
# filtered first (tools/out-filter.sh: no host timing). A program that the
# target ran case by case (SPLIT_TESTS, arm_test_p34:N) is compared as its
# cases one after another, without their report lines (tools/split-check.sh),
# with the host's whole program, without its own. Every run must have
# passed. A host program the target did not run fails, unless SKIPFILE
# lists it (the ARM_SKIP of that check-arm run, as tools/ports-slow.txt
# lists programs: arm_test_x, v32/arm_test_y); those are counted, not
# compared. The API test and the examples are not in the pool.
#
#   tools/short-cmp.sh HOSTBUILD TARGETBUILD [SKIPFILE]
#
# When HOSTBUILD has no log of a host run yet (CI), the host short set is
# built and run here first: make test-all BUILD=HOSTBUILD
# EXTRA_CPPFLAGS=-DTEST_FORCE_SHORT PAR=nproc. A HOSTBUILD log is otherwise
# taken as it is: it must come from the same commit (make verify runs both).
#
# Prints "short-cmp: N programs identical (TARGETBUILD against HOSTBUILD)", with how
# many the target skipped, or each program that differs (its first lines).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/out-filter.sh"
[ $# -eq 2 ] || [ $# -eq 3 ] || { echo "usage: tools/short-cmp.sh HOSTBUILD TARGETBUILD [SKIPFILE]" >&2; exit 2; }
H=$1; T=$2; SK=${3:-/dev/null}
[ -r "$SK" ] || { echo "short-cmp: cannot read $SK"; exit 2; }
case $H in "$ROOT"/*) MH=${H#"$ROOT"/} ;; *) MH=$H ;; esac
case $H in /*) ;; *) H=$ROOT/$H ;; esac
case $T in /*) ;; *) T=$ROOT/$T ;; esac
REPORT='^[A-Za-z0-9_/]+: [0-9]+ checks, [0-9]+ failures$'
NP=$(nproc 2>/dev/null || echo 4)

if [ ! -f "$H/log/entries.txt" ]; then
    echo "short-cmp: no host short run in $H; running it (make test-all BUILD=$MH EXTRA_CPPFLAGS=-DTEST_FORCE_SHORT)"
    make -s -C "$ROOT" -j"$NP" test-all PAR="$NP" BUILD="$MH" EXTRA_CPPFLAGS=-DTEST_FORCE_SHORT \
        > "$H.short-run.txt" 2>&1 || { tail -5 "$H.short-run.txt"; echo "short-cmp: the host short run FAILED ($H.short-run.txt)"; exit 1; }
fi
[ -f "$T/arm_log/entries.txt" ] || { echo "short-cmp: no target run in $T/arm_log (make check-arm BUILD=...)"; exit 1; }

O=$T/short-cmp
rm -rf "$O"; mkdir -p "$O" || exit 1
# the target's runs as "HOSTKEY CASE TARGETKEY" (CASE 0: the whole program)
awk '{ k = $2; c = 0
       if (match(k, /:[0-9]+$/)) { c = substr(k, RSTART + 1); k = substr(k, 1, RSTART - 1) }
       n = split(k, p, "/"); sub(/^arm_/, "", p[n]); h = p[1]; for (i = 2; i <= n; i++) h = h "/" p[i]
       print h, c, $2 }' "$T/arm_log/entries.txt" | sort -k1,1 -k2,2n > "$O/runs.txt"

same=0; fail=0; nh=0; skipped=0
for hk in $(awk '{ print $2 }' "$H/log/entries.txt"); do
    nh=$((nh + 1))
    if ! grep -q "^$hk " "$O/runs.txt"; then
        # the target's name of it: arm_ before the program's name
        case $hk in */*) ak=${hk%/*}/arm_${hk##*/} ;; *) ak=arm_$hk ;; esac
        if grep -qxF "$ak" "$SK"; then skipped=$((skipped + 1))
        else echo "short-cmp: $hk did not run on the target (not in ${SK#"$ROOT"/})"; fail=1; fi
        continue
    fi
    [ "$(cat "$H/log/$hk.rc" 2>/dev/null)" = 0 ] || { echo "short-cmp: $hk FAILED on the host ($H/log/$hk.log)"; fail=1; continue; }
    f=$O/$hk; mkdir -p "$(dirname "$f")"
    : > "$f.got"; bad=; split=0
    for tk in $(awk -v h="$hk" '$1 == h { print $3 }' "$O/runs.txt"); do
        case $tk in *:*) split=1 ;; esac
        [ "$(cat "$T/arm_log/$tk.rc" 2>/dev/null)" = 0 ] || bad="$bad $tk"
        out_filter "$T/arm_log/$tk.log" >> "$f.got"
    done
    [ -z "$bad" ] || { echo "short-cmp: FAILED on the target:$bad ($T/arm_log)"; fail=1; continue; }
    out_filter "$H/log/$hk.log" > "$f.ref"
    if [ $split = 1 ]; then
        grep -vE "$REPORT" "$f.got" > "$f.got2"; mv "$f.got2" "$f.got"
        grep -vE "$REPORT" "$f.ref" > "$f.ref2"; mv "$f.ref2" "$f.ref"
    fi
    if cmp -s "$f.ref" "$f.got"; then same=$((same + 1)); rm -f "$f.ref" "$f.got"; continue; fi
    echo "short-cmp: $hk DIFFERS (- host short, + target; first 10 lines):"
    diff "$f.ref" "$f.got" | grep '^[<>]' | head -10 | sed 's/^</-/; s/^>/+/'
    fail=1
done
# a target program the host does not have
for hk in $(awk '{ print $1 }' "$O/runs.txt" | sort -u); do
    grep -q " $hk\$" "$H/log/entries.txt" || { echo "short-cmp: $hk ran on the target, not on the host"; fail=1; }
done
line="short-cmp: $same programs identical (${T#"$ROOT"/} against ${H#"$ROOT"/})"
[ $skipped -gt 0 ] && line="$line, $skipped skipped on the target"
echo "$line"
[ $fail -eq 0 ] || { echo "short-cmp: FAILED"; exit 1; }
