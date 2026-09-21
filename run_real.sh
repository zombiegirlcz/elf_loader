#!/bin/bash
# Širší test reálných zátěží (ne --version/--help) přes ashell -c.
# Cíl: najít další RED binárky vedle node a ověřit, že jednoduché věci jdou.
set -uo pipefail
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader

t() { # t <nazev> <prikaz>
  local name="$1"; shift
  local out rc sig
  out=$(ashell -c "$L --ownall $*; echo __RC=\$?" 2>&1)
  rc=$(printf '%s' "$out" | grep -oE '__RC=[0-9]+' | tail -1)
  sig=$(printf '%s' "$out" | grep -oE 'Segmentation fault|double free|Aborted|Illegal instruction|Bus error' | tail -1)
  local body
  body=$(printf '%s' "$out" | grep -vE '^[0-9a-f]+-|maps-|frame ra=|insn|stack:|\[LDSO\]|^F:tp=|^\s*$|^  @|^0000' | tail -3 | tr '\n' '|')
  printf '%-14s %-6s %-18s %s\n' "$name" "${rc:-?}" "${sig:--}" "$body"
}

echo "===== REALNE ZATEZE (ashell -c) ====="
t perl       "$R/usr/bin/perl -e 'print 6*7, qq(\\n)'"
t perl-regex "$R/usr/bin/perl -e 'my \$s=q(abc123); \$s =~ /(\\d+)/; print \$1, qq(\\n)'"
t openssl    "$R/usr/bin/openssl dgst -sha256 $R/etc/hostname"
t openssl-rsa "$R/usr/bin/openssl genrsa 1024"
t git-ver    "$R/usr/bin/git --version"
t git-init   "$R/usr/bin/git init $R/tmp/gtest"
t ruby       "$R/usr/bin/ruby -e 'puts 6*7'"
t sqlite3    "$R/usr/bin/sqlite3 :memory: 'select 6*7;'"
t gcc-build  "$R/usr/bin/gcc -O0 -x c -o $R/tmp/h - <<< 'int main(){return 0;}' ; $R/tmp/h"
t bash-pipe  "$R/usr/bin/bash -c 'echo hello | tr a-z A-Z'"
t awk-math   "$R/usr/bin/awk 'BEGIN{print 6*7}'"
t curl-help  "$R/usr/bin/curl --version"
t cmake      "$R/usr/bin/cmake --version"
t python-thr "$R/usr/bin/python3.13 -c 'import threading; print(threading.active_count())'"
t python-hash "$R/usr/bin/python3.13 -c 'import hashlib; print(hashlib.sha256(b\"x\").hexdigest()[:16])'"
t node       "$R/opt/node/bin/node -e 'console.log(6*7)'"
t node-ver   "$R/opt/node/bin/node --version"
t npm        "$R/opt/node/bin/npm --version"
