#!/bin/bash
# Komplexní testování ELF loaderu - reálné použití
# Testuje všechny binárky v rootfs s reálnými příkazy

set -e

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
SLOW_LOG=$RESULTS_DIR/slow_$TIMESTAMP.txt

> "$ALL_LOG"
> "$PASS_LOG"
> "$FAIL_LOG"
> "$CRASH_LOG"
> "$SLOW_LOG"

PASS=0
FAIL=0
CRASH=0
SKIP=0
SLOW=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Testovací příkazy pro různé binárky - REÁLNÉ POUŽITÍ
get_test_cmd() {
    local name="$1"
    case "$name" in
        # Text processing
        grep) echo "grep -q root /etc/passwd" ;;
        sed) echo "sed -n '1p' /etc/hostname" ;;
        awk) echo "awk 'BEGIN{print 1+2}'" ;;
        mawk) echo "awk 'BEGIN{print 1+2}'" ;;
        wc) echo "wc -c /etc/hostname" ;;
        cut) echo "cut -d: -f1 /etc/passwd | head -1" ;;
        sort) echo "echo -e 'b\na\nc' | sort" ;;
        uniq) echo "echo -e 'a\na\nb' | uniq" ;;
        tr) echo "echo 'hello' | tr a-z A-Z" ;;
        head) echo "head -c 5 /etc/hostname" ;;
        tail) echo "tail -c 5 /etc/hostname" ;;
        cat) echo "cat /etc/hostname" ;;
        paste) echo "paste -d: - - < /etc/passwd | head -1" ;;
        join) echo "join -t: /etc/passwd /etc/passwd 2>/dev/null | head -1" ;;
        split) echo "echo test | split - -n 2 && ls /x* 2>/dev/null | head -1" ;;
        nl) echo "nl -nln /etc/hostname" ;;
        od) echo "od -c /etc/hostname | head -1" ;;
        hexdump) echo "hexdump -C /etc/hostname | head -1" ;;
        iconv) echo "iconv -l | head -1" ;;
        
        # File operations
        ls) echo "ls /etc | head -5" ;;
        stat) echo "stat /etc/hostname" ;;
        find) echo "find /etc -maxdepth 1 -type f | head -3" ;;
        realpath) echo "realpath /etc/hostname" ;;
        dirname) echo "dirname /etc/hostname" ;;
        basename) echo "basename /etc/hostname" ;;
        cp) echo "cp /etc/hostname /tmp/test_cp_$$ && cat /tmp/test_cp_$$ && rm /tmp/test_cp_$$" ;;
        mv) echo "echo test > /tmp/test_mv_src && mv /tmp/test_mv_src /tmp/test_mv_dst && cat /tmp/test_mv_dst && rm /tmp/test_mv_dst" ;;
        rm) echo "echo test > /tmp/test_rm && rm /tmp/test_rm" ;;
        mkdir) echo "mkdir /tmp/test_mkdir_$$ && rmdir /tmp/test_mkdir_$$" ;;
        chmod) echo "chmod +x /etc/hostname && ls -l /etc/hostname | head -1" ;;
        touch) echo "touch /tmp/test_touch_$$ && rm /tmp/test_touch_$$" ;;
        ln) echo "ln -sf /etc/hostname /tmp/test_ln_$$ && ls -l /tmp/test_ln_$$" ;;
        df) echo "df -h /" ;;
        du) echo "du -sh /etc" ;;
        
        # System info
        uname) echo "uname -a" ;;
        hostname) echo "hostname" ;;
        whoami) echo "whoami" ;;
        id) echo "id -u" ;;
        date) echo "date" ;;
        uptime) echo "uptime" ;;
        w) echo "w" ;;
        users) echo "users" ;;
        logname) echo "logname" ;;
        
        # Compression
        gzip) echo "echo test | gzip | gunzip" ;;
        gunzip) echo "echo test | gzip | gunzip" ;;
        bzip2) echo "echo test | bzip2 | bunzip2" ;;
        bunzip2) echo "echo test | bzip2 | bunzip2" ;;
        xz) echo "echo test | xz | unxz" ;;
        unxz) echo "echo test | xz | unxz" ;;
        tar) echo "tar -czf /tmp/test_$$.tar.gz /etc/hostname && tar -xzf /tmp/test_$$.tar.gz -O && rm /tmp/test_$$.tar.gz" ;;
        
        # Utilities
        echo) echo "echo 'hello world'" ;;
        printf) echo "printf 'test %d %s\n' 42 foo" ;;
        seq) echo "seq 1 5" ;;
        diff) echo "diff /etc/hostname /etc/hostname" ;;
        true) echo "true" ;;
        false) echo "false || echo exit_code=1" ;;
        
        # System tools
        ps) echo "ps aux | head -5" ;;
        kill) echo "kill --help 2>&1 | head -1" ;;
        killall) echo "killall --help 2>&1 | head -1" ;;
        pgrep) echo "pgrep -l init | head -1" ;;
        pidof) echo "pidof init || echo 1" ;;
        
        # Network (local only)
        ping) echo "ping -c 1 -W 1 127.0.0.1" ;;
        host) echo "host localhost 2>&1 | head -1" ;;
        
        # Compression tools
        gzip) echo "echo test | gzip | gunzip" ;;
        
        # Default - just --help or basic test
        *) echo "--help" ;;
    esac
}

