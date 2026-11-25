#!/bin/sh
# Replay proof: every private recording through v90m-replay in six flag sets;
# what the tool prints, the bits it decides and its exit code, hashed against
# test/golden/replay.sha256.
#
#   tools/replay-proof.sh check       replay all, compare with the manifest
#   tools/replay-proof.sh record      replay all, write the manifest
#   tools/replay-proof.sh diff REF    replay with this tree and with the tool
#                                     built from git REF (git archive), print
#                                     the first 20 lines of the diff of each
#                                     trace that differs
#
# RECS (default /home/georgica/sync-build/live-050) holds the recordings; they
# are private and never in the repository: test/golden/recordings.sha256 is
# checked first. V90M_NO_RECS=1 skips the proof (exit 0). BUILD (default
# build) is where the tool is built and the outputs go: BUILD/replay/<set>/
# <rec>.{txt,bits,rc}. Every replay runs in a directory of its own (a tool
# without -b writes bits.txt where it runs). Manifest lines: "<sha256>  <set>/
# <rec>.<ext>", sorted.
set -u

# worker of run_all (xargs): SET REC, with RP_* in the environment
if [ "${1:-}" = --job ]; then
    set=$2; rec=$3
    case $set in
    base) o="" ;;       v92) o="-9" ;;
    x2) o="-x" ;;       x2-v92) o="-x -9" ;;
    k56) o="-k" ;;      k56-v92) o="-k -9" ;;
    *) exit 2 ;;
    esac
    w=$RP_OUT/$set/$rec.d
    mkdir -p "$w" || exit 2
    if [ "$RP_USEB" = 1 ]; then b="-b $RP_OUT/$set/$rec.bits"; else b=; fi
    # shellcheck disable=SC2086
    if command -v timeout >/dev/null 2>&1; then tm="timeout ${RP_TIMEOUT:-120}"; else tm=; fi
    (cd "$w" && env -u V92_TRACE -u RXBITS -u TXBITS -u TXOWN $tm "$RP_TOOL" $o $b "$RP_RECS/$rec.raw" > "$RP_OUT/$set/$rec.txt" 2>&1
     r=$?; echo $r > "$RP_OUT/$set/$rec.rc"; [ -n "$tm" ] && [ $r -eq 124 ] && : > "$RP_OUT/$set/$rec.timeout")
    [ "$RP_USEB" = 1 ] || mv "$w/bits.txt" "$RP_OUT/$set/$rec.bits" 2>/dev/null || : > "$RP_OUT/$set/$rec.bits"
    [ -f "$RP_OUT/$set/$rec.bits" ] || : > "$RP_OUT/$set/$rec.bits"
    rm -rf "$w"
    exit 0
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SELF=$ROOT/tools/replay-proof.sh
cd "$ROOT" || exit 1
BUILD=${BUILD:-build}
case $BUILD in /*) ;; *) BUILD=$ROOT/$BUILD ;; esac
RECS=${RECS:-/home/georgica/sync-build/live-050}
MAN=$ROOT/test/golden/replay.sha256
RECMAN=$ROOT/test/golden/recordings.sha256
JOBS=$(nproc 2>/dev/null || echo 4)
SETS="base v92 x2 x2-v92 k56 k56-v92"
export LC_ALL=C
mode=${1:-}
case $mode in
check|record) [ $# -eq 1 ] || mode= ;;
diff) [ $# -eq 2 ] || mode= ;;
*) mode= ;;
esac
[ -n "$mode" ] || { echo "usage: tools/replay-proof.sh check|record|diff REF" >&2; exit 2; }

if [ "${V90M_NO_RECS:-}" = 1 ]; then echo "replay proof: skipped (V90M_NO_RECS)"; exit 0; fi
if [ "$mode" = check ] && [ ! -f "$MAN" ]; then echo "replay proof: no test/golden/replay.sha256"; exit 1; fi
[ -d "$RECS" ] || { echo "replay proof: no recordings in $RECS (V90M_NO_RECS=1 skips)"; exit 1; }
(cd "$RECS" && sha256sum -c --quiet "$RECMAN" >/dev/null 2>&1) || {
    echo "replay proof: the recordings in $RECS are not those of test/golden/recordings.sha256:"
    (cd "$RECS" && sha256sum -c "$RECMAN" 2>&1 | grep -v ': OK$' | head -10)
    exit 1
}
RECLIST=$(awk '{ sub(/\.raw$/, "", $2); print $2 }' "$RECMAN")
NRUNS=$(( $(echo "$RECLIST" | wc -l) * 6 ))
TMPDIR=${TMPDIR:-/tmp}; export TMPDIR
mkdir -p "$TMPDIR" "$BUILD" || exit 1

# run_all TOOL OUT USEB: every set and recording, in parallel
run_all() {
    rm -rf "$2"; mkdir -p "$2" || exit 1
    for s in $SETS; do mkdir -p "$2/$s"; done
    for s in $SETS; do for r in $RECLIST; do echo "$s $r"; done; done |
        RP_TOOL=$1 RP_OUT=$2 RP_USEB=$3 RP_RECS=$RECS xargs -P "$JOBS" -L 1 sh "$SELF" --job ||
        { echo "replay proof: a job failed to run"; exit 1; }
    if [ -n "$(find "$2" -name '*.timeout' | head -1)" ]; then
        echo "replay proof: replays timed out:"; find "$2" -name '*.timeout' | sort | head -10 | sed 's/^/  /'
        exit 1
    fi
}

# hashes OUT: the manifest of an output tree
hashes() {
    (cd "$1" && for s in $SETS; do for r in $RECLIST; do
        echo "$s/$r.txt"; echo "$s/$r.bits"; echo "$s/$r.rc"
    done; done | xargs sha256sum | sort -k2)
}

make -s "$BUILD/v90m-replay" BUILD="$BUILD" >&2 || { echo "replay proof: build failed"; exit 1; }
OUT=$BUILD/replay
run_all "$BUILD/v90m-replay" "$OUT" 1
hashes "$OUT" > "$BUILD/replay.sha256.now"

case $mode in
record)
    [ "$(wc -l < "$BUILD/replay.sha256.now")" -eq $((NRUNS * 3)) ] || { echo "replay proof: incomplete outputs, manifest not written"; exit 1; }
    cp "$BUILD/replay.sha256.now" "$MAN.new" && mv "$MAN.new" "$MAN" || exit 1
    echo "replay proof: $NRUNS runs recorded in test/golden/replay.sha256"
    ;;
check)
    if cmp -s "$BUILD/replay.sha256.now" "$MAN"; then
        echo "replay proof: $NRUNS runs identical"
        exit 0
    fi
    echo "replay proof: differs from test/golden/replay.sha256:"
    diff "$MAN" "$BUILD/replay.sha256.now" | grep '^[<>]' | awk '{ print $3 }' | sort -u | head -20 | sed 's/^/  /'
    n=$(diff "$MAN" "$BUILD/replay.sha256.now" | grep '^[<>]' | awk '{ print $3 }' | sort -u | wc -l)
    echo "replay proof: $n files differ"
    exit 1
    ;;
diff)
    ref=$2
    rd=$BUILD/replay-ref/$ref
    rm -rf "$rd"; mkdir -p "$rd" || exit 1
    git archive "$ref" | tar -x -C "$rd" || { echo "replay proof: no such ref $ref"; exit 1; }
    env -u BUILD make -s -C "$rd" BUILD=build build/v90m-replay >&2 || { echo "replay proof: building $ref failed"; exit 1; }
    ROUT=$rd/out
    run_all "$rd/build/v90m-replay" "$ROUT" 0
    hashes "$ROUT" > "$rd/hashes"
    nd=0
    for s in $SETS; do for r in $RECLIST; do
        for e in txt bits rc; do
            f=$s/$r.$e
            if ! cmp -s "$OUT/$f" "$ROUT/$f"; then
                nd=$((nd + 1))
                echo "--- $f differs from $ref"
                diff "$ROUT/$f" "$OUT/$f" | head -20
            fi
        done
    done; done
    # a run differs when any of its three files does
    nr=0
    for s in $SETS; do for r in $RECLIST; do
        for e in txt bits rc; do
            cmp -s "$OUT/$s/$r.$e" "$ROUT/$s/$r.$e" || { nr=$((nr + 1)); break; }
        done
    done; done
    echo "replay proof: $NRUNS runs, $nr differ from $ref"
    [ "$nr" -eq 0 ]
    ;;
esac
