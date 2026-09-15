#!/bin/sh
# gbsh-whitelist-merge.sh — naplni whitelist zpetnou vazbou z white.log.
#
# Loader (--ownall) loguje do $ROOTFS/root/elf_loader/white.log:
#   [exec] path=/bin/echo resolved=...        -> binarka prosla pres loader
#   [skip] path=/usr/bin/nmap reason=not-in-whitelist  -> NENI ve whitelistu
#   [fail] path=... resolved=... rc=127       -> redirect selhal (spatna cesta)
#
# Tento skript vezme vsechny [skip] cesty, prevede na basename a doplni je do
# whitelist.txt (bez duplikatu). Tim se "naplni whitelist" z realneho provozu:
# spustis gbsh -dw, nechas protekly provoz a pak spustis tento merge.
#
# Pouziti:
#   ROOTFS=/cesta/k/rootfs sh tools/gbsh-whitelist-merge.sh [--dry-run]
set -e

: "${ROOTFS:?ROOTFS musi byt nastaven}"
LOG="$ROOTFS/root/elf_loader/white.log"
WL="${ELF_LOADER_WHITELIST:-$ROOTFS/root/elf_loader/whitelist.txt}"
DRY=0
[ "$1" = "--dry-run" ] && DRY=1

[ -f "$LOG" ] || { echo "gbsh-whitelist-merge: white.log nenalezen ($LOG)"; exit 0; }
[ -f "$WL" ] || { mkdir -p "$(dirname "$WL")"; : > "$WL"; }

# 1) nova jmena z [skip] radku (basename z "path=...")
new=$(mktemp)
grep '^\[skip\]' "$LOG" 2>/dev/null | sed -n 's/.*path=\([^ ]*\).*/\1/p' \
    | while IFS= read -r p; do
        b=$(basename "$p" 2>/dev/null) || b="$p"
        [ -n "$b" ] && echo "$b"
      done | sort -u > "$new"

# 2) co uz ve whitelistu je
have=$(mktemp)
sed 's/#.*//' "$WL" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | grep -v '^$' | sort -u > "$have"

# 3) rozdil
add=$(mktemp)
comm -23 "$new" "$have" > "$add"
cnt=$(wc -l < "$add" | tr -d ' ')
total_new=$(wc -l < "$new" | tr -d ' ')

echo "gbsh-whitelist-merge: [skip] jmen celkem=$total_new, novych k pridani=$cnt"
if [ "$cnt" -gt 0 ]; then
    if [ "$DRY" = 1 ]; then
        echo "--- (dry-run) nova jmena ---"
        cat "$add"
    else
        {
            echo ""
            echo "# --- doplneno gbsh-whitelist-merge.sh $(date '+%Y-%m-%d %H:%M:%S') ---"
            cat "$add"
        } >> "$WL"
        echo "gbsh-whitelist-merge: pridano $cnt jmen do $WL"
    fi
fi
rm -f "$new" "$have" "$add"