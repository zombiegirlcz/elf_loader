#!/bin/sh
# Slozi minimalni dpkg status DB z var/lib/dpkg/info/*.list a var/log/dpkg.log.
#
# K cemu: po havarii dpkg mezi zapisem status-new a prejmenovanim na status
# zustane rootfs bez databaze (status i status-old chybi, var/backups prazdne).
# Seznam nainstalovanych balicku ale prezije v info/*.list a verze ve dpkg.log.
#
# Vysledek jde VZDY do status.rebuilt, nikdy ne do status - nasazeni je rucni
# krok po kontrole (prompt 13, T5).
#
# Pouziti: tools/dpkg-status-rebuild.sh [--rootfs DIR] [--out FILE]
set -eu

ROOT="${ROOTFS:-}"
OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --rootfs) ROOT="$2"; shift 2 ;;
        --out)    OUT="$2";  shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "neznamy argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$ROOT" ] || { echo "chybi --rootfs (nebo \$ROOTFS)" >&2; exit 2; }

DPKG="$ROOT/var/lib/dpkg"
INFO="$DPKG/info"
LOG="$ROOT/var/log/dpkg.log"
[ -d "$INFO" ] || { echo "neni $INFO" >&2; exit 1; }
[ -n "$OUT" ] || OUT="$DPKG/status.rebuilt"

if [ -e "$DPKG/status" ]; then
    echo "POZOR: $DPKG/status existuje - tenhle nastroj je na obnovu, ne na prepis." >&2
    echo "Pokracuju jen do $OUT, status se nedotknu." >&2
fi

TMP="${TMPDIR:-/tmp}/dpkg-rebuild.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# 1) balicky, ktere maji seznam souboru = nainstalovane
#    jmeno souboru je "pkg.list" nebo "pkg:arch.list" (multi-arch same)
for f in "$INFO"/*.list; do
    [ -e "$f" ] || continue
    b=${f##*/}; b=${b%.list}
    case "$b" in
        *:*) echo "${b%%:*} ${b##*:} same" ;;
        *)   echo "$b - -" ;;
    esac
done | sort -u > "$TMP/pkgs"

# 2) verze z dpkg.log: "DATE TIME status installed pkg:arch version"
#    posledni zaznam vyhrava (log je chronologicky)
if [ -r "$LOG" ]; then
    awk '$3=="status" && $4=="installed" && NF>=6 {
             split($5, a, ":"); v[a[1]] = $6; ar[a[1]] = a[2]
         }
         END { for (p in v) printf "%s\t%s\t%s\n", p, v[p], ar[p] }' "$LOG" \
        | sort > "$TMP/vers"
else
    : > "$TMP/vers"
fi

# 3) sestaveni
awk -v versf="$TMP/vers" '
BEGIN {
    while ((getline line < versf) > 0) {
        split(line, f, "\t"); ver[f[1]] = f[2]; larch[f[1]] = f[3]
    }
}
{
    pkg = $1; arch = $2; ma = $3
    v = (pkg in ver) ? ver[pkg] : "0~rebuilt"
    if (pkg in ver) known++; else { unknown++; unk = unk " " pkg }
    if (arch == "-") arch = (pkg in larch && larch[pkg] != "") ? larch[pkg] : "arm64"
    printf "Package: %s\n", pkg
    printf "Status: install ok installed\n"
    printf "Priority: optional\n"
    printf "Section: unknown\n"
    printf "Architecture: %s\n", arch
    if (ma == "same") printf "Multi-Arch: same\n"
    printf "Version: %s\n", v
    printf "Description: placeholder z dpkg-status-rebuild.sh\n"
    printf " Zaznam slozeny z info/%s.list po ztrate status DB.\n", pkg
    printf " Metadata (zavislosti, conffiles, skutecny popis) chybi.\n"
    printf "\n"
    total++
}
END {
    printf "balicku:        %d\n", total              > "/dev/stderr"
    printf "s verzi z logu: %d\n", known+0            > "/dev/stderr"
    printf "bez verze:      %d (dostaly 0~rebuilt)\n", unknown+0 > "/dev/stderr"
    if (unknown+0 > 0 && unknown+0 <= 40)
        printf "  ->%s\n", unk                        > "/dev/stderr"
}' "$TMP/pkgs" > "$OUT"

echo "zapsano: $OUT ($(wc -c < "$OUT") B)"
echo
echo "Co chybi a proc to neni chyba skriptu:"
echo "  - Zavislosti (Depends/Pre-Depends) - nejsou v info/ ani v dpkg.log."
echo "  - Conffiles + jejich md5 - md5 nainstalovane podoby nikde neni."
echo "  - Skutecny Description, Section, Priority, Maintainer."
echo "  Doplnit jde jen z var/lib/apt/lists/*Packages* po 'apt update'."
echo
echo "Nasazeni az po kontrole:"
echo "  cp $OUT $DPKG/status   # a pak: dpkg -l | head, dpkg --audit"
