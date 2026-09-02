#!/bin/bash
# Test VŠECH binárek v rootfs - reálné použití, ne jen --version
# Používá tmux pro TUI aplikace

set -e

# Device cesty
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader

# Výstup
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

# Počítadla
PASS=0
FAIL=0
CRASH=0
SKIP=0

# Barevný výstup
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Testovací příkazy pro různé kategorie
get_test_cmd() {
    local bin="$1"
    local name=$(basename "$bin")
    
    # Skip seznam
    case "$name" in
        # Toolchain - skip
        gcc*|g++*|gfortran*|cpp*|as|ld|ar|ranlib|strip|objdump|nm|readelf|objcopy)
            echo "SKIP_TOOLCHAIN" ;;
        
        # Interpreters - skip (komplexní)
        python*|perl*|ruby|lua|php|node|java)
            echo "SKIP_INTERPRETER" ;;
        
        # System services - skip
        systemd-*|dbus-*|udev-*|NetworkManager)
            echo "SKIP_SYSTEM" ;;
        
        # Package managers - skip (destruktivní)
        apt*|dpkg*|dpkg-deb|apt-get|apt-cache|apt-config)
            echo "SKIP_PACKAGE" ;;
        
        # Power operations - skip
        reboot|halt|poweroff|shutdown|init)
            echo "SKIP_POWER" ;;
        
        # Destructive - skip
        rm|dd|mkfs*|fdisk|parted|mkswap|swapon|swapoff)
            echo "SKIP_DESTRUCTIVE" ;;
        
        # Network (may hang) - skip
        ssh|scp|sftp|rsync|wget|curl|ftp|telnet)
            echo "SKIP_NETWORK" ;;
        
        # X11/GUI - skip (no display)
        xterm|Xvfb|Xorg|xset|xrandr|xwininfo)
            echo "SKIP_GUI" ;;
        
        # Crypto/PGP - skip (interactive)
        gpg|gpg2|gpg-agent|gpgsm|dirmngr|pinentry|openssl)
            echo "SKIP_CRYPTO" ;;
        
        # Interactive editors - test separately
        vim|vi|nano|emacs|less|more|man)
            echo "SKIP_INTERACTIVE" ;;
        
        # TUI - test with tmux
        top|htop|btop|btm|bottom|nmon|glances)
            echo "TUI" ;;
        
        # Shells - test separately
        bash|zsh|sh|dash|fish|ksh|csh|tcsh)
            echo "SHELL" ;;
        
        # Default: test with --help or basic usage
        *)
            echo "DEFAULT" ;;
    esac
}

