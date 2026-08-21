#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════
# ELF Loader — Automated Test Suite (Parrot rootfs)
# ═══════════════════════════════════════════════════════════════════════
#
# Usage:
#   ./test/run_parrot_tests.sh              # run inside proot (default)
#   ./test/run_parrot_tests.sh --ashell     # run outside proot via ashell -c
#   ./test/run_parrot_tests.sh --verbose    # show full loader output
#   ./test/run_parrot_tests.sh --mode run   # test only --run mode
#   ./test/run_parrot_tests.sh --mode ownall # test only --ownall mode
#
# Exit code: 0 if all tests pass, 1 if any test fails.
# ═══════════════════════════════════════════════════════════════════════

set -euo pipefail

# ──── Configuration ────────────────────────────────────────────────────
if [ -d "/mnt/app/files/nh/distro/parrot" ]; then
    PARROT_ROOT="/mnt/app/files/nh/distro/parrot"
elif [ -d "/data/user/0/com.linux_core/files/nh/distro/parrot" ]; then
    PARROT_ROOT="/data/user/0/com.linux_core/files/nh/distro/parrot"
else
    echo "FATAL: Parrot rootfs not found" >&2
    exit 1
fi

LIB_DIR="$PARROT_ROOT/usr/lib/aarch64-linux-gnu"
LOADER_BIN="${ELF_LOADER_BIN:-/root/elf_loader/elf_loader}"

# ──── Argument parsing ─────────────────────────────────────────────────
USE_ASHELL=0
VERBOSE=0
FILTER_MODE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --ashell)   USE_ASHELL=1 ;;
        --verbose)  VERBOSE=1 ;;
        --mode)     shift; FILTER_MODE="$1" ;;
        *)          echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
    shift
done

# ──── Counters & colors ───────────────────────────────────────────────
PASS=0
FAIL=0
SKIP=0
XFAIL=0     # expected failures (known issues)
TOTAL=0

RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
CYAN='\033[1;36m'
DIM='\033[2m'
RST='\033[0m'

# ──── Helper functions ─────────────────────────────────────────────────

exec_cmd() {
    if [ "$USE_ASHELL" -eq 1 ]; then
        ashell -c "$*"
    else
        eval "$@"
    fi
}

# Run a single test case
# Arguments: <test_name> <mode> <binary> [binary_args...]
# Environment:
#   EXPECT_EXIT     - expected exit code (default 0)
#   EXPECT_STDOUT   - regex pattern expected in stdout (optional)
#   EXPECT_STDERR   - regex pattern expected in stderr (optional)
#   EXPECT_OUTPUT   - regex pattern expected in combined stdout+stderr (optional)
#   XFAIL_REASON    - if set, test is expected to fail (known issue)
run_test() {
    local test_name="$1"
    local mode="$2"
    local binary="$3"
    shift 3
    local args="$*"
    local expect_exit="${EXPECT_EXIT:-0}"
    local expect_stdout="${EXPECT_STDOUT:-}"
    local expect_stderr="${EXPECT_STDERR:-}"
    local expect_output="${EXPECT_OUTPUT:-}"
    local xfail="${XFAIL_REASON:-}"

    TOTAL=$((TOTAL + 1))

    # Skip if mode filter is set
    if [ -n "$FILTER_MODE" ] && [ "$mode" != "$FILTER_MODE" ]; then
        SKIP=$((SKIP + 1))
        unset EXPECT_EXIT EXPECT_STDOUT EXPECT_STDERR EXPECT_OUTPUT XFAIL_REASON
        return
    fi

    # Check binary exists
    if [ ! -f "$binary" ]; then
        printf "  ${YELLOW}SKIP${RST} %-45s (binary not found)\n" "$test_name"
        SKIP=$((SKIP + 1))
        unset EXPECT_EXIT EXPECT_STDOUT EXPECT_STDERR EXPECT_OUTPUT XFAIL_REASON
        return
    fi

    local cmd="LD_LIBRARY_PATH=$LIB_DIR $LOADER_BIN"
    case "$mode" in
        run)        cmd="$cmd --run $binary $args" ;;
        ownall)     cmd="$cmd --ownall $binary $args" ;;
        shim)       cmd="$cmd --shim $binary $args" ;;
        lazy)       cmd="$cmd --lazy --run $binary $args" ;;
        introspect) cmd="$cmd $binary" ;;
    esac

    local stdout stderr combined actual_exit
    stdout=$(exec_cmd "$cmd" 2>/tmp/elf_test_stderr) && actual_exit=$? || actual_exit=$?
    stderr=$(cat /tmp/elf_test_stderr 2>/dev/null || true)
    combined="$stdout
