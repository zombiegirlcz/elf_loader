#!/system/bin/sh
# A/B mereni exec ceny nativniho ld.so proti loaderu, prootu a holemu bionicu.
#
# Hostovy skript - pousti se pres `ashell -c`, nikdy pod loaderem (past c. 5:
# `su -c` jde pres proot a maskuje path bugy). Pinovani CPU meni cisla
# o desitky %, proto se kazdy mod meri ve dvou poradich (AGENTS.md, Metodika).
#
# Pouziti:  N=40 tools/bench-native-ldso.sh [parrot|kali]
set -u

FILES=/data/user/0/com.linux_core/files
R="$FILES/nh/distro/${1:-parrot}"
LXN="${LXN:-$FILES/tmp/p12/lxnative}"
L="$FILES/usr/bin/elf_loader"
PR="$FILES/usr/bin/proot"
LD="$R/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
LP="$R/lib/aarch64-linux-gnu:$R/usr/lib/aarch64-linux-gnu"
N=${N:-40}

run() {
    name="$1"; shift
    t0=$(date +%s%N)
    i=0
    while [ "$i" -lt "$N" ]; do
        "$@" >/dev/null 2>&1
        i=$((i + 1))
    done
    t1=$(date +%s%N)
    # Celkem v ms a na iteraci v us. Pocita awk - hostovy `test`/`sh` je
    # 32bitovy a nanosekundy mu pretecou (past c. 12).
    echo "$t0 $t1 $N $name" |
        awk '{ d = ($2 - $1) / 1000000
               printf "%-18s %9.1f ms celkem %8.0f us/iter\n", $4, d, d * 1000 / $3 }'
}

AUD="${AUD:-$FILES/tmp/p12/liblx-audit.so}"
export LX_ROOTFS="$R"

echo "rootfs=$R  N=$N"
echo "--- poradi 1"
run bionic         /system/bin/toybox true
run loader-ownall  env ROOTFS="$R" ELF_ROOTFS="$R" "$L" --ownall "$R/bin/true"
run proot          "$PR" -r "$R" /bin/true
run nativni-ldso   "$LXN" "$LD" --library-path "$LP" "$R/bin/true"
run nativni+audit  "$LXN" "$LD" --library-path "$LP" --audit "$AUD" "$R/bin/true"
echo "--- poradi 2 (obracene)"
run nativni+audit  "$LXN" "$LD" --library-path "$LP" --audit "$AUD" "$R/bin/true"
run nativni-ldso   "$LXN" "$LD" --library-path "$LP" "$R/bin/true"
run proot          "$PR" -r "$R" /bin/true
run loader-ownall  env ROOTFS="$R" ELF_ROOTFS="$R" "$L" --ownall "$R/bin/true"
run bionic         /system/bin/toybox true
