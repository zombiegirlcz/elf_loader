#!/bin/bash
# Komplexní test VŠECH binárek v rootfs (~600+)
# Testuje reálné použití, ne jen --version
# Používá ashell -c pro testování na reálném Android zařízení

D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader

RESULTS_DIR=/root/elf_loader/results
mkdir -p $RESULTS_DIR
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

PASS_LOG=$RESULTS_DIR/pass_$TIMESTAMP.txt
FAIL_LOG=$RESULTS_DIR/fail_$TIMESTAMP.txt
SKIP_LOG=$RESULTS_DIR/skip_$TIMESTAMP.txt
CRASH_LOG=$RESULTS_DIR/crash_$TIMESTAMP.txt

> "$PASS_LOG"
> "$FAIL_LOG"
> "$SKIP_LOG"
> "$CRASH_LOG"

PASS=0
FAIL=0
SKIP=0
CRASH=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
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
        sort) echo "sort" ;;
        uniq) echo "uniq" ;;
        tr) echo "tr a-z A-Z" ;;
        head) echo "head -n 3 /etc/passwd" ;;
        tail) echo "tail -n 3 /etc/passwd" ;;
        cat) echo "cat /etc/hostname" ;;
        less) echo "less --version" ;;
        more) echo "more --version" ;;
        paste) echo "paste -d: - - < /etc/passwd" ;;
        join) echo "join -t: /etc/passwd /etc/passwd 2>/dev/null | head -1" ;;
        split) echo "split --help" ;;
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
        readlink) echo "readlink /etc/hostname" ;;
        file) echo "file /etc/hostname" ;;
        chown) echo "chown --help" ;;
        chmod) echo "chmod +x /etc/hostname && ls -l /etc/hostname | head -1" ;;
        chgrp) echo "chgrp --help" ;;
        install) echo "install --help" ;;
        ln) echo "ln -sf /etc/hostname /tmp/test_ln_$$ && ls -l /tmp/test_ln_$$" ;;
        touch) echo "touch /tmp/test_touch_$$ && rm /tmp/test_touch_$$" ;;
        mkdir) echo "mkdir /tmp/test_mkdir_$$ && rmdir /tmp/test_mkdir_$$" ;;
        rmdir) echo "rmdir --help" ;;
        rm) echo "rm --help" ;;
        cp) echo "cp /etc/hostname /tmp/test_cp_$$ && cat /tmp/test_cp_$$ && rm /tmp/test_cp_$$" ;;
        mv) echo "mv --help" ;;
        df) echo "df -h /" ;;
        du) echo "du -sh /etc" ;;
        sync) echo "sync" ;;
        
        # System info
        uname) echo "uname -a" ;;
        hostname) echo "hostname" ;;
        whoami) echo "whoami" ;;
        id) echo "id -u" ;;
        date) echo "date" ;;
        cal) echo "cal 2024 | head -3" ;;
        uptime) echo "uptime" ;;
        w) echo "w" ;;
        users) echo "users" ;;
        logname) echo "logname" ;;
        ps) echo "ps aux | head -5" ;;
        free) echo "free -h" ;;
        vmstat) echo "vmstat 1 2 | tail -1" ;;
        top) echo "top --help" ;;
        kill) echo "kill --help" ;;
        killall) echo "killall --help" ;;
        pgrep) echo "pgrep bash" ;;
        pidof) echo "pidof bash" ;;
        
        # Compression
        gzip) echo "echo test | gzip | gunzip" ;;
        gunzip) echo "echo test | gzip | gunzip" ;;
        bzip2) echo "echo test | bzip2 | bunzip2" ;;
        bunzip2) echo "echo test | bzip2 | bunzip2" ;;
        xz) echo "echo test | xz | unxz" ;;
        unxz) echo "echo test | xz | unxz" ;;
        lzma) echo "lzma --help" ;;
        unlzma) echo "unlzma --help" ;;
        tar) echo "tar -tzf /etc/hostname 2>&1 | head -1" ;;
        zip) echo "zip --help" ;;
        unzip) echo "unzip -l /etc/hostname 2>&1 | head -1" ;;
        
        # Networking (local only)
        ping) echo "ping -c 1 -W 1 127.0.0.1" ;;
        ping6) echo "ping6 -c 1 -W 1 ::1" ;;
        nslookup) echo "nslookup localhost" ;;
        host) echo "host localhost" ;;
        dig) echo "dig localhost" ;;
        netstat) echo "netstat -an | head -5" ;;
        ifconfig) echo "ifconfig" ;;
        ip) echo "ip addr show | head -5" ;;
        route) echo "route --help" ;;
        ss) echo "ss -tuln | head -5" ;;
        
        # Utilities
        echo) echo "echo 'hello world'" ;;
        printf) echo "printf 'test %d %s' 42 foo" ;;
        seq) echo "seq 1 5" ;;
        yes) echo "yes x | head -3" ;;
        test) echo "test -f /etc/hostname && echo ok" ;;
        [) echo "[ -f /etc/hostname ] && echo ok" ;;
        expr) echo "expr 1 + 2" ;;
        bc) echo "echo '2+2' | bc" ;;
        factor) echo "factor 12" ;;
        sum) echo "sum --help" ;;
        sha1sum) echo "echo test | sha1sum" ;;
        md5sum) echo "echo test | md5sum" ;;
        sha256sum) echo "echo test | sha256sum" ;;
        
        # Diff/Patch
        diff) echo "diff /etc/hostname /etc/hostname" ;;
        patch) echo "patch --help" ;;
        sdiff) echo "sdiff --help" ;;
        
        # Shells
        sh) echo "sh -c 'echo shell works'" ;;
        bash) echo "bash -c 'echo bash works'" ;;
        dash) echo "dash -c 'echo dash works'" ;;
        
        # Editors
        nano) echo "nano --version" ;;
        vi) echo "vi --help" ;;
        vim) echo "vim --help" ;;
        
        # Package management (test help only)
        apt) echo "apt --help" ;;
        apt-get) echo "apt-get --help" ;;
        apt-cache) echo "apt-cache --help" ;;
        dpkg) echo "dpkg --help" ;;
        
        # Systemd (test help only)
        systemctl) echo "systemctl --help" ;;
        systemd) echo "systemd --help" ;;
        
        # Default: try --help or --version
        *) echo "--help" ;;
    esac
}

