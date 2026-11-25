#!/bin/sh
# make verify-phase: the check at the end of each phase (docs/superpowers/
# specs/2026-11-22-audit-refactor-optimize-design.md 7.4; docs/history/
# audit-0.6.md has its measured time). The costs run in the background from
# the start; the other stages one after another; each prints one line:
#   armv5     make check-arm with the armv5 uClibc compiler (the box's ABI),
#             PAR=16, the whole short set
#   ports     tools/check-ports.sh build/ports: the whole suites on host gcc,
#             armv5, s390x and riscv64, QUICK on the rest; every target
#             must be identical to host gcc
#   armv7hf   make check-arm with the armv7hf musl compiler, the whole short
#             set, compared with the host short set (tools/short-cmp.sh)
#   benches   bench-v90 bench-x2 bench-k56 bench-v34 bench-v90a bench-v90c
#             bench-v92c bench-bell (armv5), then each recording replayed on
#             the host and under qemu: the hashes must be the same
#   crd       make crd-scan RECS=...: 0 CRd detections
#   costs     cost-x2 cost-k56 cost-v34 cost-v90a cost-v90c cost-v92c
#             cost-bell (armv5, COST_TICKS=0): their lines in build/costs.txt
# then "VERIFY-PHASE PASS (<seconds> s)" or "VERIFY-PHASE FAILED". Logs:
# build/verify-phase/<stage>.log.
#
#   PORTS_PAR=2                  targets at once (each 16 / PORTS_PAR programs)
#   A5=<armv5 arm-linux-gcc>     default ../toolchains/armv5-eabi--uclibc--*
#   A7=<armv7hf arm-linux-gcc>   default ../toolchains/armv7-eabihf--musl--*
#   RECS=<recordings>            default /home/georgica/sync-build/live-050
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
unset V92_VERBOSE V32_DEBUG V92_TRACE RXBITS TXBITS TXOWN
unset MAKEFLAGS MFLAGS MAKELEVEL BUILD
TMPDIR=${TMPDIR:-$ROOT/build/tmp}; export TMPDIR
mkdir -p "$TMPDIR" || exit 1
tc() { ls -d "$ROOT"/../toolchains/$1/bin/arm-linux-gcc 2>/dev/null | head -1; }
A5=${A5:-$(tc 'armv5-eabi--uclibc--*')}
A7=${A7:-$(tc 'armv7-eabihf--musl--*')}
RECS=${RECS:-/home/georgica/sync-build/live-050}
[ -n "$A5" ] && [ -x "$A5" ] || { echo "verify-phase: no armv5 compiler (A5=$A5)"; exit 1; }
[ -n "$A7" ] && [ -x "$A7" ] || { echo "verify-phase: no armv7hf compiler (A7=$A7)"; exit 1; }
V=$ROOT/build/verify-phase
rm -rf "$V"; mkdir -p "$V" || exit 1
now() { date +%s; }
t0=$(now)
fail=0

# run NAME COMMAND: run it (log NAME.log), then print the stage's line,
# made by line_NAME from the log
run() {
    n=$1; shift
    s=$(now)
    ( eval "$*" ) > "$V/$n.log" 2>&1; rc=$?
    t=$(( $(now) - s ))
    l=$(line_$n 2>/dev/null)
    if [ $rc -eq 0 ]; then printf '%-8s ok    %5ss  %s\n' "$n" "$t" "$l"
    else printf '%-8s FAIL  %5ss  %s (build/verify-phase/%s.log)\n' "$n" "$t" "$l" "$n"; tail -15 "$V/$n.log" | sed 's/^/    /'; fail=1; fi
}

