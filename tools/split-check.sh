#!/bin/sh
# The programs the ARM runner splits by case (SPLIT_TESTS in the Makefile):
# `prog --cases` prints how many cases it has, `prog N` runs case N alone.
# For each, the outputs of `prog 1` .. `prog N`, one after another, must be
# what `prog` prints without an argument: both filtered (tools/out-filter.sh,
# no host timing) and without the report lines "name: C checks, F failures",
# which each case prints for itself. Every case must pass.
#
#   tools/split-check.sh [BUILD]    (default build; host programs, built here)
#
# Prints "split: N programs, cases concatenate to the whole", or what
# differs. The runs (every case and the whole program) go in parallel, nproc
# at a time; outputs in BUILD/split/.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/out-filter.sh"
B=${1:-build}
case $B in "$ROOT"/*) MB=${B#"$ROOT"/} ;; *) MB=$B ;; esac
case $B in /*) ;; *) B=$ROOT/$B ;; esac
PROGS=$(sed -n 's/^SPLIT_TESTS *:= *//p' "$ROOT/Makefile")
[ -n "$PROGS" ] || { echo "split: no SPLIT_TESTS in the Makefile"; exit 1; }
REPORT='^[A-Za-z0-9_/]+: [0-9]+ checks, [0-9]+ failures$'
make -s -C "$ROOT" -j"$(nproc 2>/dev/null || echo 4)" BUILD="$MB" $(for p in $PROGS; do echo "$MB/$p"; done) || { echo "split: build failed"; exit 1; }
D=$B/split
rm -rf "$D"; mkdir -p "$D" || exit 1

# the case counts; then every run as "PROG ARG" (ARG - for the whole program)
jobs=
for p in $PROGS; do
    n=$("$B/$p" --cases 2>&1 | head -1)
    case $n in ''|*[!0-9]*) echo "split: $p --cases printed no case count"; exit 1 ;; esac
    [ "$n" -ge 1 ] || { echo "split: $p has no cases"; exit 1; }
    echo "$n" > "$D/$p.cases"
    jobs="$jobs $p -"
    i=1; while [ $i -le "$n" ]; do jobs="$jobs $p $i"; i=$((i + 1)); done
done
# shellcheck disable=SC2086
printf '%s %s\n' $jobs | SPLIT_B=$B SPLIT_D=$D xargs -P "$(nproc 2>/dev/null || echo 4)" -L 1 sh -c '
    if [ "$2" = - ]; then "$SPLIT_B/$1" > "$SPLIT_D/$1.whole" 2>&1; else "$SPLIT_B/$1" "$2" > "$SPLIT_D/$1.$2" 2>&1; fi
    echo $? > "$SPLIT_D/$1.$2.rc"' sh

fail=0; np=0
for p in $PROGS; do
    n=$(cat "$D/$p.cases")
    bad=
    [ "$(cat "$D/$p.-.rc")" = 0 ] || bad="$bad whole"
    i=1; : > "$D/$p.cat"
    while [ $i -le "$n" ]; do
        [ "$(cat "$D/$p.$i.rc")" = 0 ] || bad="$bad $i"
        out_filter "$D/$p.$i" | { grep -vE "$REPORT"; [ $? -le 1 ]; } >> "$D/$p.cat"
        i=$((i + 1))
    done
    out_filter "$D/$p.whole" | { grep -vE "$REPORT"; [ $? -le 1 ]; } > "$D/$p.ref"
    if [ -n "$bad" ]; then echo "split: $p FAILED (runs:$bad; outputs in $D)"; fail=1; fi
    if ! cmp -s "$D/$p.ref" "$D/$p.cat"; then
        echo "split: $p: cases 1..$n DIFFER from the whole (- whole, + cases; first 10 lines):"
        diff "$D/$p.ref" "$D/$p.cat" | grep '^[<>]' | head -10 | sed 's/^</-/; s/^>/+/'
        fail=1
    fi
    np=$((np + 1))
done
[ $fail -eq 0 ] || { echo "split: FAILED"; exit 1; }
echo "split: $np programs, cases concatenate to the whole"
