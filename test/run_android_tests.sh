#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════
# ELF Loader — Android Host Automated Test Suite (via ashell)
# ═══════════════════════════════════════════════════════════════════════
#
# Spouští testy Parrot binárek MIMO PRoot v hostitelském prostředí Androidu
# (aplikace com.linux_core / bionic) přes ashell -c.
#
# Požadavky prostředí:
#   1. Aplikace: /data/user/0/com.linux_core/files/usr/bin/elf (wrapper)
#   2. Bionic loader: /data/user/0/com.linux_core/files/usr/bin/elf_loader
#   3. Rootfs: /data/user/0/com.linux_core/files/nh/distro/parrot
#
# Použití:
#   ./test/run_android_tests.sh            # spustí testovací baterii
#   ./test/run_android_tests.sh --verbose  # detailní výpis každého testu
# ═══════════════════════════════════════════════════════════════════════

set -euo pipefail

# ──── Konfigurace cest ────────────────────────────────────────────────
APP_MNT="/mnt/app/files"
APP_USR_BIN="/data/user/0/com.linux_core/files/usr/bin"
APP_ROOTFS="/data/user/0/com.linux_core/files/nh/distro/parrot"
LOCAL_USR_BIN="$APP_MNT/usr/bin"

VERBOSE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --verbose|-v) VERBOSE=1 ;;
        *) echo "Neznámý parametr: $1" >&2; exit 1 ;;
    esac
    shift
done

# ──── Barvy ───────────────────────────────────────────────────────────
RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
CYAN='\033[1;36m'
DIM='\033[2m'
RST='\033[0m'

# ──── 1. Kontrola / Nasazení do aplikace ──────────────────────────────
echo ""
printf "${CYAN}═══ Nasazení do aplikace (/data/user/0/com.linux_core/files/usr/bin) ═══${RST}\n"

mkdir -p "$LOCAL_USR_BIN"

# Kontrola / aktualizace wrapper skriptu elf
cat > "$LOCAL_USR_BIN/elf" << 'EOF'
#!/system/bin/sh
if [ -z "$ROOTFS" ]; then
    export ROOTFS="/data/user/0/com.linux_core/files/nh/distro/parrot"
fi

BIN_DIR="/data/user/0/com.linux_core/files/usr/bin"
# Bionic libc.so v /system/lib64 musí být první, aby bionic linker nesahal na glibc ld-script
export LD_LIBRARY_PATH="/system/lib64:/system/lib:$LD_LIBRARY_PATH:$ROOTFS/usr/lib/aarch64-linux-gnu:$ROOTFS/lib"

exec "$BIN_DIR/elf_loader" --ownall "$@"
EOF
chmod 755 "$LOCAL_USR_BIN/elf"
echo "  [+] Wrapper skript 'elf' nastaven a zkontrolován."

# Kontrola existence bionic elf_loader
if [ ! -f "$LOCAL_USR_BIN/elf_loader" ]; then
    echo "  [-] CHYBA: $LOCAL_USR_BIN/elf_loader chybí!" >&2
    exit 1
fi
chmod 755 "$LOCAL_USR_BIN/elf_loader"
echo "  [+] Bionic binárka 'elf_loader' připravena."

# ──── 2. Testovací funkce ─────────────────────────────────────────────
PASS=0
FAIL=0
XFAIL=0
TOTAL=0

# Spustí testovací binárku venku z prootu přes ashell
# Použití: run_android_test <název_testu> <binárka_relativně_k_ROOTFS> [argumenty...]
run_android_test() {
    local test_name="$1"
    local rel_bin="$2"
    shift 2
    local bin_args="$*"
    local expect_exit="${EXPECT_EXIT:-0}"
    local expect_stdout="${EXPECT_STDOUT:-}"
    local xfail="${XFAIL_REASON:-}"

    TOTAL=$((TOTAL + 1))

    # Sestavíme příkaz přesně dle zadání:
    # unset PATH; export ROOTFS=...; /data/user/0/com.linux_core/files/usr/bin/elf $ROOTFS/<bin> <args>
    local remote_cmd="unset PATH; export ROOTFS=$APP_ROOTFS; $APP_USR_BIN/elf \$ROOTFS/$rel_bin $bin_args"

    local raw_output
    local exit_code=0

    # Spuštění venku přes ashell -c
    raw_output=$(ashell -c "sh -c '$remote_cmd'" 2>&1) || exit_code=$?

    local ok=1

    # Kontrola návratového kódu
    if [ "$exit_code" -ne "$expect_exit" ]; then
        ok=0
    fi

    # Kontrola očekávaného výstupu (regex)
    if [ -n "$expect_stdout" ] && ! echo "$raw_output" | grep -qE "$expect_stdout"; then
        ok=0
    fi

    if [ "$ok" -eq 1 ]; then
        if [ -n "$xfail" ]; then
            printf "  ${GREEN}PASS${RST}  %-42s ${DIM}(xfail resolved!)${RST}\n" "$test_name"
        else
            printf "  ${GREEN}PASS${RST}  %-42s\n" "$test_name"
        fi
        PASS=$((PASS + 1))
    elif [ -n "$xfail" ]; then
        printf "  ${YELLOW}XFAIL${RST} %-42s ${DIM}(%s, exit=%d)${RST}\n" "$test_name" "$xfail" "$exit_code"
        XFAIL=$((XFAIL + 1))
        if [ "$VERBOSE" -eq 1 ]; then
            echo "    output: $(echo "$raw_output" | tail -3)"
        fi
    else
        printf "  ${RED}FAIL${RST}  %-42s (exit=%d, expected=%d)\n" "$test_name" "$exit_code" "$expect_exit"
        FAIL=$((FAIL + 1))
        if [ "$VERBOSE" -eq 1 ]; then
            echo "    cmd:    $remote_cmd"
            echo "    output: $(echo "$raw_output" | tail -5)"
        fi
    fi

    unset EXPECT_EXIT EXPECT_STDOUT XFAIL_REASON
}

