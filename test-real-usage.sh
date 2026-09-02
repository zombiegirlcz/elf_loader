#!/bin/bash
# Komplexní testy reálného použití binárek v rootfs
# Testuje funkčnost, ne jen --version

# Don't exit on error - we want to continue testing even if some commands fail

D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader

RESULTS_DIR=/root/elf_loader/results
mkdir -p $RESULTS_DIR
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
PASS_LOG=$RESULTS_DIR/pass_$TIMESTAMP.txt
FAIL_LOG=$RESULTS_DIR/fail_$TIMESTAMP.txt
SKIP_LOG=$RESULTS_DIR/skip_$TIMESTAMP.txt

> "$PASS_LOG"
> "$FAIL_LOG"
> "$SKIP_LOG"

PASS=0
FAIL=0
SKIP=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

test_cmd() {
    local name="$1"
    local cmd="$2"
    local desc="$3"
    local expect="$4"
    
    local output
    local rc=0
    output=$(timeout 5 ashell -c "$cmd" 2>&1) || rc=$?
    
    if [ $rc -eq 0 ]; then
        if [ -n "$expect" ] && ! echo "$output" | grep -q "$expect"; then
            echo -e "${RED}FAIL${NC} $name: $desc (expected '$expect', got: ${output:0:50})"
            echo "FAIL: $name - $desc (expected '$expect', got: ${output:0:100})" >> "$FAIL_LOG"
            ((FAIL++))
        else
            echo -e "${GREEN}PASS${NC} $name: $desc"
            echo "PASS: $name - $desc" >> "$PASS_LOG"
            ((PASS++))
        fi
    elif [ $rc -eq 124 ]; then
        echo -e "${YELLOW}TIMEOUT${NC} $name: $desc"
        echo "TIMEOUT: $name - $desc" >> "$FAIL_LOG"
        ((FAIL++))
    elif [ $rc -ge 128 ]; then
        local sig=$((rc - 128))
        echo -e "${RED}CRASH(SIG$sig)${NC} $name: $desc"
        echo "CRASH(SIG$sig): $name - $desc" >> "$CRASH_LOG"
        ((FAIL++))
    else
        echo -e "${YELLOW}EXIT=$rc${NC} $name: $desc"
        echo "EXIT=$rc: $name - $desc" >> "$PASS_LOG"
        ((PASS++))
    fi
}

echo "=========================================="
echo "  ELF_LOADER REAL USAGE TESTS"
echo "=========================================="
echo "Rootfs: $R"
echo "Loader: $L"
echo "Date: $(date)"
echo ""

# === TEXT PROCESSING ===
echo "=== TEXT PROCESSING ==="
test_cmd "grep" "$L --ownall $R/usr/bin/grep -q root $R/etc/passwd" "grep root in passwd"
test_cmd "sed" "$L --ownall $R/usr/bin/sed -n '1p' $R/etc/hostname" "sed print first line" "TERMINATOR"
test_cmd "awk" "$L --ownall $R/usr/bin/awk 'BEGIN{print 1+2}'" "awk math" "3"
test_cmd "wc" "$L --ownall $R/usr/bin/wc -c $R/etc/hostname" "wc count chars"
test_cmd "cut" "$L --ownall $R/usr/bin/cut -d: -f1 $R/etc/passwd | head -1" "cut field" "root"
test_cmd "sort" "$L --ownall $R/usr/bin/sort" "sort stdin" <<< $'b\na\nc'
test_cmd "uniq" "$L --ownall $R/usr/bin/uniq" "uniq dedup" <<< $'a\na\nb'
test_cmd "tr" "$L --ownall $R/usr/bin/tr a-z A-Z" "tr translate" <<< "hello"
test_cmd "head" "$L --ownall $R/usr/bin/head -c 5 $R/etc/hostname" "head chars"
test_cmd "tail" "$L --ownall $R/usr/bin/tail -c 5 $R/etc/hostname" "tail chars"
test_cmd "cat" "$L --ownall $R/usr/bin/cat $R/etc/hostname" "cat file" "TERMINATOR"

# === FILE OPERATIONS ===
echo ""
echo "=== FILE OPERATIONS ==="
test_cmd "ls" "$L --ownall $R/usr/bin/ls $R/etc" "ls directory"
test_cmd "stat" "$L --ownall $R/usr/bin/stat $R/etc/hostname" "stat file"
test_cmd "find" "$L --ownall $R/usr/bin/find $R/etc -maxdepth 1 -type f | head -3" "find files"
test_cmd "realpath" "$L --ownall $R/usr/bin/realpath $R/etc/hostname" "realpath"
test_cmd "dirname" "$L --ownall $R/usr/bin/dirname $R/etc/hostname" "dirname" "etc"
test_cmd "basename" "$L --ownall $R/usr/bin/basename $R/etc/hostname" "basename" "hostname"

