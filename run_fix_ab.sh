#!/bin/bash
# A/B měření runtime přepínačů ELF_FIX na node přes ashell -c.
#
# Dvě cesty ke stejnému místu:
#   proot (čtení výsledků): /mnt/app/files/...
#   device (ashell -c):     /data/user/0/com.linux_core/files/...
set -uo pipefail

D=/data/user/0/com.linux_core/files     # device cesta pro ashell
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
N=$R/opt/node/bin/node
O=$R/tmp/fix_ab.out                     # device cesta zápisu

H=/mnt/app/files                        # host/proot zrcadlo
HR=$H/nh/distro/parrot
OH=$HR/tmp/fix_ab.out                   # host cesta čtení

variant() { # variant <label> <env-prefix> <args>
  local label="$1" env="$2" args="$3" rcs="" outs=""
  for i in 1 2 3 4 5; do
    ashell -c "$env $L --ownall $N $args > $O 2>&1; echo RC=\$?" >/dev/null 2>&1
    local rc first sig
    rc=$(grep -oE 'RC=[0-9]+' "$OH" 2>/dev/null | tail -1)
    first=$(head -1 "$OH" 2>/dev/null | tr -d '\n')
    sig=$(grep -oE 'double free|Segmentation fault|Aborted' "$OH" 2>/dev/null | tail -1)
    rcs="$rcs ${rc:-RC=?}"
    outs="$outs ${first:-<empty>}${sig:+/$sig}"
  done
  printf '%-12s RC:%s\n%-12s OUT:%s\n' "$label" "$rcs" "" "$outs"
}

echo "===== node -e 'console.log(6*7)' ====="
variant baseline ''                  "-e 'console.log(6*7)'"
variant stack    'ELF_FIX=stack'     "-e 'console.log(6*7)'"
variant phdr     'ELF_FIX=phdr'      "-e 'console.log(6*7)'"
variant both     'ELF_FIX=stack,phdr' "-e 'console.log(6*7)'"

echo
echo "===== node --version (3x) ====="
variant3() { local label="$1" env="$2" args="$3" rcs="" outs=""
  for i in 1 2 3; do
    ashell -c "$env $L --ownall $N $args > $O 2>&1; echo RC=\$?" >/dev/null 2>&1
    rcs="$rcs $(grep -oE 'RC=[0-9]+' "$OH" | tail -1)"
    outs="$outs $(head -1 "$OH" | tr -d '\n')"
  done
  printf '%-12s RC:%s OUT:%s\n' "$label" "$rcs" "$outs"
}
variant3 baseline ''                  "--version"
variant3 both     'ELF_FIX=stack,phdr' "--version"