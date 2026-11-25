#!/bin/sh
# Run test programs in parallel, then print their output in the given order.
#
#   tools/par-run.sh LOGDIR "RUNNER" PREFIX ENTRY...
#
# An ENTRY is PROG or PROG:ARG, and runs as `RUNNER PROG [ARG]` (RUNNER may
# be empty, or e.g. qemu-arm-static), PAR at a time (default 8), its stdout
# and stderr to LOGDIR/<key>.log. Its key is its file name, after the
# subdirectory it belongs to (v34/test_scr) when it is one of the SUBS of
# the Makefile, then ":ARG".
#
# The longest start first (an LPT schedule), by the seconds per key in
# PAR_TIMES (default tools/test-times.txt); a key it does not list starts
# before all of them. The seconds this run measured go to PAR_TIMES_OUT
# (default test-times.txt next to LOGDIR, i.e. $(BUILD)/test-times.txt),
# merged into what that file held; `make test-times` copies them into
# tools/test-times.txt, which a run never writes.
#
# PAR_SKIP names a file of keys (without ":ARG"), one per line, as
# tools/ports-slow.txt lists them: those entries are left out, and a line
# says how many.
#
# Afterwards, for each ENTRY in the given order: "PREFIX <key>" (when PREFIX
# is not empty; arm_ taken off the name) and its log. Exit status 1 when any
# ENTRY failed.
#
#   tools/par-run.sh --merge FROM INTO
#
# Merges the times file FROM into INTO (what `make test-times` does).
root=$(cd "$(dirname "$0")/.." && pwd)

# merge INTO < "seconds key" lines: INTO keeps its other keys
merge() {
	tmp=$1.$$
	{ [ -f "$1" ] && grep -v '^#' "$1"; echo "--"; cat; } | awk '
	$0 == "--" { next }
	NF >= 2 { t[$2] = $1 }
	END { for (k in t) print t[k], k }' | sort -k2,2 > "$tmp" \
		&& { echo "# seconds per test program, by key (tools/par-run.sh starts the longest first; make test-times)"; cat "$tmp"; } > "$1"
	st=$?; rm -f "$tmp"; return $st
}
if [ "${1:-}" = --merge ]; then
	[ -f "$2" ] || { echo "par-run: no $2" >&2; exit 1; }
	grep -v '^#' "$2" | merge "$3"; exit
fi

logdir=$1; runner=$2; prefix=$3; shift 3
par=${PAR:-8}
times=${PAR_TIMES:-$root/tools/test-times.txt}
mkdir -p "$logdir" || exit 1
logdir=$(cd "$logdir" && pwd)
times_out=${PAR_TIMES_OUT:-$(dirname "$logdir")/test-times.txt}
subs=$(sed -n 's/^SUBS *:= *//p' "$root/Makefile")

# ENTRY KEY, one per line, in the given order (less those PAR_SKIP lists)
map=$logdir/entries.txt
skipf=${PAR_SKIP:-/dev/null}
[ -r "$skipf" ] || { echo "par-run: cannot read PAR_SKIP $skipf" >&2; exit 1; }
nall=$#
printf '%s\n' "$@" | awk -v subs="$subs" -v skipf="$skipf" '
BEGIN { n = split(subs, s, " "); for (i = 1; i <= n; i++) sub_[s[i]] = 1
        while ((getline l < skipf) > 0) if (l != "" && l !~ /^#/) skip[l] = 1 }
{
    e = $0; p = e; a = ""
    if (match(e, /:[^\/:]*$/)) { p = substr(e, 1, RSTART - 1); a = substr(e, RSTART) }
    m = split(p, c, "/"); k = c[m]
    if (m >= 2 && (c[m - 1] in sub_)) k = c[m - 1] "/" k
    else if (m >= 3 && c[m - 1] == "build" && (c[m - 2] in sub_)) k = c[m - 2] "/" k
    if (k in skip) next
    print e, k a
}' > "$map" || exit 1
nrun=$(wc -l < "$map" | tr -d ' ')
[ "$nrun" -eq "$nall" ] || echo "par-run: $((nall - nrun)) of $nall runs skipped (${skipf#"$root"/})"

while read -r e k; do
	mkdir -p "$(dirname "$logdir/$k")"
	rm -f "$logdir/$k.log" "$logdir/$k.rc" "$logdir/$k.time"
done < "$map"

# longest first: unknown keys (class 0) in the given order, then by seconds
{ [ -f "$times" ] && cat "$times"; echo "--"; cat "$map"; } | awk '
$0 == "--" { entries = 1; next }
!entries { if ($1 !~ /^#/ && NF >= 2) t[$2] = $1; next }
{ i++; if ($2 in t) print 1, t[$2], i, $1, $2; else print 0, 0, i, $1, $2 }' \
	| sort -k1,1n -k2,2nr -k3,3n | awk '{ print $4, $5 }' \
	| PAR_RUNNER=$runner PAR_LOGDIR=$logdir xargs -n 2 -P "$par" sh -c '
		e=$1; k=$2; p=$e; a=
		case $e in *:*) case ${e##*:} in */*) ;; *) p=${e%:*}; a=${e##*:} ;; esac ;; esac
		now() { t=$(date +%s%N); case $t in *N) t=$(( $(date +%s) * 1000000000 )) ;; esac; echo "$t"; }
		t0=$(now)
		# shellcheck disable=SC2086
		$PAR_RUNNER "$p" $a > "$PAR_LOGDIR/$k.log" 2>&1
		echo $? > "$PAR_LOGDIR/$k.rc"
		echo $(( $(now) - t0 )) > "$PAR_LOGDIR/$k.time"' sh

# the seconds measured, merged into PAR_TIMES_OUT
while read -r e k; do
	[ -f "$logdir/$k.time" ] && awk -v k="$k" '{ printf "%.1f %s\n", $1 / 1e9, k }' "$logdir/$k.time"
done < "$map" | merge "$times_out"

fail=0
while read -r e k; do
	if [ -n "$prefix" ]; then
		case $k in */*) d=${k%/*}/ n=${k##*/} ;; *) d= n=$k ;; esac
		echo "$prefix $d${n#arm_}"
	fi
	cat "$logdir/$k.log"
	rc=$(cat "$logdir/$k.rc" 2>/dev/null || echo 1)
	[ "$rc" = 0 ] || fail=1
done < "$map"
exit $fail
