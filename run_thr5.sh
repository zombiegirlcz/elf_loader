#!/bin/bash
# Páté kolo: thread churn + dlopen v threadu + node s omezeným pool.
set -uo pipefail

PG=/mnt/app/files/nh/distro/parrot/usr/bin/gcc
PMNT=/mnt/app/files/nh/distro/parrot/tmp
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
SRC=/root/elf_loader/test

mkdir -p "$PMNT"
for t in thr_churn thr_dlopen; do
  echo "=== compile $t ==="
  "$PG" -O0 -g -pthread "$SRC/$t.c" -o "$PMNT/$t" || echo "COMPILE FAIL $t"
done

echo
echo "########## RUN (ashell -c) ##########"
for t in thr_churn thr_dlopen; do
  echo "=== $t ==="
  ashell -c "$L --ownall $R/tmp/$t; echo RC=\$?" 2>&1 \
    | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' \
    | tail -12
done

echo
echo "########## NODE s omezením thread poolu ##########"
for env in "UV_THREADPOOL_SIZE=1" "UV_THREADPOOL_SIZE=0" "UV_THREADPOOL_SIZE=1 V8_OPTIONS=--v8-pool-size=0"; do
  echo "=== node --version ($env) ==="
  ashell -c "$env $L --ownall $R/opt/node/bin/node --version > $R/tmp/nn.out 2>&1; echo RC=\$?" >/dev/null 2>&1
  tail -3 /mnt/app/files/nh/distro/parrot/tmp/nn.out
done