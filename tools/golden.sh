#!/bin/sh
# Every test program's output against its golden copy in the repository.
#
#   tools/golden.sh check BUILD    compare; exit 1 on any difference
#   tools/golden.sh record BUILD   write test/golden/out from BUILD's run
#
# The outputs are the logs of the last `make check PAR=N BUILD=BUILD`
# (BUILD/log/<key>.log: tools/par-run.sh's pool, the API test, the
# example), each filtered by tools/out-filter.sh (no host timing). The
# programs are those `make print-golden` lists; program P of directory D
# has its golden copy in test/golden/out/D/P.txt (D is test for the top
# level's test/, api for test/api/, examples, or a subdirectory: v34 ...).
#
# check prints "golden: N programs identical", or for each program that
# differs its name and the first 10 lines of the difference. A program
# whose log is missing, or older than the program, is an error too (run
# make check PAR=N first). It reads no exit status: make check, which
# fails on any failing program, comes first (make golden runs both). The
# environment variables that make tests talk more (V92_VERBOSE, V32_DEBUG)
# must not be set (make golden unsets them). A golden file is re-recorded
# only when a change explains it, and the commit lists each file
# re-recorded and why.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/out-filter.sh"
GOLD=$ROOT/test/golden/out
mode=${1:-}; B=${2:-}
case $mode in check|record) ;; *) echo "usage: tools/golden.sh check|record BUILD" >&2; exit 2 ;; esac
[ -n "$B" ] || { echo "usage: tools/golden.sh check|record BUILD" >&2; exit 2; }
# make's BUILD as the Makefile takes it (relative: the subdirectories then
# build in their own build/ for the default one); B the same, absolute
case $B in "$ROOT"/*) MB=${B#"$ROOT"/} ;; *) MB=$B ;; esac
case $B in /*) ;; *) B=$ROOT/$B ;; esac
if [ "$mode" = check ] && [ ! -d "$GOLD" ]; then echo "golden: no test/golden/out"; exit 1; fi

# the stale-log test below needs test's -nt
# (a test without it fails with status 2 or more; comparing a file with
# itself is false (1) where it has it, in bash and in dash alike)
[ "$ROOT/tools/golden.sh" -nt "$ROOT/tools/golden.sh" ]
[ $? -le 1 ] || { echo "golden: this shell's test has no -nt"; exit 2; }

list=$(make -s --no-print-directory -C "$ROOT" BUILD="$MB" print-golden) || { echo "golden: make print-golden failed"; exit 1; }
OUT=$B/golden
rm -rf "$OUT"; mkdir -p "$OUT" || exit 1

# each program's filtered output to OUT/<name>.txt; a missing or stale log
# is an error
err=0; n=0; names=
while read -r key prog; do
    case $key in */*) name=$key ;; *) name=test/$key ;; esac
    log=$B/log/$key.log
    if [ ! -f "$log" ]; then echo "golden: no output of $name ($log; run make check PAR=N BUILD=$MB)"; err=1; continue; fi
    if [ "$prog" -nt "$log" ]; then echo "golden: output of $name older than the program ($log; run make check PAR=N BUILD=$MB)"; err=1; continue; fi
    mkdir -p "$OUT/${name%/*}"
    out_filter "$log" > "$OUT/$name.txt" || { echo "golden: cannot filter $log"; err=1; continue; }
    n=$((n + 1)); names="$names $name"
done <<EOT
$list
EOT
[ $err -eq 0 ] || { echo "golden: FAILED (outputs missing)"; exit 1; }

if [ "$mode" = record ]; then
    rm -rf "$GOLD" && mkdir -p "$GOLD" && cp -R "$OUT"/. "$GOLD"/ || exit 1
    echo "golden: $n programs recorded in test/golden/out"
    exit 0
fi

fail=0; same=0
for name in $names; do
    g=$GOLD/$name.txt
    if [ ! -f "$g" ]; then echo "golden: $name: no test/golden/out/$name.txt"; fail=1; continue; fi
    if cmp -s "$g" "$OUT/$name.txt"; then same=$((same + 1)); continue; fi
    echo "golden: $name DIFFERS (- test/golden/out/$name.txt, + now; first 10 lines):"
    diff "$g" "$OUT/$name.txt" | grep '^[<>]' | head -10 | sed 's/^</-/; s/^>/+/'
    fail=1
done
# a golden file of a program no longer run
for g in $(cd "$GOLD" && find . -name '*.txt' | sed 's|^\./||; s|\.txt$||' | sort); do
    case " $names " in *" $g "*) ;; *) echo "golden: test/golden/out/$g.txt: no such program"; fail=1 ;; esac
done
if [ $fail -eq 0 ]; then echo "golden: $n programs identical"; exit 0; fi
echo "golden: FAILED ($same of $n programs identical)"
exit 1
