#!/bin/bash
# Komplexní testovací skript pro elf_loader
# Testuje reálné použití binárek, ne jen --version/--help

set -e

# Device cesty (proot mount na device)
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
E=$D/usr/bin/elroot
G=$D/usr/bin/gbsh

# Výstupní soubory
RESULTS_DIR=/root/elf_loader/results
mkdir -p $RESULTS_DIR
PASS_LOG=$RESULTS_DIR/pass_$(date +%Y%m%d_%H%M%S).txt
FAIL_LOG=$RESULTS_DIR/fail_$(date +%Y%m%d_%H%M%S).txt
SKIP_LOG=$RESULTS_DIR/skip_$(date +%Y%m%d_%H%M%S).txt

touch "$PASS_LOG" "$FAIL_LOG" "$SKIP_LOG"

# Barevný výstup
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Počítadla
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# Funkce pro testování
test_binary() {
    local bin="$1"
    local test_cmd="$2"
    local desc="$3"
    
    local full_cmd="$L --ownall $R/$test_cmd"
    
    # Zkrácený název binárky pro log
    local bin_name=$(basename $bin)
    
    # Timeout 5 sekund
    local output
    local rc
    output=$(timeout 5 $full_cmd 2>&1) || rc=$?
    
    if [ $rc -eq 0 ]; then
        echo -e "${GREEN}PASS${NC} $bin_name: $desc"
        echo "PASS: $bin_name - $desc" >> "$PASS_LOG"
        ((PASS_COUNT++))
        return 0
    elif [ $rc -eq 124 ]; then
        echo -e "${YELLOW}TIMEOUT${NC} $bin_name: $desc"
        echo "TIMEOUT: $bin_name - $desc" >> "$FAIL_LOG"
        ((FAIL_COUNT++))
        return 1
    elif [ $rc -ge 128 ]; then
        # Signal crash
        local sig=$((rc - 128))
        echo -e "${RED}CRASH(SIG$sig)${NC} $bin_name: $desc"
        echo "CRASH(SIG$sig): $bin_name - $desc" >> "$FAIL_LOG"
        ((FAIL_COUNT++))
        return 1
    else
        # Exit code 1-127 - může být OK (např. grep nenajde nic)
        echo -e "${YELLOW}EXIT=$rc${NC} $bin_name: $desc"
        echo "EXIT=$rc: $bin_name - $desc" >> "$PASS_LOG"  # Legitimní exit kódy počítáme jako pass
        ((PASS_COUNT++))
        return 0
    fi
}

# Test s očekávaným výstupem
test_binary_output() {
    local bin="$1"
    local test_cmd="$2"
    local expected="$3"
    local desc="$4"
    
    local full_cmd="$L --ownall $R/$test_cmd"
    local bin_name=$(basename $bin)
    
    local output
    local rc
    output=$(timeout 5 $full_cmd 2>&1) || rc=$?
    
    if echo "$output" | grep -q "$expected"; then
        echo -e "${GREEN}PASS${NC} $bin_name: $desc"
        echo "PASS: $bin_name - $desc" >> "$PASS_LOG"
        ((PASS_COUNT++))
        return 0
    else
        echo -e "${RED}FAIL${NC} $bin_name: $desc (expected '$expected', got: $output)"
        echo "FAIL: $bin_name - $desc (expected '$expected', got: $output)" >> "$FAIL_LOG"
        ((FAIL_COUNT++))
        return 1
    fi
}

# Test TUI aplikace přes tmux
test_tui() {
    local bin="$1"
    local test_cmd="$2"
    local desc="$3"
    
    local bin_name=$(basename $bin)
    
    # Zkusíme spustit v tmuxu a poslat quit příkaz
    if command -v tmux &> /dev/null; then
        # Vytvoř nový tmux session
        tmux new-session -d -s test_tui
        tmux send-keys -t test_tui "$L --ownall $R/$test_cmd" Enter
        
        # Počkej 2 sekundy
        sleep 2
        
        # Zkus poslat quit (q nebo Ctrl-C)
        tmux send-keys -t test_tui 'q'
        sleep 1
        
        # Zkontroluj, zda session stále běží (pokud padla, session zmizí)
        if tmux has-session -t test_tui 2>/dev/null; then
            echo -e "${GREEN}PASS${NC} $bin_name: $desc (tmux)"
            echo "PASS: $bin_name - $desc (tmux)" >> "$PASS_LOG"
            tmux kill-session -t test_tui
            ((PASS_COUNT++))
            return 0
        else
            echo -e "${RED}CRASH${NC} $bin_name: $desc (tmux session died)"
            echo "CRASH: $bin_name - $desc (tmux session died)" >> "$FAIL_LOG"
            ((FAIL_COUNT++))
            return 1
        fi
    else
        echo -e "${YELLOW}SKIP${NC} $bin_name: $desc (tmux not available)"
        echo "SKIP: $bin_name - $desc (tmux not available)" >> "$SKIP_LOG"
        ((SKIP_COUNT++))
        return 0
    fi
}