# the costs (instruction counts, the same whatever the load): each cost-*
# in a build tree of its own, all at once, in the background from the
# start, beside the other stages; their lines in build/costs.txt (cost-v90c
# and cost-v92c without the slowest ticks counted one by one, COST_TICKS=0)
COSTS="x2 k56 v34 v90a v90c v92c bell"
line_costs() { echo "$(grep -vc '^#' "$ROOT/build/costs.txt" 2>/dev/null || echo 0) lines in build/costs.txt"; }
costs() {
    for c in $COSTS; do
        ( make -s -j4 ARM_CC="$A5" BUILD=build/vp-cost/$c COST_TICKS=0 cost-$c > "$V/cost-$c.log" 2>&1; echo $? > "$V/cost-$c.rc" ) &
    done
    wait
    st=0
    for c in $COSTS; do
        [ "$(cat "$V/cost-$c.rc")" = 0 ] || { echo "cost-$c FAILED ($V/cost-$c.log)" >&2; st=1; }
        echo "# cost-$c"
        grep -E 'ARM instructions|ratio' "$V/cost-$c.log"
    done > "$ROOT/build/costs.txt"
    cat "$ROOT/build/costs.txt"
    return $st
}
( s=$(now); costs > "$V/costs.log" 2>&1; echo $? > "$V/costs.rc"; echo $(( $(now) - s )) > "$V/costs.time" ) &
costs_pid=$!

line_armv5() { grep -E '^(ALL ARM CHECKS PASS|ARM CHECKS FAILED)' "$V/armv5.log" | tail -1; }
run armv5 "make -s -j16 check-arm PAR=16 ARM_CC='$A5' BUILD=build/a5"

# every target: the whole suite (-DTEST_FULL) on host gcc (the reference),
# armv5 (the box's ABI), s390x (big-endian, glibc) and riscv64 (64-bit
# little-endian); QUICK on the others (the slow programs of SLOW left out:
# tools/ports-slow.txt and the two longest everywhere, which the four run).
# Every target gates; a target that did not run (no compiler, no runner:
# SKIPPED) is a failure, so that a pass means all of them ran. Slowest
# targets first, PORTS_PAR targets at once, each running PAR/PORTS_PAR
# programs at once.
FULL="armv5-uclibc s390x-glibc riscv64-musl"
ORDER="armv5-uclibc s390x-glibc riscv64-musl i686-musl mips-musl mipsel-musl ppc64-musl ppc-musl m68k-uclibc
aarch64be-musl aarch64-musl x86_64-musl armv7hf-musl win32-mingw host-clang host-gcc"
PORTS_PAR=${PORTS_PAR:-2}
line_ports() { cat "$V/ports.sum" 2>/dev/null; }
ports() {
    quick=$(for t in $ORDER; do case " $FULL host-gcc " in *" $t "*) ;; *) echo "$t" ;; esac; done | tr '\n' ' ')
    { cat tools/ports-slow.txt; echo arm_test_orig_retrain; echo arm_test_orig_v90_te; } > "$V/ports-slow.txt"
    # shellcheck disable=SC2086
    PAR=$PORTS_PAR PROG_PAR=$((16 / PORTS_PAR)) JOBS=$((16 / PORTS_PAR)) QUICK="$quick" SLOW="$V/ports-slow.txt" \
        tools/check-ports.sh build/ports $ORDER > "$V/ports.out" 2>&1
    rc=$?
    cat "$V/ports.out"
    [ $rc -le 1 ] || { echo "ports: check-ports.sh exited $rc" > "$V/ports.sum"; return 1; }
    # the gate: every target, compared again, must pass whole
    # shellcheck disable=SC2086
    COMPARE_ONLY=1 tools/check-ports.sh build/ports $ORDER > "$V/ports.gate" 2>&1; grc=$?
    want=$(printf '%s\n' $ORDER | grep -vc '^host-gcc$')
    # a target passes whole when its identical and skipped (quick) programs make up all of them
    got=$(awk '/FAILED|DIFFER|NOT RUN/ { next }
        match($0, /^[a-z0-9_-]+: exit 0, [0-9]+ of [0-9]+ programs identical/) {
            split(substr($0, RSTART, RLENGTH), w, " "); k = 0
            if (match($0, /, [0-9]+ skipped \(quick\)/)) { split(substr($0, RSTART + 2), x, " "); k = x[1] }
            if (w[4] + k == w[6]) n++ }
        END { print n + 0 }' "$V/ports.gate")
    # m68k (the last to be made gating, Task 5b): its line, and that it ran
    m68k=$(grep '^m68k-uclibc: exit 0, ' "$V/ports.gate")
    echo "$got of $want targets identical to host-gcc (full: $FULL); ${m68k:-m68k-uclibc: did not run}" > "$V/ports.sum"
    cat "$V/ports.gate"
    [ $grc -eq 0 ] && grep -q '^ALL PORTS PASS' "$V/ports.gate" && [ "$got" -eq "$want" ] && [ -n "$m68k" ] || {
        echo "FAILED: $(cat "$V/ports.sum")" > "$V/ports.sum"; return 1; }
}
run ports ports

