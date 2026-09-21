#!/bin/bash
# Třetí kolo: C++ __cxa_thread_atexit_impl + V8-like teardown + dlopen s cestou.
set -uo pipefail

PG=/mnt/app/files/nh/distro/parrot/usr/bin/gcc
PGXX=/mnt/app/files/nh/distro/parrot/usr/bin/g++
PMNT=/mnt/app/files/nh/distro/parrot/tmp
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
SRC=/root/elf_loader/test

mkdir -p "$PMNT"

echo "=== compile cxa_thread (C++) ==="
"$PGXX" -O0 -g -pthread "$SRC/cxa_thread.c" -o "$PMNT/cxa_thread" || echo "COMPILE FAIL cxa_thread"
echo "=== compile v8_like (C++) ==="
"$PGXX" -O0 -g -pthread "$SRC/v8_like.c" -o "$PMNT/v8_like" || echo "COMPILE FAIL v8_like"

echo
echo "########## RUN (ashell -c) ##########"
for t in cxa_thread v8_like; do
  echo "=== $t ==="
  ashell -c "$L --ownall $R/tmp/$t; echo RC=\$?" 2>&1 \
    | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' \
    | tail -22
done

echo
echo "=== dlopen s absolutni cestou (parrot libdir) ==="
ashell -c "$L --ownall $R/usr/bin/python3.13 -c 'import ctypes; h=ctypes.CDLL(\"$R/usr/lib/aarch64-linux-gnu/libm.so.6\"); print(\"ctypes libm ok\")'; echo RC=\$?" 2>&1 \
  | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' | tail -8

echo
echo "=== node s LD_LIBRARY_PATH na parrot libdirs ==="
ashell -c "LD_LIBRARY_PATH=$R/lib/aarch64-linux-gnu:$R/usr/lib/aarch64-linux-gnu $L --ownall $R/opt/node/bin/node --version; echo RC=\$?" 2>&1 \
  | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$' | head -8