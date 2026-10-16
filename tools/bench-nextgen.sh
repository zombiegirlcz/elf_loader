#!/bin/bash
# bench-nextgen.sh — srovnávací benchmark proot vs. loader vs. linuxsh chroot.
#
# Prompt 01 (docs/nextgen/01-mereni-benchmark.md): získá tvrdá čísla, podle
# kterých se rozhoduje o dalších promptech 02/10/11.
#
# Režimy:
#   proot    — /data/user/0/com.linux_core/files/usr/bin/proot se stejnými
#              volbami jako appka (PROOT_L2S_DIR, ROOTFS). Zjisti z env; pokud
#              proot chybí, režim se přeskočí.
#   loader   — $L --ownall  (přímé volání loaderu, ne přes shim)
#   linuxsh  — nativní chroot (jen pokud jsme root; jinak SKIP)
#
# Zátěže (kapitola 9): 1000× /bin/true, configure malého projektu, make -j8
# (cJSON), find /usr -type f | wc -l, git status na velkém repu,
# python3 -c 'import numpy, pandas', java -version, gh --version, node -e 0,
# apt-get install --reinstall malého balíku (jen kde dává smysl).
#
# Každé měření 3× (BENCH_RUNS přepíše), medián. Výstup: tabulka zátěž × režim.
#
# Použití (na zařízení, ideálně přes ashell, aby čísla nebyla zkreslená prootem):
#   ashell -c "ROOTFS=$R ELF_ROOTFS=$R L=$L bash /root/elf_loader/tools/bench-nextgen.sh"
#   BENCH_RUNS=5 ... bench-nextgen.sh loader
#   bench-nextgen.sh proot loader linuxsh
set -u

D=/data/user/0/com.linux_core/files
L=${L:-$D/usr/bin/elf_loader}
R=${ROOTFS:-$D/nh/distro/parrot}
PROOT=${PROOT:-$D/usr/bin/proot}
RUNS=${BENCH_RUNS:-3}
# Domácí adresář pro dočasné projekty (cJSON build, configure).
WORK=${BENCH_WORK:-$R/tmp/bench.$$}

# Režimy k běhu (z argv, default všechny tři).
MODES=("$@")
[ ${#MODES[@]} -eq 0 ] && MODES=(loader proot linuxsh)

# ── medián N čísel na stdin (ms) ────────────────────────────────────────────
median_ms() {
    sort -n | awk '{a[NR]=$1} END{
        if (NR==0) {print "NA"; exit}
        if (NR%2) print a[(NR+1)/2]; else print int((a[NR/2]+a[NR/2+1])/2)
    }'
}

# ── měření jednoho běhu v daném režimu; výstup = ms ─────────────────────────
# $1 = režim, zbytek = příkaz (shell řádka)
run_once() {
    local mode="$1"; shift
    local cmd="$*"
    local t0 t1
    t0=$(date +%s%N 2>/dev/null || date +%s000000000)
    case "$mode" in
        loader)
            # ROOTFS nastaveno, ať guest vidí cestu; příkaz jde do guest shellu,
            # aby pipy/&& fungovaly uvnitř guestu (jako `lx -c`).
            ROOTFS="$R" ELF_ROOTFS="$R" "$L" --ownall "$R/bin/bash" -c "$cmd" >/dev/null 2>&1
            ;;
        proot)
            # Stejné volby jako appka: -0 (root uid), -r rootfs, -w /, -b /dev.
            # PROOT_NO_SECCOMP se respektuje z env (ověření akcelerace).
            "$PROOT" -0 -r "$R" -w / -b /dev -b /proc "$R/bin/bash" -c "$cmd" >/dev/null 2>&1
            ;;
        linuxsh)
            # Nativní chroot (jen root): unshare -m + mount --make-rprivate, pak
            # chroot. Bez unshare by propagace mountů leakovala do global NS.
            unshare -m sh -c 'mount --make-rprivate / 2>/dev/null; chroot '"$R"' /bin/bash -c '"'"'"$cmd"'"'"'' >/dev/null 2>&1
            ;;
        *)
            return 1
            ;;
    esac
    t1=$(date +%s%N 2>/dev/null || date +%s000000000)
    echo $(( (t1 - t0) / 1000000 ))
}