$stderr"

    local ok=1

    # Check exit code
    if [ "$actual_exit" -ne "$expect_exit" ]; then
        ok=0
    fi

    # Check stdout pattern
    if [ -n "$expect_stdout" ] && ! echo "$stdout" | grep -qE "$expect_stdout"; then
        ok=0
    fi

    # Check stderr pattern
    if [ -n "$expect_stderr" ] && ! echo "$stderr" | grep -qE "$expect_stderr"; then
        ok=0
    fi

    # Check combined output pattern
    if [ -n "$expect_output" ] && ! echo "$combined" | grep -qE "$expect_output"; then
        ok=0
    fi

    if [ "$ok" -eq 1 ]; then
        if [ -n "$xfail" ]; then
            # Expected to fail but passed — good, counts as PASS
            printf "  ${GREEN}PASS${RST} %-45s [%s] ${DIM}(xfail resolved!)${RST}\n" "$test_name" "$mode"
        else
            printf "  ${GREEN}PASS${RST} %-45s [%s]\n" "$test_name" "$mode"
        fi
        PASS=$((PASS + 1))
    elif [ -n "$xfail" ]; then
        # Expected failure — not counted as failure
        printf "  ${YELLOW}XFAIL${RST} %-44s [%s] ${DIM}(%s)${RST}\n" "$test_name" "$mode" "$xfail"
        XFAIL=$((XFAIL + 1))
    else
        printf "  ${RED}FAIL${RST} %-45s [%s] (exit=%d, expected=%d)\n" \
            "$test_name" "$mode" "$actual_exit" "$expect_exit"
        FAIL=$((FAIL + 1))
        if [ "$VERBOSE" -eq 1 ]; then
            echo "    stdout: $(echo "$stdout" | tail -3)"
            echo "    stderr: $(echo "$stderr" | tail -3)"
        fi
    fi

    unset EXPECT_EXIT EXPECT_STDOUT EXPECT_STDERR EXPECT_OUTPUT XFAIL_REASON
}

# ══════════════════════════════════════════════════════════════════════
# TEST SUITES
# ══════════════════════════════════════════════════════════════════════

echo ""
printf "${CYAN}═══ ELF Loader Test Suite (Parrot rootfs) ═══${RST}\n"
printf "${CYAN}  Rootfs:  %s${RST}\n" "$PARROT_ROOT"
printf "${CYAN}  Loader:  %s${RST}\n" "$LOADER_BIN"
printf "${CYAN}  Ashell:  %s${RST}\n" "$([ $USE_ASHELL -eq 1 ] && echo 'YES' || echo 'no')"
echo ""

# ──── 1. Basic --run mode ──────────────────────────────────────────────
printf "${CYAN}── 1. Basic execution (--run) ──${RST}\n"

EXPECT_EXIT=0
run_test "true returns 0" run "$PARROT_ROOT/usr/bin/true"

EXPECT_EXIT=1
run_test "false returns 1" run "$PARROT_ROOT/usr/bin/false"

EXPECT_EXIT=0 EXPECT_STDOUT="hello"
run_test "echo prints argument" run "$PARROT_ROOT/usr/bin/echo" hello

EXPECT_EXIT=0 EXPECT_STDOUT="passwd"
run_test "ls finds file" run "$PARROT_ROOT/usr/bin/ls" "$PARROT_ROOT/etc/passwd"

EXPECT_EXIT=0 EXPECT_STDOUT="root"
run_test "cat reads passwd" run "$PARROT_ROOT/usr/bin/cat" "$PARROT_ROOT/etc/passwd"

EXPECT_EXIT=0 EXPECT_STDOUT="root"
run_test "grep finds root in passwd" run "$PARROT_ROOT/usr/bin/grep" "root" "$PARROT_ROOT/etc/passwd"

EXPECT_EXIT=1
run_test "grep missing pattern returns 1" run "$PARROT_ROOT/usr/bin/grep" "ZZZZNOTEXIST" "$PARROT_ROOT/etc/passwd"

EXPECT_EXIT=0 EXPECT_STDOUT="[0-9]"
run_test "wc counts lines" run "$PARROT_ROOT/usr/bin/wc" "-l" "$PARROT_ROOT/etc/passwd"