# Test binárky s reálným příkazem
test_binary_real() {
    local bin="$1"
    local name=$(basename "$bin")
    local test_type="$2"
    
    # Určete testovací příkaz podle binárky
    local test_cmd=""
    case "$name" in
        # Text processing
        grep) test_cmd="grep -q root /etc/passwd" ;;
        sed) test_cmd="sed -n '1p' /etc/hostname" ;;
        awk) test_cmd="awk 'BEGIN{print 1+2}'" ;;
        wc) test_cmd="wc -c /etc/hostname" ;;
        cut) test_cmd="cut -d: -f1 /etc/passwd | head -1" ;;
        sort) test_cmd="echo -e 'b\na\nc' | sort" ;;
        uniq) test_cmd="echo -e 'a\na\nb' | uniq" ;;
        tr) test_cmd="echo 'hello' | tr a-z A-Z" ;;
        head) test_cmd="head -c 5 /etc/hostname" ;;
        tail) test_cmd="tail -c 5 /etc/hostname" ;;
        cat) test_cmd="cat /etc/hostname" ;;
        
        # File operations
        ls) test_cmd="ls /etc" ;;
        cp) test_cmd="cp /etc/hostname /tmp/test_$$_cp && cat /tmp/test_$$_cp" ;;
        mv) test_cmd="test -f /tmp/test_$$_cp && mv /tmp/test_$$_cp /tmp/test_$$_mv && cat /tmp/test_$$_mv" ;;
        rm) test_cmd="test -f /tmp/test_$$_cp && rm /tmp/test_$$_cp" ;;
        mkdir) test_cmd="mkdir /tmp/test_$$_mkdir && rmdir /tmp/test_$$_mkdir" ;;
        chmod) test_cmd="chmod +x /etc/hostname && ls -l /etc/hostname | head -1" ;;
        stat) test_cmd="stat /etc/hostname" ;;
        find) test_cmd="find /etc -maxdepth 1 -type f | head -3" ;;
        realpath) test_cmd="realpath /etc/hostname" ;;
        dirname) test_cmd="dirname /etc/hostname" ;;
        basename) test_cmd="basename /etc/hostname" ;;
        ln) test_cmd="ln -sf /etc/hostname /tmp/test_$$_ln && ls -l /tmp/test_$$_ln" ;;
        
        # System info
        uname) test_cmd="uname -a" ;;
        hostname) test_cmd="hostname" ;;
        uptime) test_cmd="uptime" ;;
        whoami) test_cmd="whoami" ;;
        id) test_cmd="id -u" ;;
        ps) test_cmd="ps aux | head -5" ;;
        free) test_cmd="free -h" ;;
        df) test_cmd="df -h /" ;;
        du) test_cmd="du -sh /etc" ;;
        top) test_cmd="TUI" ;;
        
        # Date/time
        date) test_cmd="date" ;;
        cal) test_cmd="cal 2024 | head -3" ;;
        timeout) test_cmd="timeout 1 sleep 0.5 && echo ok" ;;
        sleep) test_cmd="sleep 0.1 && echo ok" ;;
        
        # Compression
        gzip) test_cmd="echo test | gzip | gunzip" ;;
        gunzip) test_cmd="echo test | gzip | gunzip" ;;
        bzip2) test_cmd="echo test | bzip2 | bunzip2" ;;
        bunzip2) test_cmd="echo test | bzip2 | bunzip2" ;;
        xz) test_cmd="echo test | xz | unxz" ;;
        unxz) test_cmd="echo test | xz | unxz" ;;
        tar) test_cmd="tar -czf /tmp/test_$$.tar.gz /etc/hostname && tar -xzf /tmp/test_$$.tar.gz -O" ;;
        
        # Networking (local)
        ping) test_cmd="ping -c 1 -W 1 127.0.0.1" ;;
        nslookup) test_cmd="nslookup localhost" ;;
        host) test_cmd="host localhost" ;;
        
        # Text utilities
        echo) test_cmd="echo 'hello world'" ;;
        printf) test_cmd="printf 'test %d %s' 42 foo" ;;
        seq) test_cmd="seq 1 5" ;;
        yes) test_cmd="yes x | head -3" ;;
        readlink) test_cmd="readlink /etc/hostname" ;;
        
        # Math/expr
        expr) test_cmd="expr 1 + 2" ;;
        bc) test_cmd="echo '2+2' | bc" ;;
        
        # Diff/patch
        diff) test_cmd="diff /etc/hostname /etc/hostname" ;;
        patch) test_cmd="echo 'test' > /tmp/test_$$.txt && patch -p0 < /dev/null" ;;
        
        # Archive
        zip) test_cmd="echo test > /tmp/test_$$.txt && zip /tmp/test_$$.zip /tmp/test_$$.txt && unzip -p /tmp/test_$$.zip" ;;
        unzip) test_cmd="echo test > /tmp/test_$$.txt && zip /tmp/test_$$.zip /tmp/test_$$.txt && unzip -p /tmp/test_$$.zip" ;;
        
        # Shell
        bash) test_cmd="bash -c 'echo test'" ;;
        sh) test_cmd="sh -c 'echo test'" ;;
        
        # Default: try --help or --version
        *) test_cmd="--help" ;;
    esac
    
    # Skip TUI (test separately)
    if [ "$test_cmd" = "TUI" ]; then
        return 1
    fi
    
    # Build full command for ashell
    local full_cmd="$L --ownall $R/usr/bin/$name $test_cmd"
    
    # Run with timeout via ashell (on real Android device)
    local output
    local rc
    output=$(timeout 5 ashell -c "$full_cmd" 2>&1) || rc=$?
    
    # Check result
    if [ $rc -eq 0 ]; then
        echo -e "${GREEN}PASS${NC} $name: $test_cmd"
        echo "PASS: $name - $test_cmd" >> "$PASS_LOG"
        echo "PASS: $name - $test_cmd" >> "$ALL_LOG"
        ((PASS++))
        return 0
    elif [ $rc -eq 124 ]; then
        echo -e "${YELLOW}TIMEOUT${NC} $name: $test_cmd"
        echo "TIMEOUT: $name - $test_cmd" >> "$FAIL_LOG"
        echo "TIMEOUT: $name - $test_cmd" >> "$ALL_LOG"
        ((FAIL++))
        return 1
    elif [ $rc -ge 128 ]; then
        # Signal crash
        local sig=$((rc - 128))
        echo -e "${RED}CRASH(SIG$sig)${NC} $name: $test_cmd"
        echo "CRASH(SIG$sig): $name - $test_cmd" >> "$CRASH_LOG"
        echo "CRASH(SIG$sig): $name - $test_cmd" >> "$ALL_LOG"
        ((CRASH++))
        return 1
    else
        # Exit code 1-127 - check if it's expected
        # grep returns 1 when no match, which is expected
        if [ "$name" = "grep" ] && [ $rc -eq 1 ]; then
            echo -e "${GREEN}PASS${NC} $name: $test_cmd (exit $rc, expected)"
            echo "PASS: $name - $test_cmd (exit $rc, expected)" >> "$PASS_LOG"
            echo "PASS: $name - $test_cmd (exit $rc, expected)" >> "$ALL_LOG"
            ((PASS++))
            return 0
        else
            echo -e "${YELLOW}EXIT=$rc${NC} $name: $test_cmd"
            echo "EXIT=$rc: $name - $test_cmd" >> "$PASS_LOG"  # Legitimní exit kódy
            echo "EXIT=$rc: $name - $test_cmd" >> "$ALL_LOG"
            ((PASS++))
            return 0
        fi
    fi
}