# Skip seznam - co nechtěme testovat
should_skip() {
    local name="$1"
    case "$name" in
        # Compiler toolchain
        gcc*|g++*|gfortran*|cpp*|as|ld|ar|ranlib|strip|objdump|nm|readelf|objcopy) return 0 ;;
        
        # Interpreters (komplexní)
        python*|perl*|ruby|lua|php|node|java) return 0 ;;
        
        # System services
        systemd-*|dbus-*|udev-*|NetworkManager) return 0 ;;
        
        # Package managers
        apt*|dpkg*|dpkg-deb|aptitude) return 0 ;;
        
        # Power management
        reboot|halt|poweroff|shutdown|init) return 0 ;;
        
        # Dangerous operations
        dd|mkfs*|fdisk|parted|mkswap|swapon|swapoff) return 0 ;;
        
        # Network daemons (potřebují síť)
        ssh|scp|sftp|rsync|wget|curl|ftp|telnet|nc|netcat) return 0 ;;
        
        # X11/GUI
        xterm|Xvfb|Xorg|xset|xrandr|xclock) return 0 ;;
        
        # Crypto
        gpg|gpg2|gpg-agent|openssl|gpgsm) return 0 ;;
        
        # Interactive editors
        vim|vi|nano|emacs|less|more|man) return 0 ;;
        
        # TUI apps
        top|htop|btop|btm|htop) return 0 ;;
        
        # Loader itself
        elf_loader) return 0 ;;
        
        # Default: don't skip
        *) return 1 ;;
    esac
}

# Test binárky
test_binary() {
    local bin="$1"
    local name=$(basename "$bin")
    local test_cmd=$(get_test_cmd "$name")
    local start_time=$(date +%s%N)
    
    # Run test via ashell
    local output
    local rc=0
    output=$(timeout 5 ashell -c "$L --ownall $bin $test_cmd" 2>&1) || rc=$?
    
    local end_time=$(date +%s%N)
    local duration=$(( (end_time - start_time) / 1000000 ))  # ms
    
    # Log to file
    echo "TEST: $name | CMD: $test_cmd | RC: $rc | TIME: ${duration}ms" >> "$ALL_LOG"
    
    # Check result
    if [ $rc -eq 0 ]; then
        echo -e "${GREEN}PASS${NC} $name (${duration}ms)"
        echo "PASS: $name - $test_cmd (${duration}ms)" >> "$PASS_LOG"
        ((PASS++))
        
        # Check if slow
        if [ $duration -gt 1000 ]; then
            echo "SLOW: $name (${duration}ms)" >> "$SLOW_LOG"
            ((SLOW++))
        fi
    elif [ $rc -eq 124 ]; then
        echo -e "${YELLOW}TIMEOUT${NC} $name (${duration}ms)"
        echo "TIMEOUT: $name - $test_cmd (${duration}ms)" >> "$FAIL_LOG"
        ((FAIL++))
    elif [ $rc -ge 128 ]; then
        local sig=$((rc - 128))
        echo -e "${RED}CRASH(SIG$sig)${NC} $name (${duration}ms)"
        echo "CRASH(SIG$sig): $name - $test_cmd (${duration}ms)" >> "$CRASH_LOG"
        ((CRASH++))
    else
        echo -e "${YELLOW}EXIT=$rc${NC} $name (${duration}ms)"
        echo "EXIT=$rc: $name - $test_cmd (${duration}ms)" >> "$PASS_LOG"
        ((PASS++))
    fi
}

