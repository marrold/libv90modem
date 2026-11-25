#!/bin/sh
# check.sh [--record] REPLAY: replay every fixture of fixtures.txt that is
# here (all at once) and check what the modem connected with against
# fixtures.txt, and the whole trace against golden/<name>.txt (cxa.raw.xz:
# golden/cxa.txt). Exit 1 on any difference. --record writes the traces
# to golden/ instead (the connection is still checked); a trace is
# re-recorded only when a change explains it, and the commit says why.
rec=0
[ "${1:-}" = --record ] && { rec=1; shift; }
[ $# -eq 1 ] || { echo "usage: test/fixtures/check.sh [--record] REPLAY" >&2; exit 2; }
R=$1
case $R in /*) ;; *) R=$(pwd)/$R ;; esac
cd "$(dirname "$0")" || exit 1
tmp=${TMPDIR:-/tmp}/v90m-fixtures.$$
rm -rf "$tmp"; mkdir -p "$tmp" || exit 1
trap 'rm -rf "$tmp"' EXIT
list=$(grep -v '^#' fixtures.txt | grep .)

# replay each present fixture in the background: TMP/<name>.txt, .rc
while IFS= read -r line; do
    f=${line%% *}; n=${f%.raw.xz}
    [ -f "$f" ] || continue
    opts=$(echo "$line" | sed 's/^[^ ]* *//; s/ *modulation.*//')
    # shellcheck disable=SC2086
    # in a directory of its own: v90m-replay writes bits.txt where it runs
    mkdir -p "$tmp/$n" || exit 1
    { xz -dc "$f" > "$tmp/$n/$n.raw" && (cd "$tmp/$n" && "$R" $opts "$n.raw") > "$tmp/$n.txt" 2>&1
      echo $? > "$tmp/$n.rc"; rm -f "$tmp/$n/$n.raw"; } &
done <<EOT
$list
EOT
wait

fail=0
[ $rec -eq 1 ] && mkdir -p golden
while IFS= read -r line; do
    f=${line%% *}; n=${f%.raw.xz}
    if [ ! -f "$f" ]; then echo "  $f: not here, skipped"; continue; fi
    want=$(echo "$line" | grep -o 'modulation [0-9]*, [0-9]* bit/s down, [0-9]* up')
    got=$(grep -o 'modulation [0-9]*, [0-9]* bit/s down, [0-9]* up' "$tmp/$n.txt" 2>/dev/null)
    rc=$(cat "$tmp/$n.rc" 2>/dev/null || echo none)
    if [ "$rc" != 0 ]; then echo "  FAIL $f: replay exit $rc"; fail=1; continue; fi
    if [ "$got" != "$want" ]; then echo "  FAIL $f: got \"$got\", want \"$want\""; fail=1; continue; fi
    if [ $rec -eq 1 ]; then
        cp "$tmp/$n.txt" "golden/$n.txt" || fail=1
        echo "  $f: $got; trace recorded in test/fixtures/golden/$n.txt"
    elif [ ! -f "golden/$n.txt" ]; then
        echo "  FAIL $f: no test/fixtures/golden/$n.txt"; fail=1
    elif cmp -s "golden/$n.txt" "$tmp/$n.txt"; then
        echo "  $f: $got; trace identical"
    else
        echo "  FAIL $f: $got, but the trace DIFFERS from test/fixtures/golden/$n.txt (- golden, + now; first 10 lines):"
        diff "golden/$n.txt" "$tmp/$n.txt" | grep '^[<>]' | head -10 | sed 's/^</    -/; s/^>/    +/'
        fail=1
    fi
done <<EOT
$list
EOT
if [ $fail -ne 0 ]; then echo "FIXTURES FAILED"; exit 1; fi
echo "fixtures: all present ones pass"