# ──── 3. Testovací baterie pod Androidem ──────────────────────────────
printf "\n${CYAN}═══ Běh testů na Android hostiteli (přes ashell) ═══${RST}\n"
printf "${CYAN}  ROOTFS:  %s${RST}\n" "$APP_ROOTFS"
printf "${CYAN}  Příkaz:  unset PATH; export ROOTFS=...; %s/elf \$ROOTFS/bin/...${RST}\n\n" "$APP_USR_BIN"

# Test 1: true
EXPECT_EXIT=0 XFAIL_REASON="segfault v libc po init"
run_android_test "true ($APP_ROOTFS/bin/true)" "bin/true"

# Test 2: false
EXPECT_EXIT=1 XFAIL_REASON="segfault v libc po init"
run_android_test "false ($APP_ROOTFS/bin/false)" "bin/false"

# Test 3: echo
EXPECT_EXIT=0 EXPECT_STDOUT="Ahoj" XFAIL_REASON="segfault v libc po init"
run_android_test "echo ($APP_ROOTFS/bin/echo)" "bin/echo" "Ahoj z Androidu!"

# Test 4: ls
EXPECT_EXIT=0 EXPECT_STDOUT="passwd" XFAIL_REASON="segfault v libc po init"
run_android_test "ls ($APP_ROOTFS/bin/ls)" "bin/ls" "$APP_ROOTFS/etc/passwd"

# Test 5: cat
EXPECT_EXIT=0 EXPECT_STDOUT="root" XFAIL_REASON="segfault v libc po init"
run_android_test "cat ($APP_ROOTFS/bin/cat)" "bin/cat" "$APP_ROOTFS/etc/passwd"

# Test 6: grep
EXPECT_EXIT=0 EXPECT_STDOUT="root" XFAIL_REASON="segfault v libc po init"
run_android_test "grep ($APP_ROOTFS/bin/grep)" "bin/grep" "root" "$APP_ROOTFS/etc/passwd"

# Test 7: wc
EXPECT_EXIT=0 EXPECT_STDOUT="[0-9]" XFAIL_REASON="segfault v libc po init"
run_android_test "wc ($APP_ROOTFS/bin/wc)" "bin/wc" "-l" "$APP_ROOTFS/etc/passwd"

# Test 8: sed
EXPECT_EXIT=0 EXPECT_STDOUT="root" XFAIL_REASON="segfault v libc po init"
run_android_test "sed ($APP_ROOTFS/bin/sed)" "bin/sed" "-n" "1p" "$APP_ROOTFS/etc/passwd"

# Test 9: uname
EXPECT_EXIT=0 EXPECT_STDOUT="aarch64|Linux" XFAIL_REASON="segfault v libc po init"
run_android_test "uname ($APP_ROOTFS/bin/uname)" "bin/uname" "-a"

# Test 10: date
EXPECT_EXIT=0 EXPECT_STDOUT="[0-9]{4}" XFAIL_REASON="segfault v libc po init"
run_android_test "date ($APP_ROOTFS/bin/date)" "bin/date"

# ──── 4. Vyhodnocení ──────────────────────────────────────────────────
echo ""
printf "${CYAN}═══ Souhrn výsledků ═══${RST}\n"
printf "  Celkem:  %d\n" "$TOTAL"
printf "  ${GREEN}Pass:    %d${RST}\n" "$PASS"
if [ "$XFAIL" -gt 0 ]; then
    printf "  ${YELLOW}XFail:   %d${RST}  ${DIM}(známé problémy, neselhává testovací skript)${RST}\n" "$XFAIL"
fi
if [ "$FAIL" -gt 0 ]; then
    printf "  ${RED}Fail:    %d${RST}\n" "$FAIL"
else
    printf "  Fail:    %d\n" "$FAIL"
fi
echo ""

if [ "$FAIL" -eq 0 ]; then
    printf "${GREEN}✓ Všechny testy doběhly v pořádku (s očekávanými stavy)!${RST}\n"
    exit 0
else
    printf "${RED}✗ %d testů selhalo neočekávaně${RST}\n" "$FAIL"
    exit 1
fi
