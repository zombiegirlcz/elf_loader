#!/system/bin/sh
# Uklid osirelych backing souboru v $ROOTFS/.l2s (link2symlink, src/l2s.h).
#
# K cemu: l2s_unlink se do prompt 13 (T2) nevolal pri rename pres l2s cil ->
# backing soubor zustal navzdy. Zaznamenano 29.10.2026: .l2s melo 9,39 GiB,
# z toho 67 187 osirelych souboru = 3,58 GiB (40 %).
#
# DULEZITE (AGENTS.md "Bezpecnost - destruktivni testy", past c. 11):
#   - tenhle skript bezi VZDY HOSTEM (ashell, /system/bin/*), NIKDY pod
#     loaderem - `rm -rf` pod loaderem mize ZDROJ bindu, ne mountpoint.
#   - analyza symlinku (readlink/find -printf %l) musi byt hostovymi
#     nastroji - proot zkracuje cile na 14 B.
#   - bezet POD ROOTEM (su 0 z ashellu), ne pod uid appky: pod appkou jsou
#     tisice symlinku do .l2s neciteine (EACCES) a jejich cile vypadnou
#     z mnoziny zivych odkazu -> falesne osireni.
#
# Algoritmus je DVOUSTUPNOVY (jednostupnovy dava falesne osirele - viz
# docs/nextgen/13-uklid-a-odolnost.md):
#
#   cesta v rootfs (symlink) -> .l2s/.l2s.<jmeno><NNNN> (symlink, "mid")
#                             -> .l2s/.l2s.<jmeno><NNNN>.<nlink> (data)
#
#   1. data      = vsechny REGULARNI soubory v $L (backing data)
#   2. mid       = vsechny SYMLINKY v $L (mezistupne), cil -> basename
#   3. rootrefs  = cile vsech symlinku v ROOTFS MIMO .l2s samotne, basename
#   4. mid_live  = mid prusecik rootrefs; mid_dead = mid \ rootrefs
#   5. live      = cile mid_live + rootrefs (rootfs smi mirit i primo na data)
#   6. orphans   = data \ live
#
# Smazat (resp. karanténovat) lze orphans + mid_dead.
#
# Pouziti:
#   tools/l2s-gc.sh --rootfs DIR [--dry-run|--delete] [--quarantine DIR]
#                   [--keep GLOB]...
#
# --dry-run je DEFAULT. Mazani jen s explicitnim --delete.
# --keep GLOB (lze opakovat) pridava denylist vzor (shell glob na basename
# backing souboru/mezistupne) - povinne predvyplneno *status*, *dpkg*,
# *.l2s.diversions* (viz docs/nextgen/13-uklid-a-odolnost.md, "Co se stalo
# s dpkg": posledni kopie databaze po havarii zila jako osireny
# .l2s.status0001.0001 a referencni kriterium ji neumi odlisit od uniklych
# tmp souboru - status/dpkg jmena se tedy nikdy nemazou automaticky).

set -eu

ROOT=""
MODE="dry-run"
QUARANTINE=""
KEEP_PATTERNS="*status* *dpkg* *.l2s.diversions*"

while [ $# -gt 0 ]; do
    case "$1" in
        --rootfs)     ROOT="$2"; shift 2 ;;
        --dry-run)    MODE="dry-run"; shift ;;
        --delete)     MODE="delete"; shift ;;
        --quarantine) QUARANTINE="$2"; MODE="quarantine"; shift 2 ;;
        --keep)       KEEP_PATTERNS="$KEEP_PATTERNS $2"; shift 2 ;;
        -h|--help)    sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "neznamy argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$ROOT" ] || { echo "chybi --rootfs" >&2; exit 2; }
[ -d "$ROOT" ] || { echo "neni adresar: $ROOT" >&2; exit 2; }
L="$ROOT/.l2s"
[ -d "$L" ] || { echo "neni $L - nic k uklidu" >&2; exit 0; }

if [ "$(id -u 2>/dev/null || echo 1)" != "0" ]; then
    echo "POZOR: nebezi jako root - pod appkou jsou symlinky do .l2s" >&2
    echo "necitelne (EACCES) a jejich cile vypadnou z mnoziny zivych." >&2
    echo "Pouzij 'su 0 -c ...' z ashellu, viz AGENTS.md." >&2
fi

if [ -n "$QUARANTINE" ]; then
    mkdir -p "$QUARANTINE"
fi

TMP="${TMPDIR:-/data/local/tmp}/l2s-gc.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# past c. 15: toybox find nezna -printf '%h' (jen %f/%l/%s/%p/%Z); nepouzivame.

# 1) data = regularni soubory v $L (basename, velikost v bajtech - jen pro report)
find "$L" -maxdepth 1 -type f -printf '%f\t%s\n' > "$TMP/data_raw" 2>/dev/null
cut -f1 "$TMP/data_raw" | sort -u > "$TMP/data_names"

