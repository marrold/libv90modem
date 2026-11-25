#!/bin/sh
# replay-opts.sh REPLAY REC_ORIG: v90m-replay's -s (start sample), -l
# (level, also decimal), -r (level as an rms), -B (Bell offered), -M (mark
# only in data mode) and -F (the caller's V.42 frames) on the public
# recordings of fixtures.txt and on a simulated call with V.42 on both
# sides that REC_ORIG (tools/rec_orig.c) records. Exit 1 on any failure.
#
#   -s: the default run starts the modem a little before the answer tone; -s S
#       starts it at sample S, and the times are the recording's either way.
#       So a default run and "-s S" for the S where it starts give the same
#       trace, to the byte, but for the two lines at the top. And a recording
#       with N samples of its start cut off (N a multiple of 80, the block)
#       with "-s S-N" gives what the whole recording gives with "-s S", with
#       the times (all N/8000 s later in the whole one) taken out.
#   -l: whole numbers give what they always did, "-14.0" and "-14.00" are
#       "-14"; "-14.4" differs from "-14" only in how many samples differ
#       from the recording (the recording's modem sent at -12); bad values
#       exit 2 with the usage message.
#   -r: the transmit rms itself, as nsdiald's sidecar records it
#       (tx_level_rms): -12 dBm0 is rms 3963 and -14 is 3148, so "-r 3963"
#       is the default and "-r 3148" is "-l -14"; "-r 3000" differs from
#       "-l -14" only in the counts; of -l and -r the last one given holds;
#       anything but a whole number 1..16384 exits 2 with the usage message.
#   -B: offering Bell 103 and 212A changes nothing for v34a, a V.8 caller;
#       v21a (V.21 forced, no V.8, silent until it hears our V.21) then gets
#       the Bell 212A answer tone 4 s into the V.22 wait, which its
#       recording does not have.
#   -M, -F: the simulated V.34 call with V.42 (rec_orig v34 OUT ec) replays
#       with our transmit as recorded to its end (but the last block's
#       padding); with -M it differs from the first data-mode bit on and not
#       before; -F adds only its frame lines, the caller's XID and SABME
#       among them with a good FCS, and the count.
R=$1; O=${2:-}
[ $# -eq 2 ] || { echo "usage: test/fixtures/replay-opts.sh REPLAY REC_ORIG" >&2; exit 2; }
case $R in /*) ;; *) R=$(pwd)/$R ;; esac
case $O in /*) ;; *) O=$(pwd)/$O ;; esac
cd "$(dirname "$0")" || exit 1
tmp=${TMPDIR:-/tmp}/v90m-replay-opts.$$
rm -rf "$tmp"; mkdir -p "$tmp" || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0
bad() { echo "  FAIL: $*"; fail=1; }
cd "$tmp" || exit 1
xz -dc "$OLDPWD/v34a.raw.xz" > v34a.raw || exit 1       # companded, V.34
xz -dc "$OLDPWD/v21a.raw.xz" > v21a.raw || exit 1

# the traces with their times taken out
norm() {
    sed -E '/^recording:/d; /^answer tone at/d; /^the modem starts at/d; /^  second [0-9]+:/d
            s/ *[0-9]+\.[0-9]+ s/ T s/g; s/^[0-9]+ samples;/N samples;/' "$1"
}

# -s: where the default run starts
"$R" v34a.raw > def.txt || bad "default run"
ans=$(sed -n 's/^answer tone at \([0-9.]*\) s.*/\1/p' def.txt)
lo=$(awk -v a="$ans" 'BEGIN { printf "%d", a * 8000 - 1600 - 5 }')
tail -n +3 def.txt > def.n
found=
s=$lo
while [ "$s" -le $((lo + 10)) ]; do
    "$R" -s "$s" v34a.raw > s.txt || bad "-s $s exit"
    tail -n +3 s.txt > s.n
    if cmp -s def.n s.n; then found=$s; break; fi
    s=$((s + 1))
done
if [ -z "$found" ]; then bad "no -s near the default start (about sample $lo) gives the default trace"
else echo "  -s $found: the default trace"; fi

# -s on a recording with its start cut off
if [ -n "$found" ]; then
    cut=100                                             # blocks = 8000 samples
    dd if=v34a.raw of=short.raw bs=160 skip=$cut 2>/dev/null
    "$R" -s "$found" v34a.raw > a.txt; "$R" -s $((found - cut * 80)) short.raw > b.txt
    # the first two lines have the recording's length and the start sample (norm drops them)
    if cmp -s a.txt b.txt; then bad "cut and uncut have the same header (the test is empty)"
    elif [ "$(norm a.txt)" = "$(norm b.txt)" ]; then
        echo "  -s $found, and -s $((found - cut * 80)) on the recording cut by $((cut * 80)): same trace, times apart"
    else bad "-s on the cut recording gives another trace"; diff a.txt b.txt | head -5; fi
fi

# -s at 0 and out of range
"$R" -s 0 v34a.raw > z.txt || bad "-s 0 exit"
grep -q '^the modem starts at sample 0 of the recording' z.txt || bad "-s 0: no start line"
"$R" -s 99999999 v34a.raw > /dev/null 2>&1; [ $? -eq 2 ] || bad "-s past the end: not exit 2"

# -l
"$R" v21a.raw > l.def; "$R" -l -12 v21a.raw > l.12
cmp -s l.def l.12 || bad "-l -12 is not the default"
"$R" -l -14 v21a.raw > l.14 || bad "-l -14"
"$R" -l -14.0 v21a.raw > l.140; "$R" -l -14.00 v21a.raw > l.1400
cmp -s l.14 l.140 && cmp -s l.14 l.1400 || bad "-l -14.0 or -14.00 is not -14"
"$R" -l -14.4 v21a.raw > l.144 || bad "-l -14.4"
cmp -s l.14 l.144 && bad "-l -14.4 is -14"
if diff l.14 l.144 | grep '^[<>]' | grep -vq -e '^[<>]   second [0-9]*: ' -e '^[<>] [0-9]* samples; after the answer'; then
    bad "-l -14.4 differs from -14 in more than the differing-sample counts"
    diff l.14 l.144 | head -5
else echo "  -l -14.4 differs from -14 in the counts of differing samples only"; fi
"$R" -l -14.4 v21a.raw | cmp -s - l.144 || bad "-l -14.4 not repeatable"
for v in -14.4.1 -14.456 abc "" - -. . 1e3 --5 -14,4 -1234 0x10; do
    "$R" -l "$v" v21a.raw > l.out 2> l.err; rc=$?
    if [ $rc -ne 2 ] || ! grep -q '^usage' l.err; then bad "-l '$v': exit $rc, want 2 and the usage message"; fi
done
for v in -1 abc "" 1.5 -0 1e3 " 5"; do
    "$R" -s "$v" v21a.raw > l.out 2> l.err; rc=$?
    if [ $rc -ne 2 ] || ! grep -q '^usage' l.err; then bad "-s '$v': exit $rc, want 2 and the usage message"; fi
done

# -r
"$R" -r 3963 v21a.raw > r.3963 || bad "-r 3963"
cmp -s l.def r.3963 || bad "-r 3963 is not the default (-12 dBm0)"
"$R" -r 3148 v21a.raw > r.3148 || bad "-r 3148"
cmp -s l.14 r.3148 || bad "-r 3148 is not -l -14"
"$R" -r 3000 v21a.raw > r.3000 || bad "-r 3000"
cmp -s l.14 r.3000 && bad "-r 3000 is -l -14"
if diff l.14 r.3000 | grep '^[<>]' | grep -vq -e '^[<>]   second [0-9]*: ' -e '^[<>] [0-9]* samples; after the answer'; then
    bad "-r 3000 differs from -l -14 in more than the differing-sample counts"
    diff l.14 r.3000 | head -5
else echo "  -r 3000 differs from -l -14 in the counts of differing samples only"; fi
"$R" -l -14 -r 3963 v21a.raw | cmp -s - l.def || bad "-l -14 -r 3963 is not -r 3963"
"$R" -r 3963 -l -14 v21a.raw | cmp -s - l.14 || bad "-r 3963 -l -14 is not -l -14"
for v in 0 16385 -5 abc "" 1.5 " 5" 3000x 1e3 99999999999; do
    "$R" -r "$v" v21a.raw > l.out 2> l.err; rc=$?
    if [ $rc -ne 2 ] || ! grep -q '^usage' l.err; then bad "-r '$v': exit $rc, want 2 and the usage message"; fi
done

# the first sample where our transmit differs from the recording, s
first() { sed -n 's/.*differs from the recording in [0-9]* (first at \([0-9.]*\) s).*/\1/p' "$1"; }

# -B
"$R" v34a.raw > b.def; "$R" -B v34a.raw > b.bell || bad "-B v34a"
cmp -s b.def b.bell || bad "-B changes the trace of v34a (a V.8 caller)"
"$R" -B v21a.raw > b.bell || bad "-B v21a"
v22=$(sed -n 's/^ *\([0-9.]*\) s  modem phase V22 .*/\1/p' l.def | head -1)
fb=$(first b.bell)
if [ -n "$v22" ] && [ -n "$fb" ] && awk -v b="$fb" -v v="$v22" 'BEGIN { d = b - v - 4; exit !(d > -0.01 && d < 0.01) }'; then
    echo "  -B: a V.8 caller's trace unchanged; v21a's (no V.8) differs from the 2225 Hz tone on, 4 s into V.22 ($fb s)"
else bad "-B on v21a: first difference at $fb s, V.22 from $v22 s (want 4 s later)"; fi

# -M and -F on a simulated call with V.42
"$O" v34 ec.raw ec > /dev/null || bad "rec_orig v34 ec"
"$R" ec.raw > e.def || bad "ec default"
"$R" -M ec.raw > e.mark || bad "-M"
"$R" -F ec.raw > e.fr || bad "-F"
conn=$(sed -n 's/^ *\([0-9.]*\) s    V.42 state 0,.*/\1/p' e.def | head -1)
fd=$(first e.def); fm=$(first e.mark); n=$(sed -n 's/^recording: .*, \([0-9]*\) samples$/\1/p' e.def)
if [ -z "$conn" ] || [ -z "$fd" ] || [ -z "$fm" ]; then bad "-M: no data mode or no first difference in the traces"
elif awk -v d="$fd" -v n="$n" 'BEGIN { exit !(d * 8000 >= n - 80) }' && awk -v m="$fm" -v c="$conn" 'BEGIN { exit !(m >= c && m < c + 0.1) }'; then
    echo "  -M: our transmit as recorded to the last block; mark only differs from $fm s, data mode at $conn s"
else bad "-M: first differences $fd (default) and $fm (-M) s, data mode at $conn s"; fi
grep -v -e '^ *[0-9.]* s    V.42 frame in: ' -e '^V.42 frames in: ' e.fr | cmp -s - e.def || bad "-F changes more than its own lines"
grep -q '^ *[0-9.]* s    V.42 frame in: [0-9]* octets, address 0x03 (C/R 1), control 0xAF, XID, FCS-16 ok$' e.fr || bad "-F: no XID"
grep -q '^ *[0-9.]* s    V.42 frame in: 2 octets, address 0x03 (C/R 1), control 0x7F, SABME P/F, FCS-16 ok$' e.fr || bad "-F: no SABME"
grep -q '^V.42 frames in: [1-9][0-9]* with a good FCS, 0 bad$' e.fr || bad "-F: no count"
echo "  -F: $(grep -c 'V.42 frame in:' e.fr) frames of the caller, nothing else changed"
"$R" -Z v21a.raw > l.out 2> l.err; rc=$?
if [ $rc -ne 2 ] || ! grep -q -- '-B\] \[-M\] \[-F\]' l.err; then bad "the usage message does not list -B, -M, -F"; fi

[ $fail -eq 0 ] && echo "replay options: pass"
[ $fail -eq 0 ] || { echo "REPLAY OPTIONS FAILED"; exit 1; }
