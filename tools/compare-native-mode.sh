#!/system/bin/sh
# Porovna test-all.sh v NATIVNIM modu (prompt 12) proti defaultnimu own-loadu.
#
# Harness bere jen jednu kategorii na beh (`case "$1"` na konci test-all.sh,
# usage v hlavicce slibuje vic, ale dispatcher to neumi), takze je pousti po
# jedne a scita vysledky sam.
#
# Harness bezi v OBOU pripadech pod loaderem; v nativnim modu se prepina jen
# `L`, tedy to, cim se spousti kazdy jednotlivy test. Vnoreny tracer by
# kolidoval s vnejsim - viz tools/lxnative-ownall.sh.
#
# Pouziti: tools/compare-native-mode.sh <guest-cesta-k-test-all.sh> [kategorie…]
set -u

FILES=/data/user/0/com.linux_core/files
R="$FILES/nh/distro/parrot"
L="$FILES/usr/bin/elf_loader"
SHIM="${LXN_SHIM:-$FILES/tmp/p12/lxnative-ownall.sh}"
TA="${1:?chybi cesta k test-all.sh (guestova, napr. /root/elf_loader/test-all.sh)}"
shift

CATS=${*:-"basic text files system datetime compression math diff archive shell python pathops fstat symlink selfexe argv0 extended extra networking nss"}
OUT="$FILES/tmp/p12/cmp"
mkdir -p "$OUT"

one() {
    mode="$1"; cat="$2"
    if [ "$mode" = native ]; then
        env_l="LX_NATIVE_MODE=1 LXN_SHIM=$SHIM"
    else
        env_l="L=$L"
    fi
    timeout 420 ashell -c "ROOTFS=$R ELF_ROOTFS=$R HOME=$FILES $env_l $L --ownall $R/bin/bash $TA $cat" \
        > "$OUT/$mode.$cat.txt" 2>&1
    p=$(sed -n 's/^PASS: \([0-9]*\)$/\1/p' "$OUT/$mode.$cat.txt" | tail -1)
    f=$(sed -n 's/^FAIL: \([0-9]*\)$/\1/p' "$OUT/$mode.$cat.txt" | tail -1)
    s=$(sed -n 's/^SKIP: \([0-9]*\)$/\1/p' "$OUT/$mode.$cat.txt" | tail -1)
    echo "${p:-?} ${f:-?} ${s:-?}"
}

printf '%-14s %-18s %-18s\n' kategorie 'own-load P/F/S' 'nativni P/F/S'
tp=0; tf=0; ts=0; np=0; nf=0; ns=0
for c in $CATS; do
    b=$(one base "$c")
    n=$(one native "$c")
    printf '%-14s %-18s %-18s\n' "$c" "$(echo "$b" | tr ' ' '/')" "$(echo "$n" | tr ' ' '/')"
    num() { case "$1" in ''|*[!0-9]*) echo 0 ;; *) echo "$1" ;; esac; }
    set -- $b
    tp=$((tp + $(num "${1:-}"))); tf=$((tf + $(num "${2:-}"))); ts=$((ts + $(num "${3:-}")))
    set -- $n
    np=$((np + $(num "${1:-}"))); nf=$((nf + $(num "${2:-}"))); ns=$((ns + $(num "${3:-}")))
done
echo
printf 'CELKEM         %-18s %-18s\n' "$tp/$tf/$ts" "$np/$nf/$ns"
echo "logy: $OUT/{base,native}.<kategorie>.txt"