# ── spustí zátěž N× v režimu, vytiskne medián ───────────────────────────────
bench() {
    local mode="$1" label="$2"; shift 2
    local cmd="$*"
    local vals=() v
    for _ in $(seq 1 "$RUNS"); do
        v=$(run_once "$mode" "$cmd")
        vals+=("$v")
    done
    local med
    med=$(printf '%s\n' "${vals[@]}" | median_ms)
    printf '%-14s %-9s %6s ms   (%s)\n' "$mode" "$label" "$med" "${vals[*]}"
}

# ── detekce režimů ──────────────────────────────────────────────────────────
have_proot=0
[ -x "$PROOT" ] && have_proot=1
is_root=0
[ "$(id -u 2>/dev/null)" = "0" ] && is_root=1

echo "bench-nextgen: L=$L"
echo "               R=$R"
echo "               proot=$PROOT (have=$have_proot)"
echo "               runs=$RUNS  root=$is_root"
echo "               modes: ${MODES[*]}"
echo ""

# ── příprava cJSON (jednorázově, jen pro make -j8) ──────────────────────────
CJSON="$WORK/cJSON"
prepare_cjson() {
    mkdir -p "$WORK" 2>/dev/null
    [ -d "$CJSON" ] && return 0
    # Preferuj git clone uvnitř guestu; když není síť, zkus lokální kopii.
    ( cd "$WORK" && "$L" --ownall "$R/usr/bin/git" clone --depth 1 \
        https://github.com/DaveGamble/cJSON.git >/dev/null 2>&1 ) || return 1
    [ -d "$CJSON" ] && return 0
    return 1
}

# ── zátěže ──────────────────────────────────────────────────────────────────
# Popis + příkaz (guest shell řádka). Prázdný příkaz = přeskoč.
declare -A LOAD_CMD LOAD_DESC
LOAD_CMD[true]='for i in $(seq 1 1000); do /bin/true; done'
LOAD_DESC[true]="1000x /bin/true"

LOAD_CMD[find]='find /usr -type f 2>/dev/null | wc -l'
LOAD_DESC[find]="find /usr -type f | wc -l"

LOAD_CMD[git]='cd /root/elf_loader && git status >/dev/null 2>&1'
LOAD_DESC[git]="git status (velký repo)"

LOAD_CMD[py]='python3 -c "import numpy, pandas" 2>/dev/null'
LOAD_DESC[py]="python3 import numpy,pandas"

LOAD_CMD[java]='command -v java >/dev/null 2>&1 && java -version 2>&1 | head -1 || false'
LOAD_DESC[java]="java -version (sdkman)"

LOAD_CMD[gh]='command -v gh >/dev/null 2>&1 && gh --version >/dev/null 2>&1 || false'
LOAD_DESC[gh]="gh --version"

LOAD_CMD[node]='node -e 0 2>/dev/null'
LOAD_DESC[node]="node -e 0"

LOAD_CMD[make]="make -C $CJSON -j8 >/dev/null 2>&1"
LOAD_DESC[make]="make -j8 (cJSON)"

LOAD_CMD[cfg]='true'   # configure placeholder, doplní se když cJSON existuje
LOAD_DESC[cfg]="configure malého projektu"

# ── hlavní smyčka ───────────────────────────────────────────────────────────
for mode in "${MODES[@]}"; do
    case "$mode" in
        proot)    [ "$have_proot" = 1 ] || { echo "SKIP proot (chybí $PROOT)"; continue; } ;;
        linuxsh)  [ "$is_root" = 1 ]    || { echo "SKIP linuxsh (nejsme root)"; continue; } ;;
        loader)   [ -x "$L" ]           || { echo "SKIP loader (chybí $L)"; continue; } ;;
        *)        echo "neznámý režim: $mode"; continue ;;
    esac
    echo "── režim $mode ──"
    for key in true find git py java gh node; do
        bench "$mode" "$key" "${LOAD_CMD[$key]}"
    done
    # make -j8 jen když je cJSON připravený
    if prepare_cjson; then
        LOAD_CMD[make]="make -C ${CJSON#$R} -j8 >/dev/null 2>&1"
        bench "$mode" "make" "${LOAD_CMD[make]}"
    else
        echo "$(printf '%-14s %-9s %6s' "$mode" make SKIP)   (cJSON nedostupný)"
    fi
    echo ""
done

rm -rf "$WORK" 2>/dev/null
echo "hotovo."