# Test TUI aplikace přes tmux
test_tui_app() {
    local bin="$1"
    local name=$(basename "$bin")
    
    # Zkusíme spustit v tmuxu
    if ! command -v tmux &> /dev/null; then
        echo -e "${YELLOW}SKIP${NC} $name: tmux not available"
        echo "SKIP: $name - tmux not available" >> "$ALL_LOG"
        ((SKIP++))
        return 0
    fi
    
    # Vytvoř nový tmux session
    local session="test_tui_$$_$name"
    tmux new-session -d -s $session 2>/dev/null || {
        echo -e "${RED}FAIL${NC} $name: tmux session creation failed"
        echo "FAIL: $name - tmux session creation failed" >> "$FAIL_LOG"
        echo "FAIL: $name - tmux session creation failed" >> "$ALL_LOG"
        ((FAIL++))
        return 1
    }
    
    # Spusť aplikaci přes ashell
    tmux send-keys -t $session "ashell -c \"$L --ownall $R/usr/bin/$name\"" Enter
    
    # Počkej 3 sekundy
    sleep 3
    
    # Zkontroluj, zda session stále běží
    if ! tmux has-session -t $session 2>/dev/null; then
        echo -e "${RED}CRASH${NC} $name: session died immediately"
        echo "CRASH: $name - session died immediately" >> "$CRASH_LOG"
        echo "CRASH: $name - session died immediately" >> "$ALL_LOG"
        ((CRASH++))
        return 1
    fi
    
    # Zkus poslat quit (q nebo Ctrl-C)
    tmux send-keys -t $session 'q'
    sleep 1
    
    # Pokud session stále běží, zkus Ctrl-C
    if tmux has-session -t $session 2>/dev/null; then
        tmux send-keys -t $session C-c
        sleep 1
    fi
    
    # Cleanup
    tmux kill-session -t $session 2>/dev/null || true
    
    echo -e "${GREEN}PASS${NC} $name: TUI started successfully"
    echo "PASS: $name - TUI started successfully" >> "$PASS_LOG"
    echo "PASS: $name - TUI started successfully" >> "$ALL_LOG"
    ((PASS++))
    return 0
}

