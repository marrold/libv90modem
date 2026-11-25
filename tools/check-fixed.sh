#!/bin/sh
# make check-fixed: fixed point and no allocation after create, checked on
# the box's compiler (the armv5 soft-float one; there is no FPU, so any
# float or double, any conversion to or from one and any libm call shows as
# an undefined symbol of the object).
#
#   tools/check-fixed.sh NM OBJDIR OBJECT...   (the library's objects, under OBJDIR)
#
# Every symbol an object leaves undefined must be one of
#   - defined by another object of the library (its internals, v90m_*);
#   - in ALLOW below: the integer helpers of the ARM EABI and of libgcc,
#     and memcpy, memmove, memset, memcmp;
#   - malloc, calloc, realloc, free, in api/v90modem.o only (where the
#     create functions are; no other allocator anywhere).
# Anything else fails: a soft-float helper (__aeabi_f*, __aeabi_d*,
# __aeabi_i2d, __addsf3 ...), a libm function (sin, sqrt, lrint ...), any
# other C library function. A symbol new to the library is added to ALLOW
# only when it is an integer helper or a memory primitive.
set -u
NM=$1; D=${2%/}; shift 2
ALLOW='__aeabi_idiv __aeabi_idivmod __aeabi_uidiv __aeabi_uidivmod __aeabi_ldivmod __aeabi_uldivmod
__aeabi_lmul __aeabi_llsl __aeabi_llsr __aeabi_lasr __aeabi_lcmp __aeabi_ulcmp
__clzsi2 __clzdi2 __ctzsi2 __ctzdi2 __popcountsi2 __popcountdi2 __ffssi2 __ffsdi2
__aeabi_memcpy __aeabi_memcpy4 __aeabi_memcpy8 __aeabi_memmove __aeabi_memmove4 __aeabi_memmove8
__aeabi_memset __aeabi_memset4 __aeabi_memset8 __aeabi_memclr __aeabi_memclr4 __aeabi_memclr8
memcpy memmove memset memcmp _GLOBAL_OFFSET_TABLE_'
ALLOC='malloc calloc realloc free'
objs=$(printf '%s\n' "$@")
[ -n "$objs" ] || { echo "check-fixed: no objects in $D"; exit 1; }
n=$(echo "$objs" | wc -l | tr -d ' ')
tmp=$D/.defined
# shellcheck disable=SC2086
"$NM" --defined-only $objs | awk 'NF == 3 { print $3 }' | sort -u > "$tmp" || exit 1
fail=0
for o in $objs; do
    rel=${o#"$D"/}
    bad=$("$NM" -u "$o" | awk '{ print $NF }' | sort -u | comm -23 - "$tmp" \
        | awk -v allow="$ALLOW" -v alloc="$ALLOC" -v api="$([ "$rel" = api/v90modem.o ] && echo 1 || echo 0)" '
        BEGIN { n = split(allow, a); for (i = 1; i <= n; i++) ok[a[i]] = 1
                n = split(alloc, m); for (i = 1; i <= n; i++) al[m[i]] = 1 }
        ($1 in ok) || (api && ($1 in al)) { next }
        { printf "%s%s", sep, $1; sep = " " }')
    [ -z "$bad" ] || { echo "check-fixed: $rel calls $bad"; fail=1; }
done
rm -f "$tmp"
[ $fail -eq 0 ] || { echo "check-fixed: FAILED (no float, no libm, allocation only in api/v90modem.o)"; exit 1; }
echo "fixed: $n objects, 0 float helpers, allocation only in api/v90modem.o"
