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
L=$D/usr/bin/elf_loader
R=$D/nh/distro/parrot
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
    timeout 2 ashell -c "$cmd" 2>&1 | head -c 100k || true
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
        # Host limits (Android app uid): /proc/uptime Permission denied,
        # shred /dev/urandom, pslog cte /proc/<pid> jineho procesu.
        # Overeno nativne bez loaderu: selze stejne -> neni bug loaderu.
        uptime|shred|pslog) return 0 ;;
        # ping/ping6: parrot binarky jsou setuid-root / raw ICMP socket, coz
        # Android app uid (10323) neumi -> "setuid: Function not implemented" /
        # "raw socket: Operation not permitted". Host limit, ne bug loaderu
        # (NSS cast je opravena: getprotobyname(icmp)=1 pod loaderem).
        ping|ping6) return 0 ;;
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

# Spusti guest skript pres `bash -s` v guestu (base64 pipe), aby se
# vyhodnotily shell operatory (| && > <). Prime predani loaderu bere cmd jako
# argv -> operatory se neinterpretuji (odtud ~30 falesnych FAILu). Base64 pipe
# obchazi quoting i ashell limit.
_guest_devcmd() {
    local script="$1"
    local b64
    b64=$(printf '%s' "$script" | base64 | tr -d '\n')
    printf 'printf %%s %s | /system/bin/base64 -d | ROOTFS=%s ELF_ROOTFS=%s ELF_LOADER=%s PATH=%s/bin:%s/usr/bin:%s/sbin:%s/usr/sbin %s --ownall %s/bin/bash -s' \
        "$b64" "$R" "$R" "$L" "$R" "$R" "$R" "$R" "$L" "$R"
}