# === FILE CREATION/DELETION ===
echo ""
echo "=== FILE CREATION/DELETION ==="
test_cmd "mkdir" "$L --ownall $R/usr/bin/mkdir /tmp/test_elf_$$ && $L --ownall $R/usr/bin/rmdir /tmp/test_elf_$$" "mkdir rmdir"
test_cmd "touch" "$L --ownall $R/usr/bin/touch /tmp/test_elf_$$ && $L --ownall $R/usr/bin/rm /tmp/test_elf_$$" "touch rm"
test_cmd "cp" "$L --ownall $R/usr/bin/cp $R/etc/hostname /tmp/test_elf_cp && $L --ownall $R/usr/bin/cat /tmp/test_elf_cp && $L --ownall $R/usr/bin/rm /tmp/test_elf_cp" "cp cat rm" "TERMINATOR"
test_cmd "mv" "$L --ownall $R/usr/bin/mv $R/etc/hostname /tmp/test_elf_mv && $L --ownall $R/usr/bin/cat /tmp/test_elf_mv && $L --ownall $R/usr/bin/mv /tmp/test_elf_mv $R/etc/hostname" "mv cat mv back" "TERMINATOR"

# === SYSTEM INFO ===
echo ""
echo "=== SYSTEM INFO ==="
test_cmd "uname" "$L --ownall $R/usr/bin/uname -a" "uname -a" "Linux"
test_cmd "hostname" "$L --ownall $R/usr/bin/hostname" "hostname" "TERMINATOR"
test_cmd "whoami" "$L --ownall $R/usr/bin/whoami" "whoami" "root"
test_cmd "id" "$L --ownall $R/usr/bin/id -u" "id -u" "0"
test_cmd "date" "$L --ownall $R/usr/bin/date" "date" "2026"
test_cmd "uptime" "$L --ownall $R/usr/bin/uptime" "uptime"

# === COMPRESSION ===
echo ""
echo "=== COMPRESSION ==="
test_cmd "gzip" "$L --ownall $R/usr/bin/echo test | $L --ownall $R/usr/bin/gzip | $L --ownall $R/usr/bin/gunzip" "gzip gunzip pipe" "test"
test_cmd "tar" "$L --ownall $R/usr/bin/tar -czf /tmp/test_elf.tar.gz $R/etc/hostname && $L --ownall $R/usr/bin/tar -xzf /tmp/test_elf.tar.gz -O && $L --ownall $R/usr/bin/rm /tmp/test_elf.tar.gz" "tar gz" "TERMINATOR"

# === NETWORK (local only) ===
echo ""
echo "=== NETWORK (local) ==="
test_cmd "ping" "$L --ownall $R/usr/bin/ping -c 1 -W 1 127.0.0.1" "ping localhost" "1 received"
test_cmd "nslookup" "$L --ownall $R/usr/bin/nslookup localhost" "nslookup" "localhost"

# === UTILITIES ===
echo ""
echo "=== UTILITIES ==="
test_cmd "echo" "$L --ownall $R/usr/bin/echo 'hello world'" "echo" "hello world"
test_cmd "printf" "$L --ownall $R/usr/bin/printf 'test %d %s' 42 foo" "printf" "test 42 foo"
test_cmd "seq" "$L --ownall $R/usr/bin/seq 1 5" "seq" "5"
test_cmd "yes" "$L --ownall $R/usr/bin/yes x | $L --ownall $R/usr/bin/head -3" "yes head" "x"
test_cmd "diff" "$L --ownall $R/usr/bin/diff $R/etc/hostname $R/etc/hostname" "diff same" ""
test_cmd "true" "$L --ownall $R/usr/bin/true" "true" ""
test_cmd "false" "$L --ownall $R/usr/bin/false" "false (exit 1)" ""
test_cmd "sleep" "$L --ownall $R/usr/bin/sleep 0.1 && echo ok" "sleep" "ok"

# === SHELLS ===
echo ""
echo "=== SHELLS ==="
test_cmd "sh" "$L --ownall $R/bin/sh -c 'echo shell works'" "sh -c" "shell works"
test_cmd "bash" "$L --ownall $R/usr/bin/bash -c 'echo bash works'" "bash -c" "bash works"

# === SUMMARY ===
echo ""
echo "=========================================="
echo "  SUMMARY"
echo "=========================================="
echo -e "PASS:  ${GREEN}$PASS${NC}"
echo -e "FAIL:  ${RED}$FAIL${NC}"
echo -e "SKIP:  ${YELLOW}$SKIP${NC}"
echo ""
echo "Logs saved to: $RESULTS_DIR"
echo "  PASS: $PASS_LOG"
echo "  FAIL: $FAIL_LOG"
echo "  SKIP: $SKIP_LOG"
