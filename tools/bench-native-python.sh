#!/system/bin/sh
# Cena rtld-auditu na symbolove tezkem programu + RSS (prompt 12).
#
# `true` naváže par symbolu, takze cenu auditu neukaze. Python naváže stovky
# a rtld-audit vypina rychlou PLT cestu (`_dl_profile_fixup` misto
# `_dl_fixup`), takze tady se projevi. Referencni cislo z AGENTS.md:
# `python3 -c 0` pod loaderem 102,8 ms.
set -u

FILES=/data/user/0/com.linux_core/files
R="$FILES/nh/distro/${1:-parrot}"
D="$FILES/tmp/p12"
LXN="${LXN:-$D/lxnative}"
AUD="${AUD:-$D/liblx-audit.so}"
L="$FILES/usr/bin/elf_loader"
PR="$FILES/usr/bin/proot"
LD="$R/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
LP="$R/lib/aarch64-linux-gnu:$R/usr/lib/aarch64-linux-gnu"
N=${N:-5}
export LX_ROOTFS="$R"

run() {
    name="$1"; shift
    t0=$(date +%s%N)
    i=0
    while [ "$i" -lt "$N" ]; do
        "$@" >/dev/null 2>&1
        i=$((i + 1))
    done
    t1=$(date +%s%N)
    echo "$t0 $t1 $N $name" |
        awk '{ d = ($2 - $1) / 1000000
               printf "%-18s %9.1f ms celkem %8.1f ms/iter\n", $4, d, d / $3 }'
}

echo "=== cas, python3 -c 0, N=$N"
for _round in 1 2; do
    echo "--- kolo $_round"
    run nativni-ldso  "$LXN" "$LD" --library-path "$LP" "$R/usr/bin/python3" -c 0
    run nativni+audit "$LXN" "$LD" --library-path "$LP" --audit "$AUD" "$R/usr/bin/python3" -c 0
    run loader-ownall env ROOTFS="$R" ELF_ROOTFS="$R" "$L" --ownall "$R/usr/bin/python3" -c 0
    run proot         "$PR" -r "$R" /usr/bin/python3 -c 0
done

echo "=== RSS (VmHWM) tehoz procesu"
rss() {
    name="$1"; shift
    v=$("$@" 2>/dev/null | grep -E '^VmHWM|^VmSize' | tr -s ' ' | tr '\n' ' ')
    printf '%-18s %s\n' "$name" "$v"
}
PY='import sys; print(open("/proc/self/status").read())'
rss nativni-ldso  "$LXN" "$LD" --library-path "$LP" "$R/usr/bin/python3" -c "$PY"
rss nativni+audit "$LXN" "$LD" --library-path "$LP" --audit "$AUD" "$R/usr/bin/python3" -c "$PY"
rss loader-ownall env ROOTFS="$R" ELF_ROOTFS="$R" "$L" --ownall "$R/usr/bin/python3" -c "$PY"
rss proot         "$PR" -r "$R" /usr/bin/python3 -c "$PY"
