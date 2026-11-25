#!/bin/sh
# tools/check-diag-names.sh PROG
# Every error an engine's header defines has its name in v90m_err_name:
# PROG (test/api/test_diag.c run with --names) prints every name the
# library has; for each family, the names its header defines (every
# PREFIX_ERR_* there, the *_NONE that means no error left out) must be
# exactly those. An error added to an engine without its name in
# api/v90modem_diag.c (and its count in api/v90modem_diag.h) fails here.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
prog=${1:?usage: tools/check-diag-names.sh PROG}
got=$("$prog" --names) || { echo "diag names: $prog --names failed"; exit 1; }
fail=0; n=0
for f in V8_ERR_:api/v90modem_diag.h V21_ERR_:api/v90modem_diag.h V22_ERR_:api/v90modem_diag.h \
         V32_ERR_:v32/v32.h P2D_ERR_:modem/p2d.h P34_ERR_:modem/p34d.h V92D_ERR_:v92/v92_p34d.h \
         P2N_ERR_:modem/p2ans.h DX_ERR_:modem/v34dx.h V34A_ERR_:modem/v34ans.h X2D_ERR_:modem/x2d.h \
         K56D_ERR_:modem/k56d.h P2L_ERR_:modem/p2call.h V34O_ERR_:modem/v34orig.h V90A_ERR_:modem/v90a.h \
         ANS_ERR_:api/v90modem_diag.h ORIG_ERR_:api/v90modem_diag.h; do
    pre=${f%%:*}; h=$ROOT/${f#*:}
    want=$(grep -oE "(^|[^A-Za-z0-9_])${pre}[A-Z0-9_]+" "$h" | sed "s/^[^A-Z]*//" | grep -vx "${pre}NONE" | sort -u)
    have=$(printf '%s\n' "$got" | grep -E "^${pre}" | sort -u)
    if [ -z "$want" ] || [ "$want" != "$have" ]; then
        echo "diag names: ${pre}* in ${f#*:} and the library's names differ:"
        printf '%s\n' "$want" > "${TMPDIR:-/tmp}/dg_want.$$"; printf '%s\n' "$have" > "${TMPDIR:-/tmp}/dg_have.$$"
        diff "${TMPDIR:-/tmp}/dg_want.$$" "${TMPDIR:-/tmp}/dg_have.$$" | sed 's/^/  /'
        rm -f "${TMPDIR:-/tmp}/dg_want.$$" "${TMPDIR:-/tmp}/dg_have.$$"
        fail=1
    fi
    n=$((n + $(printf '%s\n' "$have" | grep -c .)))
done
# the name helpers' tables end at the public enums' last values
for e in v90m_phase:PHASE v90m_conn:CONN; do
    last=$(awk -v t="${e%%:*}" '/typedef enum/ {buf=""; inenum=1; next} inenum && $0 ~ "}[ ]*" t ";" {print buf; exit} inenum {buf=buf $0 "\n"}' "$ROOT/include/v90modem.h" \
           | sed 's,/\*.*\*/,,; s,/\*.*,,' | grep -oE 'V90M_[A-Z0-9_]+' | tail -1)
    def=$(sed -n "s/^#define DG_LAST_${e#*:}[ ]*//p" "$ROOT/api/v90modem_diag.h" | tr -d ' ')
    if [ -z "$last" ] || [ "$last" != "$def" ]; then
        echo "diag names: ${e%%:*}'s last value is ${last:-?} but DG_LAST_${e#*:} is ${def:-?}: name the new value"; fail=1
    fi
done
total=$(printf '%s\n' "$got" | grep -c .)
[ "$n" -eq "$total" ] || { echo "diag names: $total names printed, $n in the families' headers"; fail=1; }
[ $fail -eq 0 ] && echo "diag names: $n, each engine error named"
exit $fail
