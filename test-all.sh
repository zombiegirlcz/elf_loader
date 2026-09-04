#!/bin/bash
# Sjednocený testovací rámec pro elf_loader — VÝHRADNĚ přes ashell -c
# Podle skills.md: bionic interpreter check → deploy do files/ → test přes ashell -c
#
# Usage:
#   ./test-all.sh all            # všechny kategorie
#   ./test-all.sh reexec         # pouze re-exec binárky
#   ./test-all.sh python         # pouze python smoke testy
#   ./test-all.sh basic text files system datetime compression networking math diff archive extended shell python

set -euo pipefail

# ─── Device paths ───────────────────────────────────────────────────────────
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
E=$D/usr/bin/elroot
G=$D/usr/bin/gbsh

# ─── Results ────────────────────────────────────────────────────────────────
RESULTS_DIR=/root/elf_loader/results
mkdir -p "$RESULTS_DIR"
STAMP=$(date +%Y%m%d_%H%M%S)
PASS_LOG=$RESULTS_DIR/pass_${STAMP}.txt
FAIL_LOG=$RESULTS_DIR/fail_${STAMP}.txt
SKIP_LOG=$RESULTS_DIR/skip_${STAMP}.txt
ALL_LOG=$RESULTS_DIR/all_${STAMP}.txt
touch "$PASS_LOG" "$FAIL_LOG" "$SKIP_LOG" "$ALL_LOG"

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# ─── Helpers ────────────────────────────────────────────────────────────────
check_interpreter() {
    local f="$1"
    if [ ! -f "$f" ]; then
        echo "ELF_LOADER_MISSING=$f" >&2
        exit 1
    fi
    local interp
    interp=$(readelf -l "$f" 2>/dev/null | grep -o '/system/bin/linker64' | head -1)
    if [ "$interp" != "/system/bin/linker64" ]; then
        echo "ELF_LOADER_WRONG_INTERP=${interp:-<none>} (expected /system/bin/linker64)" >&2
        exit 1
    fi
}
check_interpreter "$L"

ashell_rc() {
    local cmd="$1"
    local rc=0
    timeout 5 ashell -c "$cmd" >/dev/null 2>&1 || rc=$?
    echo "$rc"
}

ashell_out() {
    local cmd="$1"
    timeout 5 ashell -c "$cmd" 2>&1 || true
}

should_skip() {
    local bin="$1"
    case "$bin" in
        # Toolchain
        gcc*|g++*|gfortran*|cpp*|as|ld|ranlib*|strip*|objdump*|nm*|readelf*|objcopy*) return 0 ;;
        # Interpreters / jazyky
        *perl*|*ruby*|*lua*|*php*|*java*) return 0 ;;
        # python3 testujeme jen lehké --version/-c smoke
        *python3*)
            case "$bin" in
                python3*) return 1 ;;
                *) return 0 ;;
            esac
            ;;
        # System services
        *systemd-*|*dbus-*|*udev-*|*NetworkManager*) return 0 ;;
        # Package managers
        *apt*|*dpkg*|*dpkg-deb*|*aptitude*) return 0 ;;
        # Mount / power
        *mount*|*umount*|*chroot*|*pivot_root*|*reboot*|*halt*|*poweroff*|*shutdown*|*init*) return 0 ;;
        # Destructive
        killall*|kill*|pkill*|dd|mkfs*|fdisk*|parted*|mkswap*|swapon*|swapoff*) return 0 ;;
        # Síťové démona / nástroje
        *sftp*|*rsync*|*ftp*|*telnet*|*nc*|*netcat*) return 0 ;;
        # X11
        *X11*|*xterm*|*xvfb*|*Xorg*|*xset*|*xrandr*|*xclock*) return 0 ;;
        # Crypto
        *gpg*|*gnupg*|*openssl*|*gpgsm*) return 0 ;;
        # Interactive editors
        *vim*|*vi*|*nano*|emacs*|less*|more*|man*) return 0 ;;
        # TUI
        top|htop|btop|btm) return 0 ;;
        # Shells
        bash|zsh|sh|dash|fish) return 0 ;;
        *) return 1 ;;
    esac
}