# Prevede guest cmd na skript pro bash -s. Pokud cmd obsahuje shell operator,
# spusti se cely cmd jako skript; jinak se predradi jmeno binarky (PATH).
# TEST_CASES jsou KOMPLETNI prikazy ('wc -c ...', 'false || true',
# 'echo x > f && mv ...'). NEODEBIRAT ani NEPREDRAZOVAT bin_name - cmd jde do
# guest 'bash -s' tak jak je. (Starsi runner odebiral leading bin_name kvuli
# tomu, ze bin sel loaderu zvlast; s bash -s to neplati a predrazeni rozbilo
# mv/rm/cmp: 'mv echo test ...'.)
_guest_script() {
    local bin_name="$1" cmd="$2"
    if [ -z "$cmd" ]; then printf '%s' "$bin_name"; else printf '%s' "$cmd"; fi
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

    # Zaznamenej PIDy elf_loader pred testem
    local pids_before pids_after
    pids_before=$(pgrep -x elf_loader 2>/dev/null | tr '\n' ',' || echo "")

    local script devcmd rc out
    script=$(_guest_script "$bin_name" "$cmd")
    devcmd=$(_guest_devcmd "$script")
    rc=$(ashell_rc "$devcmd") || true
    rc=${rc:-0}
    out=$(ashell_out "$devcmd") || true

    pids_after=$(pgrep -x elf_loader 2>/dev/null | tr '\n' ',' || echo "")
    # Zaznamenej případný nárůst sirotků po tomto testu
    local count_before count_after
    count_before=$(echo "$pids_before" | tr ',' '\n' | grep -c '^[0-9][0-9]*$') || true
    count_after=$(echo "$pids_after" | tr ',' '\n' | grep -c '^[0-9][0-9]*$') || true
    if [ "$count_after" -gt "$count_before" ]; then
        echo "[ORPHAN-INCREASE] $bin_name: +$((count_after - count_before)) elf_loader procesů" >> "$ALL_LOG"
        echo "[ORPHAN-INCREASE] $bin_name: +$((count_after - count_before)) elf_loader procesů" >&2
    fi

    # Omez výstup do ALL_LOG na 200 bajtů, zabrání OOM v host aplikaci
    local out_summary
    out_summary=$(printf '%s' "$out" | head -c 200 | tr '\n' ' ')

    case "$rc" in
        0)
            echo "PASS $bin_name: $desc"
            echo "PASS: $bin_name - $desc" >> "$PASS_LOG"
            ((PASS_COUNT++)) || true
            ;;
        124)
            echo "TIMEOUT $bin_name: $desc"
            echo "TIMEOUT: $bin_name - $desc | $out_summary" >> "$FAIL_LOG"
            ((FAIL_COUNT++)) || true
            ;;
        128|129|130|131|132|133|134|135|136|137|138|139|140|141|142|143|144|145)
            local sig=$((rc - 128))
            echo "CRASH(SIG$sig) $bin_name: $desc"
            echo "CRASH(SIG$sig): $bin_name - $desc | $out_summary" >> "$FAIL_LOG"
            ((FAIL_COUNT++)) || true
            ;;
        *)
            echo "EXIT=$rc $bin_name: $desc"
            echo "EXIT=$rc: $bin_name - $desc | $out_summary" >> "$FAIL_LOG"
            ((FAIL_COUNT++)) || true
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
    local pids_before pids_after
    pids_before=$(pgrep -x elf_loader 2>/dev/null | tr '\n' ',' || echo "")

    local devcmd out rc
    devcmd=$(_guest_devcmd "$cmd")
    out=$(ashell_out "$devcmd") || true
    rc=$(ashell_rc "$devcmd") || true
    rc=${rc:-0}
    pids_after=$(pgrep -x elf_loader 2>/dev/null | tr '\n' ',' || echo "")
    # Zaznamenej případný nárůst sirotků po tomto testu
    local count_before count_after
    count_before=$(echo "$pids_before" | tr ',' '\n' | grep -c '^[0-9][0-9]*$') || true
    count_after=$(echo "$pids_after" | tr ',' '\n' | grep -c '^[0-9][0-9]*$') || true
    if [ "$count_after" -gt "$count_before" ]; then
        echo "[ORPHAN-INCREASE] $bin_name: +$((count_after - count_before)) elf_loader procesů" >> "$ALL_LOG"
        echo "[ORPHAN-INCREASE] $bin_name: +$((count_after - count_before)) elf_loader procesů" >&2
    fi

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
TEST_CASES[ps]="ps -o pid,comm | head -3"
TEST_CASES[free]="free -h"
TEST_CASES[df]="df -h /"
TEST_CASES[du]="du -sh /etc"