line_armv7hf() { grep -E '^short-cmp: [0-9]' "$V/armv7hf.log" | tail -1; }
run armv7hf "{ make -s -j16 test-all PAR=16 BUILD=build/short EXTRA_CPPFLAGS=-DTEST_FORCE_SHORT > '$V/short.log' 2>&1 || { tail '$V/short.log'; exit 1; }; } &&
    make -s -j16 check-arm PAR=16 ARM_CC='$A7' BUILD=build/a7 && tools/short-cmp.sh build/short build/a7"

# the benches: made (armv5), then each recording on the host and on qemu,
# whose hash lines ("tx hash", "rx hash", the receiver's "hash") must agree
B=build/vp-bench
line_benches() { cat "$V/benches.sum" 2>/dev/null; }
benches() {
    make -s -j16 ARM_CC="$A5" BUILD=$B bench-v90 bench-x2 bench-k56 bench-v34 bench-v90a bench-v90c bench-v92c bench-bell || return 1
    n=0; bad=
    while read -r name rec prog opts; do
        [ -n "$name" ] || continue
        # shellcheck disable=SC2086
        "$B/$prog" $opts "$B/$rec" > "$V/bench-$name.host" 2>&1
        # shellcheck disable=SC2086
        qemu-arm-static "$B/$prog.arm" $opts "$B/$rec" > "$V/bench-$name.arm" 2>&1
        h=$(grep -oE '(tx |rx )?hash [0-9a-f]{8}' "$V/bench-$name.host")
        a=$(grep -oE '(tx |rx )?hash [0-9a-f]{8}' "$V/bench-$name.arm")
        if [ -n "$h" ] && [ "$h" = "$a" ]; then n=$((n + 1)); else bad="$bad $name"; fi
    done <<EOT
v90     v90_call.rec bench_v90
api     v90_call.rec bench_api
x2      x2_call.rec  bench_v90  -x
k56     k56_call.rec bench_v90  -k
v34     v34_call.rec bench_v34
v90a    v90a.rec     bench_v90a
v90c    v90c.rec     bench_v90c
v92c    v92c.rec     bench_v90c
bell103 bell_103.rec bench_bell
bell212 bell_212.rec bench_bell
EOT
    if [ -z "$bad" ]; then echo "host = qemu hashes: $n benches" > "$V/benches.sum"; return 0; fi
    echo "host != qemu hashes:$bad ($n agree)" > "$V/benches.sum"; return 1
}
run benches benches

line_crd() { tail -1 "$V/crd.log"; }
run crd "make -s crd-scan RECS='$RECS' | tee /dev/stderr | tail -1 | grep -q ' 0 CRd detections'"

# the costs, started first
wait $costs_pid
rc=$(cat "$V/costs.rc" 2>/dev/null || echo 1)
if [ "$rc" = 0 ]; then printf '%-8s ok    %5ss  %s\n' costs "$(cat "$V/costs.time")" "$(line_costs)"
else printf '%-8s FAIL  %5ss  %s (build/verify-phase/costs.log)\n' costs "$(cat "$V/costs.time" 2>/dev/null)" "$(line_costs)"; tail -15 "$V/costs.log" | sed 's/^/    /'; fail=1; fi

t=$(( $(now) - t0 ))
if [ $fail -eq 0 ]; then echo "VERIFY-PHASE PASS ($t s)"; exit 0; fi
echo "VERIFY-PHASE FAILED ($t s)"
exit 1