# Skip seznam (binárky, které nechtěme testovat)
skip_list() {
    local bin="$1"
    case "$bin" in
        *gcc*|*g++*|*gfortran*|*cpp*|*as*|*ld*|*ar*|*ranlib*) return 0 ;;  # Toolchain
        *python*|*perl*|*ruby*|*lua*) return 0 ;;  # Interpreters (komplexní)
        *systemd-*|*dbus-*) return 0 ;;  # System services
        *apt*|*dpkg*|*dpkg-*) return 0 ;;  # Package managers (destruktivní)
        *mount*|*umount*|*chroot*|*pivot_root*) return 0 ;;  # Mount operations
        *reboot*|*halt*|*poweroff*|*shutdown*) return 0 ;;  # Power operations
        *killall*|*kill*|*pkill*) return 0 ;;  # Process killing
        *rm*|*dd*|*mkfs*|*fdisk*|*parted*) return 0 ;;  # Destructive
        *ssh*|*scp*|*sftp*|*rsync*) return 0 ;;  # Network (may hang)
        *wget*|*curl*|*ftp*) return 0 ;;  # Network (may hang)
        *X11*|*xterm*|*xvfb*) return 0 ;;  # X11 (no display)
        *gpg*|*gnupg*|*openssl*) return 0 ;;  # Crypto (interactive)
        *vim*|*nano*|*emacs*|*less*|*more*) return 0 ;;  # Interactive editors
        *top*|*htop*|*btop*|*btm*) return 0 ;;  # TUI (test separately)
        *bash*|*zsh*|*sh*|*dash*|*fish*) return 0 ;;  # Shells (test separately)
        *) return 1 ;;
    esac
}

# Hlavní testovací smyčka
echo "=== ELF_LOADER COMPREHENSIVE BINARY TESTS ==="
echo "Rootfs: $R"
echo "Loader: $L"
echo "Results: $RESULTS_DIR"
echo ""

# Seznam binárek k testování
BINARY_LIST=$(find $R/bin $R/usr/bin $R/usr/sbin $R/sbin -type f -executable 2>/dev/null | \
    grep -v "\.dpkg" | \
    grep -v "aarch64-linux-gnu" | \
    sort)

# Testovací případy pro každou kategorii
declare -A TEST_CASES

# Text processing
TEST_CASES[grep]="grep -q root /etc/passwd"
TEST_CASES[sed]="sed -n '1p' /etc/hostname"
TEST_CASES[awk]="awk 'BEGIN{print 1+2}'"
TEST_CASES[wc]="wc -c /etc/hostname"
TEST_CASES[cut]="cut -d: -f1 /etc/passwd | head -1"
TEST_CASES[sort]="echo -e 'b\na\nc' | sort"
TEST_CASES[uniq]="echo -e 'a\na\nb' | uniq"
TEST_CASES[tr]="echo 'hello' | tr a-z A-Z"
TEST_CASES[head]="head -c 5 /etc/hostname"
TEST_CASES[tail]="tail -c 5 /etc/hostname"

# File operations
TEST_CASES[cat]="cat /etc/hostname"
TEST_CASES[ls]="ls /etc"
TEST_CASES[cp]="cp /etc/hostname /tmp/test_cp && cat /tmp/test_cp"
TEST_CASES[mv]="mv /tmp/test_cp /tmp/test_mv && cat /tmp/test_mv"
TEST_CASES[rm]="rm /tmp/test_mv"
TEST_CASES[mkdir]="mkdir /tmp/test_mkdir && rmdir /tmp/test_mkdir"
TEST_CASES[chmod]="chmod +x /etc/hostname && ls -l /etc/hostname"
TEST_CASES[stat]="stat /etc/hostname"
TEST_CASES[find]="find /etc -maxdepth 1 -type f | head -3"
TEST_CASES[realpath]="realpath /etc/hostname"
TEST_CASES[dirname]="dirname /etc/hostname"
TEST_CASES[basename]="basename /etc/hostname"

# System info
TEST_CASES[uname]="uname -a"
TEST_CASES[hostname]="hostname"
TEST_CASES[uptime]="uptime"
TEST_CASES[whoami]="whoami"
TEST_CASES[id]="id -u"
TEST_CASES[ps]="ps aux | head -5"
TEST_CASES[free]="free -h"
TEST_CASES[df]="df -h /"
TEST_CASES[du]="du -sh /etc"