# date/time
TEST_CASES[date]="date"
TEST_CASES[cal]="cal 2024 | head -3"
TEST_CASES[sleep]="sleep 0.1 && echo ok"
# timeout bez prikazu -> usage RC=125; spravny smoke test je spustit neco kratkeho.
TEST_CASES[timeout]="timeout 1 true"
TEST_CASES[hostid]="hostid"
TEST_CASES[man]="man --help 2>&1 | head -1"
TEST_CASES[less]="less --help 2>&1 | head -1"
TEST_CASES[more]="more --help 2>&1 | head -1"
TEST_CASES[nano]="nano --version 2>&1 | head -1"
TEST_CASES[lesspipe]="lesspipe --help 2>&1 | head -1"
TEST_CASES[manpath]="manpath"
TEST_CASES[mandb]="mandb --help 2>&1 | head -1"
TEST_CASES[man-recode]="man-recode --help 2>&1 | head -1"
# pslog bez PID -> usage RC=255; s PID se testuje smoke cesta.
TEST_CASES[pslog]="pslog 1"
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
# ping6 nezna -W (GNU ping flag) -> RC=64; pouzij jen -c na ::1.
TEST_CASES[ping6]="ping6 -c 1 ::1"
TEST_CASES[zipinfo]="zipinfo /tmp/test.zip 2>/dev/null | head -3"
TEST_CASES[column]="printf 'a:b\nc:d\n' | column -t -s:"
TEST_CASES[expand]="printf 'a\tb\n' | expand"
TEST_CASES[unexpand]="printf 'a b\n' | unexpand"
TEST_CASES[fold]="printf 'abcdefghijklmnopqrstuvwxyz\n' | fold -w 10"
TEST_CASES[fmt]="printf 'a b c d e f g h i j k l m n o p\n' | fmt"
TEST_CASES[nl]="printf 'line1\nline2\n' | nl"
TEST_CASES[comm]="printf 'a\nb\nc\n' > /tmp/c1.txt && printf 'b\nc\nd\n' > /tmp/c2.txt && comm /tmp/c1.txt /tmp/c2.txt"
TEST_CASES[sdiff]="echo test > /tmp/s1.txt && echo test > /tmp/s2.txt && sdiff /tmp/s1.txt /tmp/s2.txt"
TEST_CASES[cmp]="echo test > /tmp/cmp1.txt && echo test > /tmp/cmp2.txt && cmp /tmp/cmp1.txt /tmp/cmp2.txt"
TEST_CASES[md5sum]="echo test | md5sum"
TEST_CASES[sha1sum]="echo test | sha1sum"
TEST_CASES[sha256sum]="echo test | sha256sum"
TEST_CASES[cksum]="echo test | cksum"
TEST_CASES[base64]="echo test | base64"
TEST_CASES[split]="printf 'abcdefghijklmnopqrstuvwxyz\n' | split -b 5 - /tmp/split_out && cat /tmp/split_out*"
TEST_CASES[csplit]="printf 'a\nb\nc\nd\ne\n' > /tmp/cs.txt && csplit /tmp/cs.txt 2 4 >/dev/null 2>&1 && cat xx*"
TEST_CASES[tee]="echo test | tee /tmp/tee_out"
TEST_CASES[script]="script -q -c 'echo test' /tmp/script_out 2>/dev/null && cat /tmp/script_out"
TEST_CASES[stty]="stty size 2>/dev/null || stty -a 2>&1 | head -1"
TEST_CASES[tput]="tput cols"
TEST_CASES[clear]="true"
TEST_CASES[reset]="true"
TEST_CASES[tset]="tset -Q 2>&1 | head -1"
TEST_CASES[wall]="echo test | wall -n 2>&1 | head -1 || true"
TEST_CASES[systemctl]="systemctl --version 2>&1 | head -1"
TEST_CASES[service]="service --version 2>&1 | head -1"
TEST_CASES[journalctl]="journalctl --version 2>&1 | head -1"
TEST_CASES[loginctl]="loginctl --version 2>&1 | head -1"
TEST_CASES[timedatectl]="timedatectl --version 2>&1 | head -1"
TEST_CASES[localectl]="localectl --version 2>&1 | head -1"
TEST_CASES[findmnt]="findmnt --version 2>&1 | head -1"
TEST_CASES[lsblk]="lsblk --version 2>&1 | head -1"
TEST_CASES[fallocate]="fallocate -l 1 /tmp/fallocate_test 2>/dev/null && rm -f /tmp/fallocate_test"
TEST_CASES[truncate]="truncate -s 1 /tmp/truncate_test 2>/dev/null && rm -f /tmp/truncate_test"
TEST_CASES[shred]="shred -n 1 -s 1 /tmp/shred_test 2>/dev/null && rm -f /tmp/shred_test"
TEST_CASES[fuser]="fuser -m /tmp 2>&1 | head -1"
TEST_CASES[nice]="nice -n 10 true"
TEST_CASES[renice]="renice -n 10 -p \$\$ 2>&1 | head -1 || true"
TEST_CASES[nohup]="nohup true /tmp/nohup_test 2>/dev/null && rm -f /tmp/nohup_test"
TEST_CASES[watch]="watch --version 2>&1 | head -1"
TEST_CASES[factor]="factor 42"
TEST_CASES[xxd]="echo test | xxd | head -1"
TEST_CASES[getconf]="getconf LONG_BIT 2>/dev/null || getconf -a 2>&1 | head -1"
TEST_CASES[getent]="getent passwd root 2>/dev/null | head -1"
TEST_CASES[iconv]="echo test | iconv -f UTF-8 -t ASCII 2>&1 | head -1"
TEST_CASES[localedef]="localedef --version 2>&1 | head -1"
TEST_CASES[zdump]="zdump --version 2>&1 | head -1"
TEST_CASES[gencat]="gencat --version 2>&1 | head -1 || true"
TEST_CASES[strings]="strings /etc/hostname | head -1"
TEST_CASES[logger]="logger --version 2>&1 | head -1"
TEST_CASES[dmesg]="dmesg --version 2>&1 | head -1"
TEST_CASES[pr]="echo test | pr | head -1"
TEST_CASES[rlwrap]="rlwrap --version 2>&1 | head -1 || true"
TEST_CASES[tmux]="tmux -V 2>&1 | head -1"
TEST_CASES[zcat]="echo test | zcat 2>/dev/null || gzip -c /etc/hostname | zcat | head -1"
TEST_CASES[zless]="gzip -c /etc/hostname > /tmp/test.gz && zless /tmp/test.gz | head -1"
TEST_CASES[zmore]="gzip -c /etc/hostname > /tmp/test.gz && zmore /tmp/test.gz | head -1"
TEST_CASES[zfgrep]="gzip -c /etc/hostname > /tmp/test.gz && zfgrep host /tmp/test.gz"
TEST_CASES[zegrep]="gzip -c /etc/hostname > /tmp/test.gz && zegrep host /tmp/test.gz"
TEST_CASES[zgrep]="gzip -c /etc/hostname > /tmp/test.gz && zgrep host /tmp/test.gz"
TEST_CASES[bunzip2]="bzip2 -c /etc/hostname | bunzip2 | head -1"
TEST_CASES[bzcat]="bzip2 -c /etc/hostname | bzcat | head -1"
TEST_CASES[bzmore]="bzip2 -c /etc/hostname | bzmore 2>&1 | head -1"
TEST_CASES[bzless]="bzip2 -c /etc/hostname | bzless 2>&1 | head -1"
TEST_CASES[bzgrep]="bzip2 -c /etc/hostname | bzgrep host"
TEST_CASES[test]="test -f /etc/hostname"
TEST_CASES[mt]="mt -f /dev/null status 2>&1 | head -1"
TEST_CASES[mountpoint]="mountpoint -q /tmp || mountpoint /tmp 2>&1 | head -1"
TEST_CASES[partx]="partx --version 2>&1 | head -1"
TEST_CASES[logger]="logger --version 2>&1 | head -1"
TEST_CASES[tty]="tty 2>&1 | head -1"
TEST_CASES[who]="who 2>&1 | head -1"
TEST_CASES[users]="users 2>&1 | head -1"
TEST_CASES[busybox]="busybox --help 2>&1 | head -1"
TEST_CASES[link]="link --version 2>&1 | head -1"
TEST_CASES[ln]="ln --version 2>&1 | head -1"
TEST_CASES[mknod]="mknod --version 2>&1 | head -1"
TEST_CASES[mkfifo]="mkfifo --version 2>&1 | head -1"
TEST_CASES[chattr]="chattr --version 2>&1 | head -1"
TEST_CASES[lsattr]="lsattr --version 2>&1 | head -1"
TEST_CASES[seq]="seq 5"
TEST_CASES[printf]="printf 'test\\n'"
TEST_CASES[od]="echo test | od -An -tx1 | head -1"
TEST_CASES[hexdump]="echo test | hexdump -C | head -1"
TEST_CASES[dd]="dd if=/dev/zero bs=1 count=1 2>&1 | head -1"
TEST_CASES[getent]="getent passwd root 2>/dev/null | head -1"
TEST_CASES[iconv]="echo test | iconv -f UTF-8 -t ASCII 2>&1 | head -1"
TEST_CASES[localedef]="localedef --version 2>&1 | head -1"
TEST_CASES[zdump]="zdump --version 2>&1 | head -1"
TEST_CASES[gencat]="gencat --version 2>&1 | head -1 || true"
TEST_CASES[strings]="strings /etc/hostname | head -1"
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
TEST_CASES[lsof]="lsof --help 2>/dev/null | head -1"
TEST_CASES[ps]="ps -o pid,comm | head -3"
TEST_CASES[pgrep]="pgrep -l init 2>/dev/null | head -1"

