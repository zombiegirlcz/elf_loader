#!/bin/bash
# Bisect thread churn: od kolika rund to spadne.
set -uo pipefail
PG=/mnt/app/files/nh/distro/parrot/usr/bin/gcc
PMNT=/mnt/app/files/nh/distro/parrot/tmp
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
SRC=/root/elf_loader/test

"$PG" -O0 -g -pthread "$SRC/thr_churn_n.c" -o "$PMNT/thr_churn_n" || exit 1

for n in 1 2 5 10 20 40 80 120 160 200; do
  out=$(ashell -c "$L --ownall $R/tmp/thr_churn_n $n; echo RC=\$?" 2>&1)
  rc=$(echo "$out" | grep -oE 'RC=[0-9]+' | tail -1)
  res=$(echo "$out" | grep -E 'OK thr_churn_n|Segmentation|double free|FAIL' | tail -1)
  printf "rounds=%-4s %-40s %s\n" "$n" "$res" "$rc"
done