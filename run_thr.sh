#!/bin/bash
# Sestaví a spustí minimální thread/TLS testy pod elf_loaderem na device.
#
# POZOR na dvě různé cesty ke STEJNÉMU místu:
#   proot (kompilace):  /mnt/app/files/nh/distro/parrot
#   device (ashell -c): /data/user/0/com.linux_core/files/nh/distro/parrot
# /root/elf_loader/files NENÍ bind-mount na device (stat inode se liší),
# takže NIKDY nekompiluj přes /root/elf_loader/files.
set -uo pipefail

PG=/mnt/app/files/nh/distro/parrot/usr/bin/gcc
PMNT=/mnt/app/files/nh/distro/parrot/tmp
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
SRC=/root/elf_loader/test

mkdir -p "$PMNT"
for t in thr_simple thr_malloc thr_tls; do
  echo "=== compile $t ==="
  "$PG" -O0 -g -pthread "$SRC/$t.c" -o "$PMNT/$t" || { echo "COMPILE FAIL $t"; continue; }
  echo "interp: $(readelf -l "$PMNT/$t" 2>/dev/null | grep -o '/lib/ld-linux[^ ]*' | head -1)"
done

echo
echo "########## RUN pod elf_loaderem (ashell -c) ##########"
for t in thr_simple thr_malloc thr_tls; do
  echo "=== $t ==="
  ashell -c "$L --ownall $R/tmp/$t; echo RC=\$?" 2>&1 \
    | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' \
    | tail -15
done