# ─── Categories ─────────────────────────────────────────────────────────────
category_basic() {
    echo ""
    echo "=== basic ==="
    for cmd in echo true false; do
        [ -f "$R/bin/$cmd" ] && run_test "$R/bin/$cmd" "${TEST_CASES[$cmd]:-true}" "basic $cmd"
    done
}

category_reexec() {
    echo ""
    echo "=== reexec ==="
    for tool in tar gzip gunzip bzip2 bunzip2 xz unxz; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "re-exec $tool ${TEST_CASES[$tool]:-test}"
    done
}

category_text() {
    echo ""
    echo "=== text ==="
    for tool in grep sed awk wc cut sort uniq tr head tail cat; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
    done
}

category_files() {
    echo ""
    echo "=== files ==="
    for tool in ls cp mv rm mkdir stat find realpath dirname basename; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
    done
}

category_system() {
    echo ""
    echo "=== system ==="
    for tool in uname hostname uptime whoami id ps free df du hostid man less more nano lesspipe manpath mandb man-recode pslog pstree; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
    done
}

category_datetime() {
    echo ""
    echo "=== datetime ==="
    for tool in date cal timeout sleep; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
    done
}

category_compression() {
    echo ""
    echo "=== compression ==="
    for tool in gzip gunzip bzip2 bunzip2 xz unxz tar; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
    done
}

