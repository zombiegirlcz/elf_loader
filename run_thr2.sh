#!/bin/bash
# Druhé kolo: těžší thread/dlopen/C++ TLS scénáře + srovnání node/python.
set -uo pipefail

PG=/mnt/app/files/nh/distro/parrot/usr/bin/gcc
PGXX=/mnt/app/files/nh/distro/parrot/usr/bin/g++
PMNT=/mnt/app/files/nh/distro/parrot/tmp
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
SRC=/root/elf_loader/test

mkdir -p "$PMNT"

build() { echo "=== compile $1 ==="; "$2" -O0 -g -pthread "$SRC/$1.c" -o "$PMNT/$1" || echo "COMPILE FAIL $1"; }
build thr_heavy "$PG"
build dl_open  "$PG"
build cxa_thread "$PGXX"

echo
echo "########## RUN (ashell -c) ##########"
for t in thr_heavy dl_open cxa_thread; do
  echo "=== $t ==="
  ashell -c "$L --ownall $R/tmp/$t; echo RC=\$?" 2>&1 \
    | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' \
    | tail -18
done

echo
echo "=== python3 -c 'import os,sys,json,ssl' ==="
ashell -c "$L --ownall $R/usr/bin/python3.13 -c 'import os,sys,json,ssl,hashlib,threading; print(\"py-ok\", sys.version_info[:2], threading.active_count())'; echo RC=\$?" 2>&1 \
  | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' | tail -8

echo
echo "=== node --version (baseline crash) ==="
ashell -c "$L --ownall $R/opt/node/bin/node --version; echo RC=\$?" 2>&1 \
  | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' | head -6