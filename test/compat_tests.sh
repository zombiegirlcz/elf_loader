#!/bin/sh
# ═══════════════════════════════════════════════════════════════════════
# compat_tests.sh — paritní test proot vs elf wrapper
# ═══════════════════════════════════════════════════════════════════════
#   proot:   WRAPPER=""      sh compat_tests.sh > ref.txt
#   host:    WRAPPER=elf     sh compat_tests.sh > dev.txt
#
# Vsechny TESTOVANE binarky jsou parrot (volane jmenem — na hostu je
# resolvne elf wrapper). Harness pouziva jen shell builtiny, aby vysledky
# nezavisely na tom, jestli bezi pod parrot sh nebo mksh.
# ═══════════════════════════════════════════════════════════════════════

W=${WRAPPER:-}

t() {  # t <popis> <cmd...>
    name=$1; shift
    echo "=== $name"
    if [ -n "$W" ]; then out=$("$W" "$@" </dev/null 2>&1); rc=$?
    else out=$("$@" </dev/null 2>&1); rc=$?; fi
    echo "$out"
    echo "--rc=$rc"
}

tp() {  # tp <popis> <stdin> <cmd...>  (echo jako pure builtin generator)
    name=$1; data=$2; shift 2
    echo "=== $name"
    if [ -n "$W" ]; then out=$(echo "$data" | "$W" "$@" 2>&1); rc=$?
    else out=$(echo "$data" | "$@" 2>&1); rc=$?; fi
    echo "$out"
    echo "--rc=$rc"
}

echo "# compat battery start"

t  echo        echo hello world
t  printf      printf 'answer=%d\n' 42
t  seq         seq 1 5
tp wc-l        'a
b
c'              wc -l
tp sort        'c
a
b'              sort
tp uniq        'x
x
y'              uniq
tp cut         'a:b:c'           cut -d: -f2
tp tr          abc               tr a-z A-Z
tp head2       '1
2
3'              head -n 2
tp tail1       '1
2
3'              tail -n 1
tp grep-count  'foo
bar
foo'             grep -c foo
tp sed         hello             sed s/hello/ahoj/
t  basename    basename /usr/bin/ls
t  dirname     dirname /usr/bin/ls
t  expr        expr 6 \* 7
t  uname-m     uname -m
t  id-u-root   id -u
t  hostname    hostname
t  pwd         pwd

# fork/exec uvnitr parrot shellu (pod loaderem)
# POZOR: t() pridava $W samo — v volani NENI $W argument!
if [ -n "$W" ]; then
    t  bash-exit7   bash -c 'exit 7'
    t  bash-hello   bash -c 'echo from-bash'
    t  sh-pipe      sh -c 'seq 3 | tail -n 1'
else
    t  bash-exit7   bash -c 'exit 7'
    t  bash-hello   bash -c 'echo from-bash'
    t  sh-pipe      sh -c 'seq 3 | tail -n 1'
fi

# fs operace v CWD
td=cwtest.$$
t  mkdir-rmdir sh -c "mkdir -p $td/sub && rmdir $td/sub $td && echo fsok"

echo "# compat battery end"