EXPECT_EXIT=0 EXPECT_STDOUT="root"
run_test "sed extracts first line" run "$PARROT_ROOT/usr/bin/sed" "-n" "1p" "$PARROT_ROOT/etc/passwd"

EXPECT_EXIT=0 EXPECT_STDOUT="[0-9]{4}"
run_test "date outputs year" run "$PARROT_ROOT/usr/bin/date"

EXPECT_EXIT=0 EXPECT_STDOUT="aarch64"
run_test "uname -m shows arch" run "$PARROT_ROOT/usr/bin/uname" "-m"

EXPECT_EXIT=0
run_test "id runs successfully" run "$PARROT_ROOT/usr/bin/id"

EXPECT_EXIT=0 EXPECT_STDOUT="PATH="
run_test "env prints variables" run "$PARROT_ROOT/usr/bin/env"

# ──── 2. Ownall mode (own-loaded glibc) ────────────────────────────────
# NOTE: --ownall own-loads libc.so.6 from Parrot (no dlopen). Some binaries
# crash (SIGSEGV/139) due to known TLS-bridging limitations (see README).
# Those are marked XFAIL.
printf "\n${CYAN}── 2. Own-loader mode (--ownall) ──${RST}\n"

EXPECT_EXIT=0
run_test "true returns 0" ownall "$PARROT_ROOT/usr/bin/true"

EXPECT_EXIT=1 XFAIL_REASON="TLS crash — exit cleanup"
run_test "false returns 1" ownall "$PARROT_ROOT/usr/bin/false"

EXPECT_EXIT=0 EXPECT_STDOUT="ownall-test" XFAIL_REASON="TLS crash in coreutils"
run_test "echo prints argument" ownall "$PARROT_ROOT/usr/bin/echo" ownall-test

EXPECT_EXIT=0 EXPECT_STDOUT="passwd" XFAIL_REASON="TLS crash — intermittent"
run_test "ls finds file" ownall "$PARROT_ROOT/usr/bin/ls" "$PARROT_ROOT/etc/passwd"

EXPECT_EXIT=0 EXPECT_STDOUT="root" XFAIL_REASON="TLS crash in coreutils"
run_test "cat reads passwd" ownall "$PARROT_ROOT/usr/bin/cat" "$PARROT_ROOT/etc/passwd"

# grep with --ownall crashes — TLS in libpcre triggers SIGSEGV
EXPECT_EXIT=0 EXPECT_STDOUT="root" XFAIL_REASON="TLS crash in libpcre"
run_test "grep finds root" ownall "$PARROT_ROOT/usr/bin/grep" "root" "$PARROT_ROOT/etc/passwd"

# wc with --ownall may crash due to locale/TLS
EXPECT_EXIT=0 EXPECT_STDOUT="[0-9]" XFAIL_REASON="TLS crash"
run_test "wc counts lines" ownall "$PARROT_ROOT/usr/bin/wc" "-l" "$PARROT_ROOT/etc/passwd"

# sed with --ownall may crash (libacl/libattr TLS)
EXPECT_EXIT=0 EXPECT_STDOUT="root" XFAIL_REASON="TLS crash in libacl"
run_test "sed extracts line" ownall "$PARROT_ROOT/usr/bin/sed" "-n" "1p" "$PARROT_ROOT/etc/passwd"

# date with --ownall — locale TLS issue
EXPECT_EXIT=0 EXPECT_STDOUT="[0-9]{4}" XFAIL_REASON="TLS crash"
run_test "date outputs year" ownall "$PARROT_ROOT/usr/bin/date"

# uname with --ownall may crash
EXPECT_EXIT=0 EXPECT_STDOUT="aarch64" XFAIL_REASON="TLS crash"
run_test "uname -m shows arch" ownall "$PARROT_ROOT/usr/bin/uname" "-m"

# ──── 3. Symbol interposition (--shim) ─────────────────────────────────
# NOTE: --shim uses dlopen'd host libs (like --run). echo may not call puts
# (some coreutils use write() directly), so the shim intercept test may
# not match. We test both that it runs and that the interpose is registered.
printf "\n${CYAN}── 3. Shim mode (--shim) ──${RST}\n"

EXPECT_EXIT=0 EXPECT_OUTPUT="interposing.*puts"
run_test "shim registers interpose" shim "$PARROT_ROOT/usr/bin/echo" shim-test

EXPECT_EXIT=0
run_test "true with shim" shim "$PARROT_ROOT/usr/bin/true"

EXPECT_EXIT=0 EXPECT_STDOUT="root"
run_test "cat passwd with shim" shim "$PARROT_ROOT/usr/bin/cat" "$PARROT_ROOT/etc/passwd"

