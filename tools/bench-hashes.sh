#!/bin/sh
# The benches' hashes against test/golden/bench.txt: each simulated call
# the Makefile's bench targets record (tools/rec_*) replayed on the host by
# its bench (rig/bench_*), which ends with "tx hash" (everything the modem
# sent) and "rx hash" (the data bits it received); bench_v90a, a receiver
# alone, prints the hash of its decisions ("dec" below). A hash of nothing
# (811c9dc5: no bit received) is an error. Where the far end sends the
# bench's own pseudo-random sequence (all but bell*, which receive text),
# an rx hash pins how many bits arrived and that each was right, not which
# ones they were.
#
#   tools/bench-hashes.sh check    compare; exit 1 on any difference
#   tools/bench-hashes.sh record   write test/golden/bench.txt
#
# BUILD (default build) holds the programs, built here, and the recordings
# and logs (BUILD/bench/<name>.rec, .log). The calls run in parallel.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${BUILD:-build}
case $B in /*) ;; *) B=$ROOT/$B ;; esac
GOLD=$ROOT/test/golden/bench.txt
D=$B/bench

# name, recorder and its arguments (the bench targets' own), bench and its
# options; the recording is the recorder's last argument but for "v92"
JOBS="
v90      rec_v90  -55 0.2 40 0 @    bench_v90
x2       rec_x2   40 0 @            bench_v90 -x
k56      rec_k56  40 0 @            bench_v90 -k
v34      rec_v34  16 @              bench_v34
v90a     rec_v90a 100 48000 @       bench_v90a
v90c     rec_v90c 100 5 @           bench_v90c
v92c     rec_v90c 100 5 @ v92       bench_v90c
bell103  rec_bell 103 3 @           bench_bell
bell212  rec_bell 212 3 @           bench_bell
"

# --one NAME: record and replay one call; its hash lines to D/NAME.hash
if [ "${1:-}" = --one ]; then
    set -- $(echo "$JOBS" | awk -v n="$2" '$1 == n')
    name=$1; shift
    rec= ; bench=
    for a in "$@"; do
        case $a in
        bench_*) bench=$B/$a ;;
        @) rec="$rec $D/$name.rec" ;;
        *) if [ -n "$bench" ]; then bench="$bench $a"; else rec="${rec:+$rec }$a"; fi ;;
        esac
    done
    rec=$B/$rec
    rm -f "$D/$name.hash"
    # shellcheck disable=SC2086
    { $rec && $bench "$D/$name.rec"; } > "$D/$name.log" 2>&1 || { echo "bench-hashes: $name failed ($D/$name.log)"; exit 1; }
    sed -n "s/^\([tr]x\) hash \([0-9a-f]\{8\}\)$/$name \1 \2/p; s/^bench_v90a: .*, hash \([0-9a-f]\{8\}\), .*/$name dec \1/p" \
        "$D/$name.log" > "$D/$name.hash"
    [ -s "$D/$name.hash" ] || { echo "bench-hashes: $name printed no hash ($D/$name.log)"; exit 1; }
    # 811c9dc5 is the hash of nothing: a bench that received no bit at all
    if grep -q ' 811c9dc5$' "$D/$name.hash"; then echo "bench-hashes: $name received nothing (hash 811c9dc5; $D/$name.log)"; exit 1; fi
    exit 0
fi

mode=${1:-}
case $mode in check|record) ;; *) echo "usage: tools/bench-hashes.sh check|record" >&2; exit 2 ;; esac
[ "$mode" = record ] || [ -f "$GOLD" ] || { echo "bench hashes: no test/golden/bench.txt"; exit 1; }
mkdir -p "$D" || exit 1
progs=$(echo "$JOBS" | awk 'NF { print $2; for (i = 3; i <= NF; i++) if ($i ~ /^bench_/) print $i }' | sort -u)
make -s -C "$ROOT" -j"$(nproc 2>/dev/null || echo 4)" BUILD="$B" $(for p in $progs; do echo "$B/$p"; done) || exit 1
names=$(echo "$JOBS" | awk 'NF { print $1 }')
echo "$names" | xargs -P "$(echo "$names" | wc -l)" -I{} "$0" --one {} || exit 1
for n in $names; do cat "$D/$n.hash"; done > "$D/bench.txt"

if [ "$mode" = record ]; then
    mkdir -p "$(dirname "$GOLD")" && cp "$D/bench.txt" "$GOLD" || exit 1
    echo "bench hashes: $(wc -l < "$GOLD" | tr -d ' ') recorded in test/golden/bench.txt"
    exit 0
fi
if cmp -s "$GOLD" "$D/bench.txt"; then
    echo "bench hashes: $(wc -l < "$GOLD" | tr -d ' ') identical"
    exit 0
fi
echo "bench hashes: DIFFER from test/golden/bench.txt (- golden, + now; logs in $D):"
diff "$GOLD" "$D/bench.txt" | grep '^[<>]' | sed 's/^</-/; s/^>/+/'
exit 1