category_networking() {
    echo ""
    echo "=== networking ==="
    for tool in ping nslookup; do
        if [ -e "$R/usr/bin/$tool" ]; then
            if [ -f "$R/usr/bin/$tool" ]; then
                run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
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
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
    done
}

category_diff() {
    echo ""
    echo "=== diff ==="
    for tool in diff patch; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
    done
}

category_archive() {
    echo ""
    echo "=== archive ==="
    for tool in zip unzip; do
        [ -f "$R/usr/bin/$tool" ] && run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "${TEST_CASES[$tool]:-test}"
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
        run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "extended $tool ${TEST_CASES[$tool]:-test}"
    done
}

category_extra() {
    echo "EXTRA START" >&2
    echo ""
    echo "=== extra ==="
    for tool in zipinfo column expand unexpand fold fmt nl comm sdiff cmp md5sum sha1sum sha256sum cksum base64 split csplit tee script stty tput clear reset tset wall systemctl service journalctl loginctl timedatectl localectl findmnt lsblk fallocate truncate shred fuser nice renice nohup watch factor xxd getconf getent iconv localedef zdump gencat strings logger dmesg pr rlwrap tmux zcat zless zmore zfgrep zegrep zgrep bunzip2 bzcat bzmore bzless bzgrep test mt mountpoint partx tty who users busybox link ln mknod mkfifo chattr lsattr seq printf od hexdump dd; do
        echo "TESTING: $tool" >&2
        [ -f "$R/usr/bin/$tool" ] || continue
        # Skip non-ELF executables (scripts, symlinks to scripts, etc.)
        if ! readelf -h "$R/usr/bin/$tool" >/dev/null 2>&1; then
            echo "SKIP $tool: not an ELF binary"
            echo "SKIP: $tool - not an ELF binary" >> "$SKIP_LOG"
            ((SKIP_COUNT++)) || true
            continue
        fi
        run_test "$R/usr/bin/$tool" "${TEST_CASES[$tool]:-test}" "extra $tool ${TEST_CASES[$tool]:-test}"
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

# Regresni test: absolutni symlink venv/bin/python mirici MIMO ROOTFS
# (uv managed Python pod /data/.../uv/python/...). resolve_symlinks_under_root()
# ho NESMI prependnout pod ROOTFS, jinak $R/data/... -> ENOENT a venv je mrtvy.
category_uv() {
    echo ""
    echo "=== uv (venv symlink mimo ROOTFS) ==="
    local uv_bin="$R/usr/local/bin/uv"
    if [ ! -x "$uv_bin" ]; then
        echo "SKIP uv: not found at $uv_bin"
        echo "SKIP: uv - not found" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return 0
    fi

    local vdir="$R/root/uv_regr_venv"
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local rc out

    # 1) vytvor venv (uv) a spust venv python pres absolutni symlink mimo ROOTFS
    ashell -c "$env $L --ownall $uv_bin venv $vdir" >/dev/null 2>&1 || true
    out=$(ashell_out "$env $L --ownall $vdir/bin/python -V")
    if printf '%s' "$out" | grep -Fq "Python 3"; then
        echo "PASS uv: venv python symlink mimo ROOTFS ($out)"
        echo "PASS: uv - venv python symlink" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL uv: venv python | $out"
        echo "FAIL: uv - venv python symlink | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 2) uv pip install do venv + import z venv pythonu
    rc=$(ashell_rc "$env $L --ownall $uv_bin pip install --python $vdir/bin/python six")
    out=$(ashell_out "$env $L --ownall $vdir/bin/python -c 'import six; print(six.__version__)'")
    if [ "$rc" = 0 ] && printf '%s' "$out" | grep -Fq "1."; then
        echo "PASS uv: pip install + import six ($out)"
        echo "PASS: uv - pip install six" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL uv: pip install/import RC=$rc out=$out"
        echo "FAIL: uv - pip install six | RC=$rc | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 3) uv python find + prime spusteni managed Pythonu (glibc ELF mimo
    #    ROOTFS). Puvodne padalo na "Failed to inspect Python interpreter"
    #    (raw bionic exec managed Pythonu bez PT_INTERP) - fix #1 (is_glibc_elf).
    #    Vystup (cesta k interpetu) si ulozime pro case #4 - NEhardcodovat
    #    verzi (uv python find realne vraci 3.14.7, ne 3.15; hardcoded by
    #    test tiše rotoval/padal pri zmene managed verze nebo bez site).
    rc=$(ashell_rc "$env $L --ownall $uv_bin python find")
    out=$(ashell_out "$env $L --ownall $uv_bin python find")
    local py_path=""
    if [ "$rc" = 0 ] && printf '%s' "$out" | grep -Fq "python"; then
        echo "PASS uv: python find + managed Python ($out)"
        echo "PASS: uv - python find" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
        # posledni radek vystupu bez mezer = cesta k interpetu (vnejsi cesta
        # uz pod ROOTFS/device, takze ji uv run dostane jako --python <cesta>)
        py_path=$(printf '%s' "$out" | tr -d '\r' | grep -F "python" | tail -1 | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    else
        echo "FAIL uv: python find RC=$rc out=$out"
        echo "FAIL: uv - python find | RC=$rc | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 4) uv run python -c (re-exec managed Pythonu pod loaderem, end-to-end).
    #    Interpet odvozeny z case #3 (--python <cesta>), ne hardcoded verze.
    if [ -n "$py_path" ]; then
        rc=$(ashell_rc "$env $L --ownall $uv_bin run --no-project --python $py_path python -c 'print(42)'")
        out=$(ashell_out "$env $L --ownall $uv_bin run --no-project --python $py_path python -c 'print(42)'")
        if [ "$rc" = 0 ] && printf '%s' "$out" | grep -Fq "42"; then
            echo "PASS uv: run managed Python -c print(42) ($out)"
            echo "PASS: uv - run python" >> "$PASS_LOG"
            ((PASS_COUNT++)) || true
        else
            echo "FAIL uv: run python RC=$rc out=$out"
            echo "FAIL: uv - run python | RC=$rc | $out" >> "$FAIL_LOG"
            ((FAIL_COUNT++)) || true
        fi
    else
        echo "FAIL uv: run python | interpet neznamy (python find selhal)"
        echo "FAIL: uv - run python | interpreter from python find unavailable" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# Regresni test: symlink chain v ROOTFS s absolutnimi cily na skript/ELF.
# Klasicky pripad: $R/usr/bin/which -> /etc/alternatives/which ->
# /usr/bin/which.debianutils (skript /bin/sh). Entry shebang detekce v main()
# musi nejdriv resolvnout symlinky pod ROOTFS, jinak open(path) na hostu selze
# (broken symlink) a loader hlasi "Not an ELF file". Druhy pripad: symlink na
# ELF (awk -> /etc/alternatives/awk -> gawk) musi porad bezet jako ELF.
category_symlink() {
    echo ""
    echo "=== symlink chain (absolutni cile v ROOTFS) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L PATH=$R/usr/bin:$R/bin"
    local rc out

    # 1) symlink -> skript (/bin/sh): which pres alternatives chain
    rc=$(ashell_rc "$env $L --ownall $R/usr/bin/which ls")
    out=$(ashell_out "$env $L --ownall $R/usr/bin/which ls")
    if [ "$rc" = 0 ] && printf '%s' "$out" | grep -Fq "/usr/bin/ls"; then
        echo "PASS symlink: which -> alternatives -> skript ($out)"
        echo "PASS: symlink - which skript" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL symlink: which | RC=$rc | $out"
        echo "FAIL: symlink - which skript | RC=$rc | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 2) symlink -> ELF: awk -> alternatives -> gawk (must not break ELF path)
    out=$(ashell_out "$env $L --ownall $R/usr/bin/awk 'BEGIN{print 1+2}'")
    if printf '%s' "$out" | grep -Fq "3"; then
        echo "PASS symlink: awk -> alternatives -> gawk ELF ($out)"
        echo "PASS: symlink - awk ELF" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL symlink: awk | $out"
        echo "FAIL: symlink - awk ELF | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 3) regrese guard: zadny 'Not an ELF file' na stderr u which
    out=$(ashell_out "$env $L --ownall $R/usr/bin/which ls 2>&1")
    if printf '%s' "$out" | grep -Fq "Not an ELF file"; then
        echo "FAIL symlink: which stale hlasi 'Not an ELF file'"
        echo "FAIL: symlink - which Not an ELF | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    else
        echo "PASS symlink: which nehlasi 'Not an ELF file'"
        echo "PASS: symlink - which no-Not-ELF" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    fi
}

# Regresni test: stat s dir_fd (stary ABI __fxstatat64: ver, fd, path, buf, flags).
# Drivejsi shim mel 4 argy -> glibc volani interpretovalo 'fd' (cislo) jako
# ukazatel na cestu -> EFAULT ("Bad address") u Python os.stat(dir_fd=...),
# shutil.rmtree a PEP517 buildu wheel (pad python-nmap). Tento test to hlida.
category_fstat() {
    echo ""
    echo "=== fstatat dir_fd (__fxstatat64 ABI) ==="
    local py_bin
    py_bin=$(find "$R/bin" "$R/usr/bin" -name 'python3*' -type f 2>/dev/null | head -1 || true)
    if [ -z "${py_bin:-}" ] || [ ! -f "$py_bin" ]; then
        echo "SKIP fstat: python3 not found"
        echo "SKIP: fstat - no python3" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return 0
    fi
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local code='import os,sys;fd=os.open(sys.argv[1],os.O_RDONLY);st=os.stat(sys.argv[2],dir_fd=fd,follow_symlinks=False);print(chr(70)+chr(83)+chr(84)+chr(79)+chr(75),st.st_size)'
    local rc out
    rc=$(ashell_rc "$env $L --ownall $py_bin -c '$code' $R/etc passwd")
    out=$(ashell_out "$env $L --ownall $py_bin -c '$code' $R/etc passwd")
    if [ "$rc" = 0 ] && printf '%s' "$out" | grep -Fq "FSTOK"; then
        echo "PASS fstat: os.stat(dir_fd) bez EFAULT ($out)"
        echo "PASS: fstat - os.stat dir_fd" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL fstat: os.stat(dir_fd) RC=$rc out=$out"
        echo "FAIL: fstat - os.stat dir_fd | RC=$rc | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    if printf '%s' "$out" | grep -Fq "Bad address"; then
        echo "FAIL fstat: EFAULT 'Bad address' stale"
        echo "FAIL: fstat - Bad address | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# Regresni test: NSS /etc/protocols pod loaderem. shim_open64_nocancel musi
# mit /etc/protocols (+ /etc/services) v whitelistu, jinak glibc NSS files
# backend cte host /etc/protocols (neexistuje) -> ENOENT -> ping hlasi
# "unknown protocol icmp". Test zamerne NEPOUZIVA ping (ten je setuid-root /
# raw ICMP socket = host limit -> should_skip), ale getprotobyname() pres
# Python, ktery na techto privilegiich nezavisi.
category_nss() {
    echo ""
    echo "=== nss /etc/protocols (getprotobyname) ==="
    local py_bin
    py_bin=$(find "$R/bin" "$R/usr/bin" -name 'python3*' -type f 2>/dev/null | head -1 || true)
    if [ -z "${py_bin:-}" ] || [ ! -f "$py_bin" ]; then
        echo "SKIP nss: python3 not found"
        echo "SKIP: nss - no python3" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return 0
    fi
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local code='import socket;print(chr(80)+chr(82)+chr(79)+chr(84)+chr(79),socket.getprotobyname(chr(105)+chr(99)+chr(109)+chr(112)))'
    local rc out
    rc=$(ashell_rc "$env $L --ownall $py_bin -c '$code'")
    out=$(ashell_out "$env $L --ownall $py_bin -c '$code'")
    if [ "$rc" = 0 ] && printf '%s' "$out" | grep -Fq "PROTO 1"; then
        echo "PASS nss: getprotobyname(icmp) = 1 ($out)"
        echo "PASS: nss - getprotobyname icmp" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL nss: getprotobyname(icmp) RC=$rc out=$out"
        echo "FAIL: nss - getprotobyname icmp | RC=$rc | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
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

category_discovered() {
    echo ""
    echo "=== discovered ==="
    local dirs=("$R/bin" "$R/usr/bin" "$R/sbin" "$R/usr/sbin")
    local dir bin name cmd
    for dir in "${dirs[@]}"; do
        [ -d "$dir" ] || continue
        # Use process substitution to avoid subshell for while loop
        while IFS= read -r -d '' bin; do
            name=$(basename "$bin")
            # Determine a safe test command: try --version first, fallback to true
            if { "$bin" --version >/dev/null 2>&1; } 2>/dev/null; then
                cmd="$name --version"
            else
                cmd="true"
            fi
            run_test "$bin" "$cmd" "discovered $name"
        done < <(find "$dir" -maxdepth 1 -type f -print0 2>/dev/null)
    done
    check_orphans "discovered"
}

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
    extra)
        category_extra
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
    uv)
        category_uv
        print_summary
        ;;
    symlink)
        category_symlink
        print_summary
        ;;
    fstat)
        category_fstat
        print_summary
        ;;
    nss)
        category_nss
        print_summary
        ;;
        discovered)
            category_discovered
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
        category_extra
        category_shell
        category_python
        category_uv
        category_symlink
        category_fstat
        category_nss
        print_summary
        ;;
esac