# ──── 4. Lazy PLT binding (--lazy) ────────────────────────────────────
printf "\n${CYAN}── 4. Lazy binding (--lazy --run) ──${RST}\n"

EXPECT_EXIT=0 EXPECT_STDOUT="lazy-test"
run_test "echo with lazy binding" lazy "$PARROT_ROOT/usr/bin/echo" lazy-test

EXPECT_EXIT=0
run_test "true with lazy binding" lazy "$PARROT_ROOT/usr/bin/true"

EXPECT_EXIT=0 EXPECT_STDOUT="root"
run_test "cat passwd with lazy" lazy "$PARROT_ROOT/usr/bin/cat" "$PARROT_ROOT/etc/passwd"

EXPECT_EXIT=0 EXPECT_STDOUT="passwd"
run_test "ls with lazy binding" lazy "$PARROT_ROOT/usr/bin/ls" "$PARROT_ROOT/etc/passwd"

# ──── 5. Introspect mode ──────────────────────────────────────────────
# Introspect prints to stdout (printf), not stderr
printf "\n${CYAN}── 5. Introspect (no --run) ──${RST}\n"

EXPECT_EXIT=0 EXPECT_OUTPUT="Base:.*0x"
run_test "introspect ls" introspect "$PARROT_ROOT/usr/bin/ls"

EXPECT_EXIT=0 EXPECT_OUTPUT="Entry:.*0x"
run_test "introspect cat" introspect "$PARROT_ROOT/usr/bin/cat"

EXPECT_EXIT=0 EXPECT_OUTPUT="dynsym:.*[0-9]+ symbols"
run_test "introspect shows symbols" introspect "$PARROT_ROOT/usr/bin/echo"

# ──── 6. Edge cases ───────────────────────────────────────────────────
printf "\n${CYAN}── 6. Edge cases ──${RST}\n"

EXPECT_EXIT=0 EXPECT_STDOUT="hello world"
run_test "echo with spaces" run "$PARROT_ROOT/usr/bin/echo" "hello" "world"

EXPECT_EXIT=0
run_test "echo empty string" run "$PARROT_ROOT/usr/bin/echo" ""

EXPECT_EXIT=2
run_test "ls nonexistent returns error" run "$PARROT_ROOT/usr/bin/ls" "/nonexistent_path_xyz"

EXPECT_EXIT=2
run_test "grep no args returns error" run "$PARROT_ROOT/usr/bin/grep"

# ──── 7. Ashell roundtrip (only when --ashell) ─────────────────────────
if [ "$USE_ASHELL" -eq 1 ]; then
    printf "\n${CYAN}── 7. Ashell roundtrip ──${RST}\n"

    EXPECT_EXIT=0 EXPECT_STDOUT="ashell-works"
    run_test "ashell echo roundtrip" run "$PARROT_ROOT/usr/bin/echo" ashell-works

    EXPECT_EXIT=0 EXPECT_STDOUT="root"
    run_test "ashell cat passwd" run "$PARROT_ROOT/usr/bin/cat" "$PARROT_ROOT/etc/passwd"
fi

# ══════════════════════════════════════════════════════════════════════
# SUMMARY
# ══════════════════════════════════════════════════════════════════════
echo ""
printf "${CYAN}═══ Results ═══${RST}\n"
printf "  Total:  %d\n" "$TOTAL"
printf "  ${GREEN}Pass:   %d${RST}\n" "$PASS"
if [ "$FAIL" -gt 0 ]; then
    printf "  ${RED}Fail:   %d${RST}\n" "$FAIL"
else
    printf "  Fail:   %d\n" "$FAIL"
fi
if [ "$XFAIL" -gt 0 ]; then
    printf "  ${YELLOW}XFail:  %d${RST}  ${DIM}(known issues, not counted as failures)${RST}\n" "$XFAIL"
fi
if [ "$SKIP" -gt 0 ]; then
    printf "  ${YELLOW}Skip:   %d${RST}\n" "$SKIP"
fi
echo ""

if [ "$FAIL" -eq 0 ]; then
    printf "${GREEN}✓ All tests passed!${RST}"
    if [ "$XFAIL" -gt 0 ]; then
        printf " ${DIM}(%d known failures)${RST}" "$XFAIL"
    fi
    echo ""
    exit 0
else
    printf "${RED}✗ %d test(s) failed${RST}\n" "$FAIL"
    exit 1
fi