# 2) mid = symlinky v $L (basename -> cil basename)
#    past c. 11: ciste proot %l davaji zkracene cile (14 B) - tenhle skript
#    bezi hostem (ashell), takze host toybox find dava spravnou delku.
find "$L" -maxdepth 1 -type l -printf '%f\t%l\n' > "$TMP/mid_raw" 2>/dev/null
awk -F'\t' '{n=split($2,a,"/"); print $1 "\t" a[n]}' "$TMP/mid_raw" > "$TMP/mid_map"
cut -f1 "$TMP/mid_map" | sort -u > "$TMP/mid_names"

# 3) rootrefs = cile vsech symlinku v cele rootfs MIMO .l2s, basename
#    (past c. 11 opet: musi byt hostovy find, jinak vychazi nesmysl)
find "$ROOT" -xdev -type l -printf '%p\t%l\n' 2>/dev/null \
    | grep -v $'\t.*/\\.l2s/' \
    | grep -v '^'"$L"'/' \
    | awk -F'\t' '{n=split($2,a,"/"); print a[n]}' \
    | sort -u > "$TMP/rootrefs"

# 4) mid_live / mid_dead
comm -12 "$TMP/mid_names" "$TMP/rootrefs" > "$TMP/mid_live_names"
comm -23 "$TMP/mid_names" "$TMP/rootrefs" > "$TMP/mid_dead_names"

# cile zivych mezistupnu (co mid_live_names ukazuje na data) + primo rootrefs
awk -F'\t' 'NR==FNR{live[$1]=1; next} ($1 in live){print $2}' \
    "$TMP/mid_live_names" "$TMP/mid_map" > "$TMP/live_via_mid"
cat "$TMP/live_via_mid" "$TMP/rootrefs" | sort -u > "$TMP/live_names"

# 6) orphans = data \ live
comm -23 "$TMP/data_names" "$TMP/live_names" > "$TMP/orphan_names"

# denylist filtr (keep patterny) na orphans i mid_dead
filter_keep() {
    in="$1"; out="$2"
    : > "$out"
    while IFS= read -r n; do
        [ -n "$n" ] || continue
        skip=0
        for pat in $KEEP_PATTERNS; do
            case "$n" in $pat) skip=1; break ;; esac
        done
        [ "$skip" = 1 ] || echo "$n" >> "$out"
    done < "$in"
}
filter_keep "$TMP/orphan_names" "$TMP/orphan_del"
filter_keep "$TMP/mid_dead_names" "$TMP/mid_dead_del"

n_data=$(wc -l < "$TMP/data_names")
n_mid=$(wc -l < "$TMP/mid_names")
n_mid_live=$(wc -l < "$TMP/mid_live_names")
n_mid_dead=$(wc -l < "$TMP/mid_dead_names")
n_refs=$(wc -l < "$TMP/rootrefs")
n_orphan=$(wc -l < "$TMP/orphan_del")
n_mid_dead_del=$(wc -l < "$TMP/mid_dead_del")

# velikost orphans v bajtech (past c. 12: pocitej v MiB, host sh/test je 32b)
awk -F'\t' 'NR==FNR{keep[$1]=1; next} ($1 in keep){s+=$2} END{printf "%d", s+0}' \
    "$TMP/orphan_del" "$TMP/data_raw" > "$TMP/orphan_bytes"
orphan_bytes=$(cat "$TMP/orphan_bytes")
orphan_mib=$(awk -v b="$orphan_bytes" 'BEGIN{printf "%.1f", b/1048576}')

echo "l2s-gc: $L"
echo "  data soubory:        $n_data"
echo "  mezistupne:          $n_mid  (zive: $n_mid_live, mrtve: $n_mid_dead)"
echo "  odkazy mimo .l2s:    $n_refs"
echo "  osirele (k uklidu):  $n_orphan data + $n_mid_dead_del mid  (~${orphan_mib} MiB dat)"
echo "  mod:                 $MODE"

if [ "$MODE" = "dry-run" ]; then
    echo "(--dry-run, nic se nemaze; --delete nebo --quarantine DIR pro akci)"
    exit 0
fi

del_one() {
    f="$L/$1"
    [ -e "$f" ] || return 0
    if [ "$MODE" = "quarantine" ]; then
        mv -f "$f" "$QUARANTINE/" 2>/dev/null || true
    else
        rm -f "$f"
    fi
}

while IFS= read -r n; do [ -n "$n" ] && del_one "$n"; done < "$TMP/orphan_del"
while IFS= read -r n; do [ -n "$n" ] && del_one "$n"; done < "$TMP/mid_dead_del"

echo "hotovo."