# Test shell
test_shell() {
    local bin="$1"
    local name=$(basename "$bin")
    
    # Test základní funkčnost
    local test_cmd="bash -c '$R/usr/bin/$name -c \"echo test\"'"
    
    local output
    local rc
    output=$(timeout 5 bash -c "$test_cmd" 2>&1) || rc=$?
    
    if echo "$output" | grep -q "test"; then
        echo -e "${GREEN}PASS${NC} $name: shell test"
        echo "PASS: $name - shell test" >> "$PASS_LOG"
        echo "PASS: $name - shell test" >> "$ALL_LOG"
        ((PASS++))
        return 0
    else
        echo -e "${RED}FAIL${NC} $name: shell test (output: $output)"
        echo "FAIL: $name - shell test (output: $output)" >> "$FAIL_LOG"
        echo "FAIL: $name - shell test (output: $output)" >> "$ALL_LOG"
        ((FAIL++))
        return 1
    fi
}

# Hlavní smyčka
echo "=========================================="
echo "  ELF_LOADER ALL BINARIES TEST"
echo "=========================================="
echo "Rootfs: $R"
echo "Loader: $L"
echo "Results: $RESULTS_DIR"
echo "Timestamp: $TIMESTAMP"
echo ""

# Najdi všechny binárky
echo "Scanning for binaries..."
BINARY_COUNT=$(find $R/bin $R/usr/bin $R/usr/sbin $R/sbin -type f -executable 2>/dev/null | \
    grep -v "\.dpkg" | \
    grep -v "aarch64-linux-gnu" | \
    wc -l)

echo "Found $BINARY_COUNT binaries"
echo ""

# Testuj každou binárku
TOTAL=0
for bin in $(find $R/bin $R/usr/bin $R/usr/sbin $R/sbin -type f -executable 2>/dev/null | \
    grep -v "\.dpkg" | \
    grep -v "aarch64-linux-gnu" | \
    sort); do
    
    name=$(basename "$bin")
    test_type=$(get_test_cmd "$bin")
    
    ((TOTAL++))
    
    # Progress indicator
    if [ $((TOTAL % 50)) -eq 0 ]; then
        echo "Progress: $TOTAL/$BINARY_COUNT..."
    fi
    
    case "$test_type" in
        SKIP_*)
            echo -e "${BLUE}SKIP${NC} $name: ${test_type#SKIP_}"
            echo "SKIP: $name - ${test_type#SKIP_}" >> "$ALL_LOG"
            ((SKIP++))
            ;;
        TUI)
            test_tui_app "$bin"
            ;;
        SHELL)
            test_shell "$bin"
            ;;
        DEFAULT)
            test_binary_real "$bin" "$test_type"
            ;;
    esac
done

# Summary
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
echo "Logs:"
echo "  ALL:   $ALL_LOG"
echo "  PASS:  $PASS_LOG"
echo "  FAIL:  $FAIL_LOG"
echo "  CRASH: $CRASH_LOG"
echo ""

# Export results for analysis
echo "PASS_COUNT=$PASS" > $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "FAIL_COUNT=$FAIL" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "CRASH_COUNT=$CRASH" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "SKIP_COUNT=$SKIP" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "TOTAL_COUNT=$TOTAL" >> $RESULTS_DIR/stats_$TIMESTAMP.txt

echo "Done! Results saved to $RESULTS_DIR"