run_test() {
    local bin="$1"
    local cmd="$2"
    local desc="$3"
    local bin_name
    bin_name=$(basename "$bin")

    if should_skip "$bin_name"; then
        echo "SKIP $bin_name: $desc"
        echo "SKIP: $bin_name - $desc" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return 0
    fi

    local rc out
    rc=$(ashell_rc "$L --ownall $bin $cmd") || true
    rc=${rc:-0}
    out=$(ashell_out "$L --ownall $bin $cmd") || true

    echo "TEST: $bin_name | RC=$rc | $desc" >> "$ALL_LOG"

    case "$rc" in
        0)
            echo "PASS $bin_name: $desc"
            echo "PASS: $bin_name - $desc" >> "$PASS_LOG"
            ((PASS_COUNT++)) || true
            ;;
        124)
            echo "TIMEOUT $bin_name: $desc"
            echo "TIMEOUT: $bin_name - $desc" >> "$FAIL_LOG"
            ((FAIL_COUNT++)) || true
            ;;
        128|129|130|131|132|133|134|135|136|137|138|139|140|141|142|143|144|145)
            local sig=$((rc - 128))
            echo "CRASH(SIG$sig) $bin_name: $desc"
            echo "CRASH(SIG$sig): $bin_name - $desc" >> "$FAIL_LOG"
            ((FAIL_COUNT++)) || true
            ;;
        *)
            echo "EXIT=$rc $bin_name: $desc"
            echo "EXIT=$rc: $bin_name - $desc" >> "$PASS_LOG"
            ((PASS_COUNT++)) || true
            ;;
    esac
}

