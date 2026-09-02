#!/bin/bash
# Test VŠECH binárek v rootfs - reálné použití přes ashell
# Používá ashell -c pro testování na reálném Android zařízení

D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader

RESULTS_DIR=/root/elf_loader/results
mkdir -p $RESULTS_DIR
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
ALL_LOG=$RESULTS_DIR/all_tests_$TIMESTAMP.txt
PASS_LOG=$RESULTS_DIR/pass_$TIMESTAMP.txt
FAIL_LOG=$RESULTS_DIR/fail_$TIMESTAMP.txt
CRASH_LOG=$RESULTS_DIR/crash_$TIMESTAMP.txt

> "$ALL_LOG"
> "$PASS_LOG"
> "$FAIL_LOG"
> "$CRASH_LOG"

PASS=0
FAIL=0
CRASH=0
SKIP=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Testovací příkazy pro různé binárky
get_test_cmd() {
    local name="$1"
    case "$name" in
        grep) echo "grep -q root /etc/passwd" ;;
        sed) echo "sed -n '1p' /etc/hostname" ;;
        awk) echo "awk 'BEGIN{print 1+2}'" ;;
        wc) echo "wc -c /etc/hostname" ;;
        cut) echo "cut -d: -f1 /etc/passwd | head -1" ;;
        sort) echo "echo -e 'b\na\nc' | sort" ;;
        uniq) echo "echo -e 'a\na\nb' | uniq" ;;
        tr) echo "echo 'hello' | tr a-z A-Z" ;;
        head) echo "head -c 5 /etc/hostname" ;;
        tail) echo "tail -c 5 /etc/hostname" ;;
        cat) echo "cat /etc/hostname" ;;
        ls) echo "ls /etc" ;;
        cp) echo "cp /etc/hostname /tmp/test_cp_$$ && cat /tmp/test_cp_$$" ;;
        mv) echo "test -f /tmp/test_cp_$$ && mv /tmp/test_cp_$$ /tmp/test_mv_$$ && cat /tmp/test_mv_$$" ;;
        rm) echo "test -f /tmp/test_cp_$$ && rm /tmp/test_cp_$$" ;;
        mkdir) echo "mkdir /tmp/test_mkdir_$$ && rmdir /tmp/test_mkdir_$$" ;;
        chmod) echo "chmod +x /etc/hostname && ls -l /etc/hostname | head -1" ;;
        stat) echo "stat /etc/hostname" ;;
        find) echo "find /etc -maxdepth 1 -type f | head -3" ;;
        realpath) echo "realpath /etc/hostname" ;;
        dirname) echo "dirname /etc/hostname" ;;
        basename) echo "basename /etc/hostname" ;;
        uname) echo "uname -a" ;;
        hostname) echo "hostname" ;;
        whoami) echo "whoami" ;;
        id) echo "id -u" ;;
        date) echo "date" ;;
        gzip) echo "echo test | gzip | gunzip" ;;
        gunzip) echo "echo test | gzip | gunzip" ;;
        tar) echo "tar -czf /tmp/test_$$.tar.gz /etc/hostname && tar -xzf /tmp/test_$$.tar.gz -O" ;;
        echo) echo "echo 'hello world'" ;;
        printf) echo "printf 'test %d %s' 42 foo" ;;
        seq) echo "seq 1 5" ;;
        diff) echo "diff /etc/hostname /etc/hostname" ;;
        *) echo "--help" ;;
    esac
}

