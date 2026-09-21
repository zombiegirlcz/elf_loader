#!/bin/bash
# A/B měření ELF_FIX — SPRÁVNÉ parsování (crash dump jde do stejného souboru).
# Klíčová metrika: kolik běhů vytisklo přesně "42" a jaké RC.
set -uo pipefail

D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
N=$R/opt/node/bin/node
O=$R/tmp/ab.out
H=/mnt/app/files
OH=$H/nh/distro/parrot/tmp/ab.out

measure() { # measure <label> <env> <args> <expected>
  local label="$1" env="$2" args="$3" want="$4"
  local n=5 ok=0 rcs=""
  for i in $(seq 1 $n); do
    ashell -c "$env $L --ownall $N $args > $O 2>&1; echo RC=\$? >> $O" >/dev/null 2>&1
    if grep -qxF "$want" "$OH" 2>/dev/null; then ok=$((ok+1)); fi
    rcs="$rcs $(grep -oE 'RC=[0-9]+' "$OH" 2>/dev/null | tail -1)"
  done
  printf '%-10s want=%-6s OK=%d/%d  RC:%s\n' "$label" "$want" "$ok" "$n" "$rcs"
}

echo "===== node -e 'console.log(6*7)' (expected 42) ====="
measure baseline ''                   "-e 'console.log(6*7)'" 42
measure stack    'ELF_FIX=stack'      "-e 'console.log(6*7)'" 42
measure phdr     'ELF_FIX=phdr'       "-e 'console.log(6*7)'" 42
measure both     'ELF_FIX=stack,phdr' "-e 'console.log(6*7)'" 42

echo
echo "===== node --version (expected v26.8.1) ====="
measure baseline ''                   "--version" v26.8.1
measure stack    'ELF_FIX=stack'      "--version" v26.8.1
measure phdr     'ELF_FIX=phdr'       "--version" v26.8.1
measure both     'ELF_FIX=stack,phdr' "--version" v26.8.1

echo
echo "===== kontrola: aplikuje se ELF_FIX? (stack_probe) ====="
P=$R/tmp/stack_probe
for f in '' 'ELF_FIX=stack' 'ELF_FIX=phdr' 'ELF_FIX=stack,phdr'; do
  ashell -c "$f $L --ownall $P > $O 2>&1; echo RC=\$? >> $O" >/dev/null 2>&1
  echo "--- ${f:-<baseline>} ---"
  grep -aE '__libc_stack_end=|DSO name=|GNU_RELRO vaddr=' "$OH" 2>/dev/null | head -12
done