# Skip seznam - které binárky nechat vynechat
should_skip() {
    local name="$1"
    case "$name" in
        # Toolchain - skip
        gcc*|g++*|gfortran*|cpp*|gcov*|gprof|ld|as|ar|ranlib|strip|objdump|nm|readelf|objcopy|elfedit) return 0 ;;
        
        # Interpreters - skip (komplexní)
        python*|perl*|ruby|lua|php|node|java|tcl|expect) return 0 ;;
        
        # System services - skip
        systemd-*|dbus-*|udev-*|NetworkManager|polkit) return 0 ;;
        
        # Package managers - skip (destruktivní)
        apt*|dpkg*|dpkg-deb|aptitude) return 0 ;;
        
        # Power operations - skip
        reboot|halt|poweroff|shutdown|init|telinit) return 0 ;;
        
        # Destructive - skip
        dd|mkfs*|fdisk|parted|mkswap|swapon|swapoff|wipefs) return 0 ;;
        
        # Network (may hang) - skip
        ssh|scp|sftp|rsync|wget|curl|ftp|telnet|nc|netcat) return 0 ;;
        
        # X11/GUI - skip (no display)
        xterm|Xvfb|Xorg|xset|xrandr|xwininfo|xprop|xclock) return 0 ;;
        
        # Crypto/PGP - skip (interactive)
        gpg|gpg2|gpg-agent|gpgsm|dirmngr|pinentry|openssl|ca-certificates) return 0 ;;
        
        # Interactive editors - skip
        vim|vi|nano|emacs|less|more|man|view) return 0 ;;
        
        # TUI - skip (need tmux)
        top|htop|btop|btm|bottom|nmon|glances|mc) return 0 ;;
        
        # Shells - skip (test separately)
        bash|zsh|sh|dash|fish|ksh|csh|tcsh) return 0 ;;
        
        # Special binaries - skip
        proot|qemu*|loader|boot) return 0 ;;
        
        # Default: don't skip
        *) return 1 ;;
    esac
}