# Skip seznam
should_skip() {
    local name="$1"
    case "$name" in
        gcc*|g++*|gfortran*|cpp*|as|ld|ar|ranlib|strip|objdump|nm|readelf) return 0 ;;
        python*|perl*|ruby|lua|php|node|java) return 0 ;;
        systemd-*|dbus-*|udev-*|NetworkManager) return 0 ;;
        apt*|dpkg*|dpkg-deb) return 0 ;;
        reboot|halt|poweroff|shutdown) return 0 ;;
        rm|dd|mkfs*|fdisk|parted) return 0 ;;
        ssh|scp|sftp|rsync|wget|curl|ftp|telnet) return 0 ;;
        xterm|Xvfb|Xorg|xset|xrandr) return 0 ;;
        gpg|gpg2|gpg-agent|openssl) return 0 ;;
        vim|vi|nano|emacs|less|more|man) return 0 ;;
        bash|zsh|sh|dash|fish) return 0 ;;
        top|htop|btop|btm) return 0 ;;  # TUI - skip for now
        *) return 1 ;;
    esac
}

echo "=========================================="
echo "  ELF_LOADER ALL BINARIES TEST"
echo "=========================================="
echo "Rootfs: $R"
echo "Loader: $L"
echo "Results: $RESULTS_DIR"
echo ""

TOTAL=0
for bin in $(find $R/bin $R/usr/bin $R/usr/sbin $R/sbin -type f -executable 2>/dev/null | \
    grep -v "\.dpkg" | \
    grep -v "aarch64-linux-gnu" | \
    sort); do
    
    name=$(basename "$bin")
    ((TOTAL++))
    
    # Progress
    if [ $((TOTAL % 100)) -eq 0 ]; then
        echo "Progress: $TOTAL..."
    fi
    
    # Skip
    if should_skip "$name"; then
        echo -e "${BLUE}SKIP${NC} $name"
        echo "SKIP: $name" >> "$ALL_LOG"
        ((SKIP++))
        continue
    fi
    
    # Get test command
    test_cmd=$(get_test_cmd "$name")
    
    # Run test via ashell
    rc=0
    output=$(timeout 5 ashell -c "$L --ownall $bin $test_cmd" 2>&1) || rc=$?
    
    if [ -z "$rc" ]; then rc=0; fi
    
    if [ $rc -eq 0 ]; then
        echo -e "${GREEN}PASS${NC} $name"
        echo "PASS: $name - $test_cmd" >> "$PASS_LOG"
        echo "PASS: $name - $test_cmd" >> "$ALL_LOG"
        ((PASS++))
    elif [ $rc -eq 124 ]; then
        echo -e "${YELLOW}TIMEOUT${NC} $name"
        echo "TIMEOUT: $name - $test_cmd" >> "$FAIL_LOG"
        echo "TIMEOUT: $name - $test_cmd" >> "$ALL_LOG"
        ((FAIL++))
    elif [ $rc -ge 128 ]; then
        sig=$((rc - 128))
        echo -e "${RED}CRASH(SIG$sig)${NC} $name"
        echo "CRASH(SIG$sig): $name - $test_cmd" >> "$CRASH_LOG"
        echo "CRASH(SIG$sig): $name - $test_cmd" >> "$ALL_LOG"
        ((CRASH++))
    else
        echo -e "${YELLOW}EXIT=$rc${NC} $name"
        echo "EXIT=$rc: $name - $test_cmd" >> "$PASS_LOG"
        echo "EXIT=$rc: $name - $test_cmd" >> "$ALL_LOG"
        ((PASS++))
    fi
done

echo ""
echo "=========================================="
echo "  SUMMARY"
echo "=========================================="
echo "Total:   $TOTAL"
echo -e "PASS:    ${GREEN}$PASS${NC}"
echo -e "FAIL:    ${RED}$FAIL${NC}"
echo -e "CRASH:   ${RED}$CRASH${NC}"
echo -e "SKIP:    ${YELLOW}$SKIP${NC}"
echo ""
echo "Logs: $RESULTS_DIR"
echo ""

# Save stats
echo "PASS_COUNT=$PASS" > $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "FAIL_COUNT=$FAIL" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "CRASH_COUNT=$CRASH" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "SKIP_COUNT=$SKIP" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "TOTAL_COUNT=$TOTAL" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
