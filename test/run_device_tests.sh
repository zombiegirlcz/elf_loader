#!/system/bin/sh
# ═══════════════════════════════════════════════════════════════════════
# ELF Loader — Device Test Suite (host Android shell, mimo proot)
# ═══════════════════════════════════════════════════════════════════════
# Spuštění z Android terminálu (com.linux_core host shell):
#     sh /data/user/0/com.linux_core/files/run_device_tests.sh
#   nebo přes bind:
#     sh /linux_core/run_device_tests.sh
#
# Výsledky: /data/user/0/com.linux_core/files/test_results.txt (+ stdout)
# ═══════════════════════════════════════════════════════════════════════

FILES="/data/user/0/com.linux_core/files"
ELF="$FILES/usr/bin/elf"
ROOTFS_DEFAULT="$FILES/nh/distro/parrot"
LOG="$FILES/test_results.txt"

[ -n "$ROOTFS" ] || export ROOTFS="$ROOTFS_DEFAULT"

PASS=0; FAIL=0
{
echo "═══ ELF Loader device tests ═══"
echo "datum:    $(date)"
echo "elf:      $ELF ($(ls -la "$ELF" 2>/dev/null | awk '{print $5}') B)"
echo "rootfs:   $ROOTFS"
echo ""
} > "$LOG"

# run <nazev> <ocekavany_rc> <cmd...>          -> test exit kódu
# expect_out <nazev> <ocekavany_rc> <pattern> <cmd...> -> test rc + obsah výstupu
run() {
    name="$1"; want_rc="$2"; shift 2
    out=$("$@" 2>&1); rc=$?
    if [ "$rc" = "$want_rc" ]; then
        PASS=$((PASS+1)); echo "PASS  $name (rc=$rc)" >> "$LOG"
    else
        FAIL=$((FAIL+1)); echo "FAIL  $name (rc=$rc, ocekavano $want_rc)" >> "$LOG"
        echo "$out" | sed 's/^/      | /' >> "$LOG"
    fi
}

expect_out() {
    name="$1"; want_rc="$2"; pattern="$3"; shift 3
    out=$("$@" 2>&1); rc=$?
    ok=1
    [ "$rc" = "$want_rc" ] || ok=0
    echo "$out" | grep -q "$pattern" || ok=0
    # gating: loader hlášky nesmí prosakovat do výstupu
    echo "$out" | grep -q '\[+\]' && ok=0
    if [ "$ok" = 1 ]; then
        PASS=$((PASS+1)); echo "PASS  $name (rc=$rc, out~'$pattern', cisty vystup)" >> "$LOG"
    else
        FAIL=$((FAIL+1)); echo "FAIL  $name (rc=$rc, ocekavano $want_rc + '$pattern' + zadny [+])" >> "$LOG"
        echo "$out" | sed 's/^/      | /' >> "$LOG"
    fi
}

B=$ROOTFS/bin
E=$ROOTFS/etc
# glibc binárky nelze execnout přímo (PT_INTERP /lib/ld-linux neexistuje na
# bionic hostu -> rc=126) - VŽDY přes elf wrapper (elf_loader --ownall)
R="$ELF"

# ─── 1) funkční baterie (rc + obsah, čistý výstup bez [+]) ───
expect_out "echo"        0 "hello world"   $R $B/echo hello world
expect_out "uname"       0 "aarch64"       $R $B/uname -m
expect_out "date"        0 "^[0-9][0-9][0-9][0-9]$"   $R $B/date +%Y
expect_out "cat"         0 "TERMINATOR"    $R $B/cat $E/hostname
expect_out "wc"          0 "^11 "          $R $B/wc -c $E/hostname
expect_out "grep"        0 "^1$"           $R $B/grep -c ^root $E/passwd
expect_out "sed"         0 "TERMINATOR"    $R $B/sed -n 1p $E/hostname
run  "true"              0                 $R $B/true
run  "false"             1                 $R $B/false
expect_out "ls etc"      0 "."             $R $B/ls $E

# ─── 2) ELF_DEBUG gating ───
out=$(ELF_DEBUG=1 $R $B/echo dbg-on 2>&1)
if echo "$out" | grep -q "\[+\]"; then
    PASS=$((PASS+1)); echo "PASS  ELF_DEBUG=1 zapina verbose ([+] pritomny)" >> "$LOG"
else
    FAIL=$((FAIL+1)); echo "FAIL  ELF_DEBUG=1 zapina verbose ([-] chybi [+])" >> "$LOG"
fi

# ─── souhrn ───
TOTAL=$((PASS+FAIL))
echo "" >> "$LOG"
echo "═══ Souhrn: $PASS/$TOTAL pass, $FAIL fail ═══" >> "$LOG"
if [ "$FAIL" = 0 ]; then
    echo "✓ VSECHNY TESTY PROLY ("$TOTAL"/"$TOTAL")" >> "$LOG"
else
    echo "✗ $FAIL testu SELHALO" >> "$LOG"
fi

cat "$LOG"
echo ""
echo "vysledky zapsany do: $LOG"
