#!/bin/sh
# A test program's output as the comparisons see it: without carriage
# returns (wine) and without the lines that measure the host, not the
# modem (TIMING: CPU and wall time, and the size of structures in memory,
# which follows pointer width and alignment: v90tx_t, v34_rx_t, V.44's
# encoder and decoder). tools/check-ports.sh (target against target) and
# tools/golden.sh (the host against test/golden/out) both use it. These
# lines are left out of the golden files too: a test must print no result
# on a line that matches TIMING.
#
#   tools/out-filter.sh < OUTPUT          the filtered output on stdout
#   . tools/out-filter.sh; out_filter F   the same from a script, of file F
TIMING='of CPU here|% of real time| ns +[0-9]+ ns +[0-9]+ ns$|^  speed on |^  sizeof\(|^  decoder state: sizeof\(|^  sizes: encoder [0-9]+ octets, decoder [0-9]+ octets$'

# grep -v exits 1 when every line was left out: still a good filter
out_filter() {
    [ -r "$1" ] || { echo "out-filter: cannot read $1" >&2; return 2; }
    tr -d '\r' < "$1" | { grep -vE "$TIMING"; [ $? -le 1 ]; }
}

case $0 in */out-filter.sh|out-filter.sh) out_filter /dev/stdin ;; esac