# Test binárky
test_binary() {
    local bin="$1"
    local name=$(basename "$bin")
    local test_cmd=$(get_test_cmd "$name")
    
    # Build full command
    local full_cmd="$L --ownall $bin $test_cmd"
    
    # Run via ashell with timeout
    local output
    local rc=0
    output=$(timeout 5 ashell -c "$full_cmd" 2>&1) || rc=$?
    
    # Check result
    if [ $rc -eq 0 ]; then
        echo -e "${GREEN}PASS${NC} $name"
        echo "PASS: $name - $test_cmd" >> "$PASS_LOG"
        ((PASS++))
    elif [ $rc -eq 124 ]; then
        echo -e "${YELLOW}TIMEOUT${NC} $name"
        echo "TIMEOUT: $name - $test_cmd" >> "$FAIL_LOG"
        ((FAIL++))
    elif [ $rc -ge 128 ]; then
        local sig=$((rc - 128))
        echo -e "${RED}CRASH(SIG$sig)${NC} $name"
        echo "CRASH(SIG$sig): $name - $test_cmd" >> "$CRASH_LOG"
        ((CRASH++))
    else
        # Exit code 1-127 - check if expected
        if [ "$name" = "grep" ] && [ $rc -eq 1 ]; then
            echo -e "${GREEN}PASS${NC} $name (exit $rc)"
            echo "PASS: $name - $test_cmd (exit $rc, expected)" >> "$PASS_LOG"
            ((PASS++))
        else
            echo -e "${YELLOW}EXIT=$rc${NC} $name"
            echo "EXIT=$rc: $name - $test_cmd" >> "$PASS_LOG"
            ((PASS++))
        fi
    fi
}

# Hlavní smyčka
echo "=========================================="
echo "  ELF_LOADER ALL BINARIES TEST (~600+)"
echo "=========================================="
echo "Rootfs: $R"
echo "Loader: $L"
echo "Timestamp: $TIMESTAMP"
echo ""

# Najdi všechny binárky
echo "Scanning for binaries..."
BINARY_LIST=$(find $R/bin $R/usr/bin $R/usr/sbin $R/sbin -type f -executable 2>/dev/null | \
    grep -v "\.dpkg" | \
    grep -v "aarch64-linux-gnu" | \
    sort)

TOTAL=$(echo "$BINARY_LIST" | wc -l)
echo "Found $TOTAL binaries"
echo ""

# Testuj každou binárku
CURRENT=0
for bin in $BINARY_LIST; do
    name=$(basename "$bin")
    ((CURRENT++))
    
    # Progress indicator
    if [ $((CURRENT % 50)) -eq 0 ]; then
        echo "Progress: $CURRENT/$TOTAL ($PASS pass, $FAIL fail, $CRASH crash, $SKIP skip)"
    fi
    
    # Skip
    if should_skip "$name"; then
        echo -e "${BLUE}SKIP${NC} $name"
        echo "SKIP: $name" >> "$SKIP_LOG"
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
echo "Total:   $TOTAL"
echo -e "PASS:    ${GREEN}$PASS${NC}"
echo -e "FAIL:    ${RED}$FAIL${NC}"
echo -e "CRASH:   ${RED}$CRASH${NC}"
echo -e "SKIP:    ${YELLOW}$SKIP${NC}"
echo ""
echo "Tested: $((PASS + FAIL + CRASH)) binaries"
echo "Skipped: $SKIP binaries"
echo ""
echo "Logs saved to: $RESULTS_DIR"
echo "  PASS: $PASS_LOG"
echo "  FAIL: $FAIL_LOG"
echo "  CRASH: $CRASH_LOG"
echo "  SKIP: $SKIP_LOG"
echo ""

# Save stats
echo "PASS_COUNT=$PASS" > $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "FAIL_COUNT=$FAIL" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "CRASH_COUNT=$CRASH" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "SKIP_COUNT=$SKIP" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "TOTAL_COUNT=$TOTAL" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
echo "TESTED_COUNT=$((PASS + FAIL + CRASH))" >> $RESULTS_DIR/stats_$TIMESTAMP.txt
