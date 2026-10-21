#!/bin/bash
# relsymlinks.sh — prevede absolutni symlinky v rootfs na KANONICKY RELATIVNI
# tvar (presne hloubka x "../" + cesta od guest korene), jako `symlinks -cr`.
#
# Proc: jadro vyhodnocuje absolutni symlink uvnitr $ROOTFS proti HOST koreni,
# takze /etc/alternatives/awk -> /usr/bin/mawk mimo chroot miri na host
# /usr/bin/mawk (neexistuje). Po konverzi to jadro resi samo a spravne, za
# nulovou cenu za behu. Invariant pak drzi shim_symlink/symlinkat v loaderu
# (absolutni cil zapise rovnou relativne) - viz src/relsym.h.
#
# Pro proot je konverze neutralni (relativni linky resi jadro stejne).
#
# Pouziti:
#   tools/relsymlinks.sh --dry-run            # jen vypis, nic nemeni
#   tools/relsymlinks.sh                      # konvertuje + zapise zurnal
#   tools/relsymlinks.sh --revert <zurnal>    # vrati puvodni (absolutni) cile
#   tools/relsymlinks.sh --check              # kolik absolutnich linku zbyva
# Volby: --root <cesta> (default $ROOTFS / $R), --log <soubor>, --quiet
set -euo pipefail

ROOT=${ROOTFS:-${R:-}}
MODE=convert
LOG=""
QUIET=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) MODE=dry ;;
        --check)   MODE=check ;;
        --revert)  MODE=revert; LOG=${2:?--revert potrebuje zurnal}; shift ;;
        --root)    ROOT=${2:?}; shift ;;
        --log)     LOG=${2:?}; shift ;;
        --quiet)   QUIET=1 ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "neznamy argument: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { [ "$QUIET" = 1 ] || printf '%s\n' "$*"; }

# ── revert ─────────────────────────────────────────────────────────────────
if [ "$MODE" = revert ]; then
    [ -f "$LOG" ] || { echo "zurnal neexistuje: $LOG" >&2; exit 2; }
    n=0
    while IFS=$'\t' read -r link orig _rel; do
        [ -n "${link:-}" ] || continue
        [ -L "$link" ] || { echo "preskoceno (uz neni symlink): $link" >&2; continue; }
        ln -sfn "$orig" "$link"
        n=$((n + 1))
    done < "$LOG"
    say "vraceno $n linku podle $LOG"
    exit 0
fi

[ -n "$ROOT" ] || { echo "chybi --root (nebo \$ROOTFS/\$R)" >&2; exit 2; }
# ROOT drzime BEZ koncoveho lomitka; "/" (beh uvnitr rootfs - v prootu nebo
# pod loaderem, kde uz JE rootfs korenem) tak znamena prazdny prefix.
# POZOR: `find -printf` je GNU; host toybox ho nema, takze nastroj patri
# spoustet v guestu (tam je rootfs bud "/" nebo plna cesta).
ROOT=${ROOT%/}
FIND_ROOT=${ROOT:-/}
[ -d "$FIND_ROOT" ] || { echo "rootfs neexistuje: $FIND_ROOT" >&2; exit 2; }

if [ "$MODE" = convert ] && [ -z "$LOG" ]; then
    LOG="${LX_LOG:-/tmp}/relsymlinks-$(date +%Y%m%d_%H%M%S).tsv"
fi

# Cile, ktere MUSI zustat absolutni: host mountpointy a virtualni FS
# (stejny seznam jako shim_excluded() v src/main.c).
is_host_target() {
    case "$1" in
        /proc|/proc/*|/dev|/dev/*|/sys|/sys/*|/system|/system/*|/data|/data/*) return 0 ;;
        /apex|/apex/*|/linkerconfig|/linkerconfig/*|/metadata|/metadata/*) return 0 ;;
        /sdcard|/sdcard/*|/storage|/storage/*|/vendor|/vendor/*|/odm|/odm/*) return 0 ;;
        /product|/product/*|/persist|/persist/*|/cache|/cache/*) return 0 ;;
        /config|/config/*|/debug_ramdisk|/debug_ramdisk/*) return 0 ;;
    esac
    return 1
}

# Adresare v rootfs, ktere vubec neprochazime (virtualni FS + host bind
# mounty, ktere se v rootfs objevuji).
PRUNE=(proc sys dev data system apex vendor linkerconfig storage sdcard)
find_args=()
for d in "${PRUNE[@]}"; do
    find_args+=(-path "${ROOT}/$d" -prune -o)
done

converted=0; skipped_host=0; skipped_rootfs=0; total_abs=0
[ "$MODE" = convert ] && : > "$LOG"

# Cteni pres `find -printf '%p\t%l'`: vypise link i jeho cil bez forku.
# (Puvodni `readlink` na kazdy link znamenal fork+exec na desitky tisic
# symlinku = desitky minut pod loaderem.)
while IFS=$'\t' read -r link target; do
    [ -n "${link:-}" ] || continue
    case "$target" in /*) ;; *) continue ;; esac   # relativni = uz hotovo
    total_abs=$((total_abs + 1))

    # cil uz hostove-absolutni pod rootfs (napr. proot link2symlink .l2s)
    if [ -n "$ROOT" ]; then
        case "$target" in
            "$ROOT"/*|"$ROOT") skipped_rootfs=$((skipped_rootfs + 1)); continue ;;
        esac
    fi
    if is_host_target "$target"; then
        skipped_host=$((skipped_host + 1))
        continue
    fi

    # hloubka = pocet komponent adresare linku, merena od guest korene
    guest=${link#"$ROOT"}
    dir=${guest%/*}
    depth=0
    IFS=/ read -r -a comps <<< "${dir#/}"
    for c in "${comps[@]:-}"; do [ -n "$c" ] && depth=$((depth + 1)); done

    up=""
    i=0
    while [ "$i" -lt "$depth" ]; do up="$up../"; i=$((i + 1)); done
    rel="$up${target#/}"
    [ -n "$rel" ] || rel="."
    rel=${rel%/}
    [ -n "$rel" ] || rel="."

    case "$MODE" in
        check) ;;                      # --check jen pocita, nevypisuje
        dry) say "$guest: $target -> $rel" ;;
        convert)
            ln -sfn "$rel" "$link"
            printf '%s\t%s\t%s\n' "$link" "$target" "$rel" >> "$LOG"
            [ "$QUIET" = 1 ] || say "$guest: $target -> $rel"
            ;;
    esac
    converted=$((converted + 1))
done < <(find "$FIND_ROOT" "${find_args[@]}" -type l -printf '%p\t%l\n' 2>/dev/null)

# souhrn se tiskne vzdy (i s --quiet)
say() { printf '%s\n' "$*"; }
case "$MODE" in
    check) say "absolutnich linku: $total_abs (prevoditelnych $converted, host cilu $skipped_host, pod-rootfs cilu $skipped_rootfs)" ;;
    dry)   say "--dry-run: prevedlo by se $converted z $total_abs absolutnich (host cilu $skipped_host, pod-rootfs cilu $skipped_rootfs)" ;;
    convert) say "prevedeno $converted z $total_abs absolutnich (host cilu $skipped_host, pod-rootfs cilu $skipped_rootfs); zurnal: $LOG" ;;
esac
exit 0
