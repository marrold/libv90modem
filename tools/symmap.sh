#!/bin/sh
# tools/symmap.sh NM OBJECT
# The internal symbols of the static library's one relocatable object, as
# an objcopy --redefine-syms map: each global symbol the library defines,
# other than the public API (v90m_* without a second underscore) and the
# compiler's own (__*, .*), is renamed v90m__i_<name>: the library's own
# v90m__* accessors too (v90m__ec becomes v90m__i_v90m__ec). They stay global, which every target's
# relocations allow (making them local does not: i686's PC thunks, MIPS's
# GOT16), and cannot clash with a program's names. Where C names carry a
# leading underscore (32-bit Windows) it stays in front.
set -e
nm=$1; obj=$2
syms=$($nm -g --defined-only "$obj" | awk 'NF >= 3 { print $3 }' | sort -u)
u=""; echo "$syms" | grep -qx '_v90m_init' && u="_"
echo "$syms" | awk -v u="$u" '
    { n = $0 }
    u != "" { if (substr(n, 1, 1) != "_") next; n = substr(n, 2) }
    n ~ /^v90m__i_/ { next }
    n ~ /^v90m_/ && n !~ /^v90m__/ { next }
    n ~ /^_/ || n ~ /^\./ || n ~ /^\$/ { next }
    { print u n " " u "v90m__i_" n }'