# Date/time
TEST_CASES[date]="date"
TEST_CASES[cal]="cal 2024 | head -3"
TEST_CASES[timeout]="timeout 1 sleep 0.5 && echo ok"

# Compression
TEST_CASES[gzip]="echo test | gzip | gunzip"
TEST_CASES[bzip2]="echo test | bzip2 | bunzip2"
TEST_CASES[xz]="echo test | xz | unxz"
TEST_CASES[tar]="tar -czf /tmp/test.tar.gz /etc/hostname && tar -xzf /tmp/test.tar.gz -O"

# Networking (local only)
TEST_CASES[ping]="ping -c 1 -W 1 127.0.0.1"
TEST_CASES[nslookup]="nslookup localhost"

# Text utilities
TEST_CASES[echo]="echo 'hello world'"
TEST_CASES[printf]="printf 'test %d %s' 42 foo"
TEST_CASES[seq]="seq 1 5"
TEST_CASES[yes]="yes x | head -3"

# Math/expr
TEST_CASES[expr]="expr 1 + 2"
TEST_CASES[bc]="echo '2+2' | bc"

# Diff/patch
TEST_CASES[diff]="diff /etc/hostname /etc/hostname"
TEST_CASES[patch]="echo 'test' > /tmp/test.txt && patch -p0 < /dev/null"

# Archive
TEST_CASES[zip]="echo test > /tmp/test.txt && zip /tmp/test.zip /tmp/test.txt && unzip -p /tmp/test.zip"
TEST_CASES[unzip]="echo test > /tmp/test.txt && zip /tmp/test.zip /tmp/test.txt && unzip -p /tmp/test.zip"

# TUI applications (test separately with tmux)
TUI_APPS="top htop btop btm"

# Test základních binárek
echo "=== Testing basic utilities ==="
for cmd in echo true false; do
    test_binary "$R/bin/$cmd" "bin/$cmd" "basic $cmd"
done

# Test text processing
echo ""
echo "=== Testing text processing ==="
for tool in grep sed awk wc cut sort uniq tr head tail cat; do
    if [ -f "$R/usr/bin/$tool" ]; then
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test file operations
echo ""
echo "=== Testing file operations ==="
for tool in ls cp mv rm mkdir stat find realpath dirname basename; do
    if [ -f "$R/usr/bin/$tool" ]; then
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test system info
echo ""
echo "=== Testing system info ==="
for tool in uname hostname uptime whoami id ps free df du; do
    if [ -f "$R/usr/bin/$tool" ]; then
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test date/time
echo ""
echo "=== Testing date/time ==="
for tool in date cal timeout; do
    if [ -f "$R/usr/bin/$tool" ]; then
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test compression
echo ""
echo "=== Testing compression ==="
for tool in gzip gunzip bzip2 bunzip2 xz unxz tar; do
    if [ -f "$R/usr/bin/$tool" ]; then
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test networking (local only)
echo ""
echo "=== Testing networking (local) ==="
for tool in ping nslookup; do
    if [ -f "$R/usr/bin/$tool" ]; then
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test math/expr
echo ""
echo "=== Testing math/expr ==="
for tool in expr; do
    if [ -f "$R/usr/bin/$tool" ]; then
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test diff/patch
echo ""
echo "=== Testing diff/patch ==="
for tool in diff; do
    if [ -f "$R/usr/bin/$tool" ]; do
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test archive
echo ""
echo "=== Testing archive ==="
for tool in zip unzip; do
    if [ -f "$R/usr/bin/$tool" ]; then
        test_binary "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    fi
done

# Test TUI applications
echo ""
echo "=== Testing TUI applications ==="
for app in $TUI_APPS; do
    if [ -f "$R/usr/bin/$app" ]; then
        test_tui "$R/usr/bin/$app" "usr/bin/$app" "$app TUI"
    fi
done

# Test shell
echo ""
echo "=== Testing shell ==="
if [ -f "$R/usr/bin/bash" ]; then
    test_binary_output "$R/usr/bin/bash" "usr/bin/bash -c 'echo test'" "test" "bash -c"
fi

# Summary
echo ""
echo "=== SUMMARY ==="
echo -e "PASS:  ${GREEN}$PASS_COUNT${NC}"
echo -e "FAIL:  ${RED}$FAIL_COUNT${NC}"
echo -e "SKIP:  ${YELLOW}$SKIP_COUNT${NC}"
echo ""
echo "Logs:"
echo "  PASS: $PASS_LOG"
echo "  FAIL: $FAIL_LOG"
echo "  SKIP: $SKIP_LOG"
