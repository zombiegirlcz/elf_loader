#!/system/bin/sh
# bench-nextgen.sh — srovnávací benchmark proot vs. loader vs. linuxsh chroot.
#
# Prompt 01 (docs/nextgen/01-mereni-benchmark.md).
#
# ARCHITEKTURA MĚŘENÍ (důležité):
#   Měříme JEDNU invokaci daného režimu na iteraci, spouštěnou z HOST-LEVEL
#   shellu (ne z guest shellu uvnitř režimu). Kdyby workload běžel uvnitř
#   guest bash, každý jeho potomek by v loader režimu šel znovu přes exec shim
#   (zanořený loader) a měřilo by se něco jiného než startovací cena.
#
#   Jediný platný host-level kanál je bionic zsh spuštěný MIMO DW:
#       LX_DW=0 $D/usr/bin/zsh <tento skript>
#   (DW by cesty přeložil do rootfs a spustil guest shell pod loaderem.)
#
# Režimy:
#   loader   — $L --ownall <HOST cesta> [args]
#   proot    — $PROOT -0 -r $R -w / -b /proc -b /dev <GUEST-abs cesta> [args]
#   linuxsh  — chroot $R <guest cesta> [args]   (jen root; jinak SKIP)
#
# Zátěže: launch (N× /bin/true), find /usr, git status, python import,
# node, make -j8 (cJSON).
#
# Každé měření BENCH_RUNS× (default 3), medián. BENCH_N = počet launch
# iterací (default 50).
set -u

D=/data/user/0/com.linux_core/files
R=${ROOTFS:-$D/nh/distro/parrot}
L=${L:-$D/usr/bin/elf_loader}
PROOT=${PROOT:-$D/usr/bin/proot}
RUNS=${BENCH_RUNS:-3}
NLAUNCH=${BENCH_N:-50}
WORK=${BENCH_WORK:-$R/tmp/bench.$$}

MODES="$*"
[ -z "$MODES" ] && MODES="loader proot linuxsh"

median_ms() {
    sort -n | awk '{a[NR]=$1} END{
        if (NR==0) {print "NA"; exit}
        if (NR%2) print a[(NR+1)/2]; else print int((a[NR/2]+a[NR/2+1])/2)
    }'
}

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# host cesta -> guest-abs (odříznout $R)
to_guest() { case "$1" in "$R"*) printf '%s' "${1#$R}" ;; *) printf '%s' "$1" ;; esac; }

# $1=mode, $2=HOST cesta cíle, zbytek args (args už v host/guest tvaru dle režimu)
launch() {
    local mode="$1"; shift
    local host="$1"; shift
    local guest; guest=$(to_guest "$host")
    [ -z "$guest" ] && guest=/
    case "$mode" in
        loader)  ROOTFS="$R" ELF_ROOTFS="$R" "$L" --ownall "$host" "$@" >/dev/null 2>&1 ;;
        proot)   "$PROOT" -0 -r "$R" -w / -b /proc -b /dev "$guest" "$@" >/dev/null 2>&1 ;;
        linuxsh) chroot "$R" "$guest" "$@" >/dev/null 2>&1 ;;
    esac
}

# N× launch true → ms/launch (medián z RUNS běhů celé smyčky)
bench_launch() {
    local mode="$1" vals="" t0 t1 i v
    local k=0
    while [ $k -lt "$RUNS" ]; do
        t0=$(now_ms)
        i=0; while [ $i -lt "$NLAUNCH" ]; do launch "$mode" "$R/bin/true"; i=$((i+1)); done
        t1=$(now_ms)
        v=$(( (t1 - t0) / NLAUNCH ))
        vals="$vals$v\n"
        k=$((k+1))
    done
    printf "$vals" | median_ms
}

# jeden příkaz → ms (medián z RUNS)
bench_cmd() {
    local mode="$1"; shift
    local vals="" t0 t1 v k=0
    while [ $k -lt "$RUNS" ]; do
        t0=$(now_ms)
        launch "$mode" "$@"
        t1=$(now_ms)
        vals="$vals$(( t1 - t0 ))\n"
        k=$((k+1))
    done
    printf "$vals" | median_ms
}

have_proot=0; [ -x "$PROOT" ] && have_proot=1
is_root=0; [ "$(id -u 2>/dev/null)" = "0" ] && is_root=1

printf 'bench-nextgen: L=%s\n' "$L"
printf '               R=%s\n' "$R"
printf '               proot=%s (have=%s)\n' "$PROOT" "$have_proot"
printf '               runs=%s launchN=%s root=%s\n' "$RUNS" "$NLAUNCH" "$is_root"
printf '               modes: %s\n\n' "$MODES"

CJSON="$WORK/cJSON"
prepare_cjson() {
    mkdir -p "$WORK" 2>/dev/null
    [ -d "$CJSON" ] && return 0
    ( cd "$WORK" && "$L" --ownall "$R/usr/bin/git" clone --depth 1 \
        https://github.com/DaveGamble/cJSON.git >/dev/null 2>&1 ) || return 1
    [ -d "$CJSON" ]
}

for mode in $MODES; do
    case "$mode" in
        proot)   [ "$have_proot" = 1 ] || { echo "SKIP proot (chybí $PROOT)"; continue; } ;;
        linuxsh) [ "$is_root" = 1 ]    || { echo "SKIP linuxsh (nejsme root)"; continue; } ;;
        loader)  [ -x "$L" ]           || { echo "SKIP loader (chybí $L)"; continue; } ;;
        *)       echo "neznámý režim: $mode"; continue ;;
    esac
    echo "── režim $mode ──"
    printf '%-8s %-8s %6s ms/launch\n' "$mode" launch "$(bench_launch "$mode")"
    printf '%-8s %-8s %6s ms\n' "$mode" find  "$(bench_cmd "$mode" "$R/usr/bin/find" /usr -type f)"
    printf '%-8s %-8s %6s ms\n' "$mode" git   "$(bench_cmd "$mode" "$R/usr/bin/git" -C /root/elf_loader status)"
    printf '%-8s %-8s %6s ms\n' "$mode" py    "$(bench_cmd "$mode" "$R/usr/bin/python3" -c 'import numpy, pandas')"
    printf '%-8s %-8s %6s ms\n' "$mode" node  "$(bench_cmd "$mode" "$R/usr/bin/node" -e 0)"
    if prepare_cjson; then
        printf '%-8s %-8s %6s ms\n' "$mode" make "$(bench_cmd "$mode" "$R/usr/bin/make" -C "$(to_guest "$CJSON")" -j8)"
    else
        printf '%-8s %-8s %6s ms\n' "$mode" make "SKIP(cJSON)"
    fi
    echo ""
done

rm -rf "$WORK" 2>/dev/null
echo hotovo.
