#!/bin/bash
# Čtvrté kolo: PLATNÉ C++ thread_local dtory (&__dso_handle) + F:tp analýza node.
set -uo pipefail

PGXX=/mnt/app/files/nh/distro/parrot/usr/bin/g++
PMNT=/mnt/app/files/nh/distro/parrot/tmp
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
SRC=/root/elf_loader/test

mkdir -p "$PMNT"

echo "=== compile cxa_thread (C++, &__dso_handle) ==="
"$PGXX" -O0 -g -pthread "$SRC/cxa_thread.c" -o "$PMNT/cxa_thread" || echo "COMPILE FAIL"
echo "=== compile v8_like (C++, &__dso_handle) ==="
"$PGXX" -O0 -g -pthread "$SRC/v8_like.c" -o "$PMNT/v8_like" || echo "COMPILE FAIL"

echo
echo "########## RUN (ashell -c) ##########"
for t in cxa_thread v8_like; do
  echo "=== $t ==="
  ashell -c "$L --ownall $R/tmp/$t; echo RC=\$?" 2>&1 \
    | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' \
    | tail -18
done

echo
echo "########## NODE: F:tp registr x22 (dso_symbol) ##########"
# F:tp řádek: F:tp=.. pc=.. sp=.. ad=.. x00=.. x01=.. ... x22=..
ashell -c "$L --ownall $R/opt/node/bin/node --version; echo RC=\$?" 2>&1 \
  | grep -E '^F:tp=' | head -1 \
  | tr ' ' '\n' | grep -E '^(pc|sp|ad|x0[0-9]|x1[0-9]|x2[0-9]|x30)=' \
  | sed 's/^/  /'