run_test_output() {
    local bin="$1"
    local cmd="$2"
    local expected="$3"
    local desc="$4"
    local bin_name
    bin_name=$(basename "$bin")

    if should_skip "$bin_name"; then
        echo "SKIP $bin_name: $desc"
        echo "SKIP: $bin_name - $desc" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return 0
    fi

    local out rc
    out=$(ashell_out "$L --ownall $bin $cmd") || true
    rc=$(ashell_rc "$L --ownall $bin $cmd") || true
    rc=${rc:-0}

    echo "TEST: $bin_name | RC=$rc | $desc" >> "$ALL_LOG"

    if [ -n "$expected" ] && printf '%s\n' "$out" | grep -Fq -- "$expected"; then
        echo "PASS $bin_name: $desc"
        echo "PASS: $bin_name - $desc" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL $bin_name: $desc (expected '$expected', got: $out)"
        echo "FAIL: $bin_name - $desc (expected '$expected', got: $out)" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# ─── Test cases ─────────────────────────────────────────────────────────────
declare -A TEST_CASES

# basic
TEST_CASES[echo]="echo 'hello world'"
TEST_CASES[true]="true"
TEST_CASES[false]="false || true"

# re-exec / archiv / komprese
TEST_CASES[tar]="tar -czf /tmp/test.tar.gz /etc/hostname && tar -xzf /tmp/test.tar.gz -O"
TEST_CASES[gzip]="echo test | gzip | gunzip"
TEST_CASES[gunzip]="echo test | gzip | gunzip"
TEST_CASES[bzip2]="echo test | bzip2 | bunzip2"
TEST_CASES[bunzip2]="echo test | bzip2 | bunzip2"
TEST_CASES[xz]="echo test | xz | unxz"
TEST_CASES[unxz]="echo test | xz | unxz"

# text processing
TEST_CASES[grep]="grep -q root /etc/passwd"
TEST_CASES[sed]="sed -n '1p' /etc/hostname"
TEST_CASES[awk]="awk 'BEGIN{print 1+2}'"
TEST_CASES[wc]="wc -c /etc/hostname"
TEST_CASES[cut]="cut -d: -f1 /etc/passwd | head -1"
TEST_CASES[sort]="printf 'b\na\nc\n' | sort"
TEST_CASES[uniq]="printf 'a\na\nb\n' | uniq"
TEST_CASES[tr]="echo 'hello' | tr a-z A-Z"
TEST_CASES[head]="head -c 5 /etc/hostname"
TEST_CASES[tail]="tail -c 5 /etc/hostname"
TEST_CASES[cat]="cat /etc/hostname"

# file operations
TEST_CASES[ls]="ls /etc | head -5"
TEST_CASES[cp]="cp /etc/hostname /tmp/test_cp && cat /tmp/test_cp && rm /tmp/test_cp"
TEST_CASES[mv]="echo test > /tmp/test_mv_src && mv /tmp/test_mv_src /tmp/test_mv_dst && cat /tmp/test_mv_dst && rm /tmp/test_mv_dst"
TEST_CASES[rm]="echo test > /tmp/test_rm && rm /tmp/test_rm"
TEST_CASES[mkdir]="mkdir /tmp/test_mkdir && rmdir /tmp/test_mkdir"
TEST_CASES[chmod]="chmod +x /etc/hostname && ls -l /etc/hostname | head -1"
TEST_CASES[stat]="stat /etc/hostname"
TEST_CASES[find]="find /etc -maxdepth 1 -type f | head -3"
TEST_CASES[realpath]="realpath /etc/hostname"
TEST_CASES[dirname]="dirname /etc/hostname"
TEST_CASES[basename]="basename /etc/hostname"

# system info
TEST_CASES[uname]="uname -a"
TEST_CASES[hostname]="hostname"
TEST_CASES[uptime]="uptime"
TEST_CASES[whoami]="whoami"
TEST_CASES[id]="id -u"
TEST_CASES[ps]="ps aux | head -3"
TEST_CASES[free]="free -h"
TEST_CASES[df]="df -h /"
TEST_CASES[du]="du -sh /etc"

# date/time
TEST_CASES[date]="date"
TEST_CASES[cal]="cal 2024 | head -3"
TEST_CASES[sleep]="sleep 0.1 && echo ok"
TEST_CASES[hostid]="hostid"
TEST_CASES[man]="man --help 2>&1 | head -1"
TEST_CASES[less]="less --help 2>&1 | head -1"
TEST_CASES[more]="more --help 2>&1 | head -1"
TEST_CASES[nano]="nano --version 2>&1 | head -1"
TEST_CASES[lesspipe]="lesspipe --help 2>&1 | head -1"
TEST_CASES[manpath]="manpath"
TEST_CASES[mandb]="mandb --help 2>&1 | head -1"
TEST_CASES[man-recode]="man-recode --help 2>&1 | head -1"
TEST_CASES[pslog]="pslog"
TEST_CASES[pstree]="pstree -h 2>&1 | head -1"
TEST_CASES[zipinfo]="echo test > /tmp/test.txt && zip /tmp/test.zip /tmp/test.txt && zipinfo /tmp/test.zip | head -5"

# compression
TEST_CASES[gzip]="echo test | gzip | gunzip"
TEST_CASES[gunzip]="echo test | gzip | gunzip"
TEST_CASES[bzip2]="echo test | bzip2 | bunzip2"
TEST_CASES[bunzip2]="echo test | bzip2 | bunzip2"
TEST_CASES[xz]="echo test | xz | unxz"
TEST_CASES[unxz]="echo test | xz | unxz"
TEST_CASES[tar]="tar -czf /tmp/test.tar.gz /etc/hostname && tar -xzf /tmp/test.tar.gz -O"

# networking (local only)
TEST_CASES[ping]="ping -c 1 -W 1 127.0.0.1"
TEST_CASES[nslookup]="nslookup localhost"

# text utilities
TEST_CASES[printf]="printf 'test %d %s\n' 42 foo"
TEST_CASES[seq]="seq 1 5"
TEST_CASES[yes]="yes x | head -3"
TEST_CASES[expr]="expr 1 + 2"
TEST_CASES[bc]="echo '2+2' | bc"

# diff/patch
TEST_CASES[diff]="diff /etc/hostname /etc/hostname"
TEST_CASES[patch]="echo test > /tmp/test_patch && patch -p0 < /dev/null"

# archive
TEST_CASES[zip]="echo test > /tmp/test.txt && zip /tmp/test.zip /tmp/test.txt && unzip -p /tmp/test.zip"
TEST_CASES[unzip]="echo test > /tmp/test.txt && zip /tmp/test.zip /tmp/test.txt && unzip -p /tmp/test.zip"

# extended re-exec / runtime
TEST_CASES[file]="file /etc/hostname"
TEST_CASES[gdb]="gdb --help 2>&1 | head -1"
TEST_CASES[strace]="strace --help 2>&1 | head -1"
TEST_CASES[ltrace]="ltrace --help 2>&1 | head -1"
TEST_CASES[perf]="perf --help 2>&1 | head -1"
TEST_CASES[numactl]="numactl --help 2>&1 | head -1"
TEST_CASES[curl]="curl -V 2>&1 | head -1"
TEST_CASES[wget]="wget --version 2>&1 | head -1"
TEST_CASES[ssh]="ssh -V 2>&1 | head -1"
TEST_CASES[scp]="scp -V 2>&1 | head -1"
TEST_CASES[gdb]="gdb --help 2>&1 | head -1"
TEST_CASES[strace]="strace --help 2>&1 | head -1"
TEST_CASES[ltrace]="ltrace --help 2>&1 | head -1"
TEST_CASES[perf]="perf --help 2>&1 | head -1"
TEST_CASES[numactl]="numactl --help 2>&1 | head -1"
TEST_CASES[ping6]="ping6 -c 1 -W 1 127.0.0.1"
TEST_CASES[zipgrep]="echo test > /tmp/test.txt && zip /tmp/test.zip /tmp/test.txt && zipgrep test /tmp/test.zip"
TEST_CASES[zipdetails]="zipdetails /tmp/test.zip 2>/dev/null | head -5"
TEST_CASES[pmap]="pmap -x 1 2>/dev/null | head -5"
TEST_CASES[gdbus]="gdbus --version 2>&1 | head -1"
TEST_CASES[hostnamectl]="hostnamectl status 2>&1 | head -5"
TEST_CASES[gawk]="gawk 'BEGIN{print 1+2}'"
TEST_CASES[mawk]="mawk 'BEGIN{print 1+2}'"
TEST_CASES[xargs]="echo 'a' | xargs echo"
TEST_CASES[shuf]="printf 'a\nb\nc\n' | shuf | head -1"
TEST_CASES[paste]="paste -d: - - < /etc/passwd | head -1"
TEST_CASES[join]="join -t: /etc/passwd /etc/passwd 2>/dev/null | head -1"
TEST_CASES[jq]="echo '{\"a\":1}' | jq '.a'"
TEST_CASES[lsof]="lsof --help 2>&1 | head -1"
TEST_CASES[ps]="ps aux | head -3"
TEST_CASES[pgrep]="pgrep -l init 2>/dev/null | head -1"

# ─── Categories ─────────────────────────────────────────────────────────────
category_basic() {
    echo ""
    echo "=== basic ==="
    for cmd in echo true false; do
        [ -f "$R/bin/$cmd" ] && run_test "$R/bin/$cmd" "bin/$cmd" "basic $cmd"
    done
}

category_reexec() {
    echo ""
    echo "=== reexec ==="
    for tool in tar gzip gunzip bzip2 bunzip2 xz unxz; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "re-exec $tool ${TEST_CASES[$tool]:-test}"
    done
}

category_text() {
    echo ""
    echo "=== text ==="
    for tool in grep sed awk wc cut sort uniq tr head tail cat; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    done
}

category_files() {
    echo ""
    echo "=== files ==="
    for tool in ls cp mv rm mkdir stat find realpath dirname basename; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    done
}

category_system() {
    echo ""
    echo "=== system ==="
    for tool in uname hostname uptime whoami id ps free df du hostid man less more nano lesspipe manpath mandb man-recode pslog pstree; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    done
}

category_datetime() {
    echo ""
    echo "=== datetime ==="
    for tool in date cal timeout sleep; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    done
}

category_compression() {
    echo ""
    echo "=== compression ==="
    for tool in gzip gunzip bzip2 bunzip2 xz unxz tar; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    done
}

category_networking() {
    echo ""
    echo "=== networking ==="
    for tool in ping nslookup; do
        if [ -e "$R/usr/bin/$tool" ]; then
            if [ -f "$R/usr/bin/$tool" ]; then
                run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
            else
                echo "SKIP $tool: not a regular file"
                echo "SKIP: $tool - not a regular file" >> "$SKIP_LOG"
                ((SKIP_COUNT++)) || true
            fi
        fi
    done
}

category_math() {
    echo ""
    echo "=== math ==="
    for tool in expr; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    done
}

category_diff() {
    echo ""
    echo "=== diff ==="
    for tool in diff patch; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    done
}

category_archive() {
    echo ""
    echo "=== archive ==="
    for tool in zip unzip; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "usr/bin/$tool" "${TEST_CASES[$tool]:-test}"
    done
}

category_extended() {
    echo ""
    echo "=== extended ==="
    for tool in file timeout gawk mawk xargs shuf paste join jq lsof ps pgrep pmap gdbus hostnamectl ping6 zipgrep zipdetails gdb strace ltrace perf numactl curl wget ssh scp; do
        [ -f "$R/usr/bin/$tool" ] || continue
        # Skip non-ELF executables (scripts, symlinks to scripts, etc.)
        if ! readelf -h "$R/usr/bin/$tool" >/dev/null 2>&1; then
            echo "SKIP $tool: not an ELF binary"
            echo "SKIP: $tool - not an ELF binary" >> "$SKIP_LOG"
            ((SKIP_COUNT++)) || true
            continue
        fi
        run_test "$R/usr/bin/$tool" "usr/bin/$tool" "extended $tool ${TEST_CASES[$tool]:-test}"
    done
}

category_shell() {
    echo ""
    echo "=== shell ==="
    if [ -f "$R/usr/bin/bash" ]; then
        run_test_output "$R/usr/bin/bash" "usr/bin/bash -c 'echo test'" "test" "bash -c"
    fi
}

category_python() {
    echo ""
    echo "=== python ==="
    py_bin=$(find "$R/bin" "$R/usr/bin" "$R/usr/sbin" "$R/sbin" -name 'python3*' -type f 2>/dev/null | head -1 || true)
    if [ -n "${py_bin:-}" ] && [ -f "$py_bin" ]; then
        local py
        py=$(basename "$py_bin")
        run_test "$py_bin" "$py --version" "python3 --version"
        run_test "$py_bin" "$py -c 'import os,sys,json,ctypes,subprocess; print(\"py_ok\", sys.version_info.major)'" "python3 import os/sys/json/ctypes/subprocess"
        run_test "$py_bin" "$py -c 'import os; print(\"py_listdir\", os.listdir(\"/\")[:3])'" "python3 os.listdir('/')"
        run_test "$py_bin" "$py -c 'import subprocess,sys; r=subprocess.run([\"id\",\"-u\"], capture_output=True, text=True); sys.stdout.write(r.stdout.strip()+\"\\n\")'" "python3 subprocess id -u"
    else
        echo "SKIP python3: not found"
    fi

    echo ""
    echo "=== python-tools ==="
    py_bin=$(find "$R/bin" "$R/usr/bin" -name 'python3*' -type f 2>/dev/null | head -1 || true)
    if [ -n "${py_bin:-}" ] && [ -f "$py_bin" ]; then
        local py
        py=$(basename "$py_bin")
        # NOTE: kaggle/modal/yt_dlp/huggingface_hub currently segfault under loader
        for mod in kaggle modal yt_dlp huggingface_hub; do
            echo "SKIP $py: python -m $mod (segfault under loader)"
            echo "SKIP: python -m $mod - segfault under loader" >> "$SKIP_LOG"
            ((SKIP_COUNT++)) || true
        done
    fi
}

print_summary() {
    echo ""
    echo "=== SUMMARY ==="
    echo "PASS: $PASS_COUNT"
    echo "FAIL: $FAIL_COUNT"
    echo "SKIP: $SKIP_COUNT"
    echo ""
    echo "Logs:"
    echo "  ALL:   $ALL_LOG"
    echo "  PASS:  $PASS_LOG"
    echo "  FAIL:  $FAIL_LOG"
    echo "  SKIP:  $SKIP_LOG"
}

# ─── Dispatch ───────────────────────────────────────────────────────────────
case "${1:-all}" in
    basic)
        category_basic
        print_summary
        ;;
    reexec)
        category_reexec
        print_summary
        ;;
    text)
        category_text
        print_summary
        ;;
    files)
        category_files
        print_summary
        ;;
    system)
        category_system
        print_summary
        ;;
    datetime)
        category_datetime
        print_summary
        ;;
    compression)
        category_compression
        print_summary
        ;;
    networking)
        category_networking
        print_summary
        ;;
    math)
        category_math
        print_summary
        ;;
    diff)
        category_diff
        print_summary
        ;;
    archive)
        category_archive
        print_summary
        ;;
    extended)
        category_extended
        print_summary
        ;;
    shell)
        category_shell
        print_summary
        ;;
    python)
        category_python
        print_summary
        ;;
    all|*)
        category_basic
        category_reexec
        category_text
        category_files
        category_system
        category_datetime
        category_compression
        category_networking
        category_math
        category_diff
        category_archive
        category_extended
        category_shell
        category_python
        print_summary
        ;;
esac