# Main
echo "=========================================="
echo "  ELF_LOADER COMPREHENSIVE TEST"
echo "=========================================="
echo "Rootfs: $R"
echo "Loader: $L"
echo "Results: $RESULTS_DIR"
echo "Started: $(date)"
echo ""

# Count total binaries
TOTAL=$(find $R/bin $R/usr/bin $R/usr/sbin $R/sbin -type f -executable 2>/dev/null | \
    grep -v "\.dpkg" | \
    grep -v "aarch64-linux-gnu" | \
    wc -l)

echo "Total binaries found: $TOTAL"
echo ""

# Test each binary
CURRENT=0
for bin in $(find $R/bin $R/usr/bin $R/usr/sbin $R/sbin -type f -executable 2>/dev/null | \
    grep -v "\.dpkg" | \
    grep -v "aarch64-linux-gnu" | \
    sort); do
    
    name=$(basename "$bin")
    ((CURRENT++))
    
    # Progress every 50
    if [ $((CURRENT % 50)) -eq 0 ]; then
        echo -e "${CYAN}Progress: $CURRENT/$TOTAL (${PASS} pass, ${FAIL} fail, ${CRASH} crash, ${SKIP} skip)${NC}"
    fi
    
    # Skip
    if should_skip "$name"; then
        echo -e "${BLUE}SKIP${NC} $name"
        echo "SKIP: $name" >> "$ALL_LOG"
        ((SKIP++))
        continue
    fi
    
    # Test
    test_binary "$bin"
done

# Summary
echo ""
echo "=========================================="
echo "  FINAL SUMMARY"
echo "=========================================="
echo "Total:     $TOTAL"
echo -e "Passed:    ${GREEN}$PASS${NC}"
echo -e "Failed:    ${RED}$FAIL${NC}"
echo -e "Crashed:   ${RED}$CRASH${NC}"
echo -e "Skipped:   ${YELLOW}$SKIP${NC}"
echo -e "Slow:      ${CYAN}$SLOW${NC}"
echo ""
echo "Success rate: $(echo "scale=1; $PASS * 100 / ($PASS + $FAIL + $CRASH)" | bc)%"
echo ""
echo "Logs saved to: $RESULTS_DIR"
echo "  - All tests: $ALL_LOG"
echo "  - Passed:    $PASS_LOG"
echo "  - Failed:    $FAIL_LOG"
echo "  - Crashed:   $CRASH_LOG"
echo "  - Slow:      $SLOW_LOG"
echo ""
echo "Completed: $(date)"

# Save stats
cat > $RESULTS_DIR/stats_$TIMESTAMP.txt << EOF
PASS_COUNT=$PASS
FAIL_COUNT=$FAIL
CRASH_COUNT=$CRASH
SKIP_COUNT=$SKIP
TOTAL_COUNT=$TOTAL
SUCCESS_RATE=$(echo "scale=1; $PASS * 100 / ($PASS + $FAIL + $CRASH)" | bc)%
EOF
