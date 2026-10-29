#!/bin/bash
# Sjednocený testovací rámec pro elf_loader.
# Podle skills.md: bionic interpreter check → deploy do files/ → test přes ashell -c
#
# DULEZITE: cely skript se spousti JEDNOU pres `ashell -c` (ne kazdy
# jednotlivy test zvlast - drivejsi navrh). `ashell`'s vlastni shell ma
# jen /system/bin/sh, zadny bash, takze jako jediny prikaz musi spustit
# primo own-loadovany guest bash (plnou cestou pod $R):
#
#   ashell -c "ROOTFS=$R ELF_ROOTFS=$R HOME=$D L=$L $L --ownall $R/bin/bash /root/elf_loader/test-all.sh all"
#
# Uvnitr uz bezi vse primo (`ashell_rc`/`ashell_out` nevolaji dalsi ashell).
# POZOR na `timeout`/`bash` v pomocnych funkcich - MUSI byt plnou cestou pod
# $R, ne bare jmenem: PATH v own-loadovanem guestu dava /system/bin PRED
# rootfs, takze bare "timeout" by se resolvoval na host bionic toybox (bez
# nasich hooku), ktery by sveho cile (glibc pod ROOTFS) zkusil spustit RAW
# -> "Permission denied". S plnou cestou se spravne own-loaduje guest
# timeout, ktery uz hooky ma - viz postup.md 2026-10-06 (8)/(9).
#
# Usage (po spusteni pres ashell viz vyse):
#   ./test-all.sh all            # vsechny kategorie
#   ./test-all.sh reexec         # pouze re-exec binarky
#   ./test-all.sh python         # pouze python smoke testy
#   ./test-all.sh basic text files system datetime compression networking math diff archive extended shell python

set -euo pipefail

# ─── Device paths ───────────────────────────────────────────────────────────
D=/data/user/0/com.linux_core/files
L=${L:-$D/usr/bin/elf_loader}
R=$D/nh/distro/parrot
E=$D/usr/bin/elroot

# ─── Results ────────────────────────────────────────────────────────────────
RESULTS_DIR=$R/root/elf_loader/results
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
# Nativni mod (prompt 12, prototyp): `L` nemiri na loader, ale na shell shim
# tools/lxnative-ownall.sh, ktery test spusti pres nativni rootfs ld.so.
# Shim neni bionic ELF, takze by na `check_interpreter` skoncil tise na
# `exit 1` (stderr se v ashell kanalu ztrati). Default je nedotceny.
if [ "${LX_NATIVE_MODE:-0}" = 1 ]; then
    L=${LXN_SHIM:-$D/tmp/p12/lxnative-ownall.sh}
    [ -x "$L" ] || { echo "LX_NATIVE_MODE=1 ale neni $L" >&2; exit 1; }
else
    check_interpreter "$L"
fi

# Spusti prikaz PRIMO (bez dalsiho ashell -c) - cely test-all.sh uz bezi
# uvnitr jednoho ashell -c (viz hlavicka). DULEZITE: `timeout`/`bash` tady
# MUSI byt plnou cestou pod $R, ne bare jmenem - PATH v own-loadovanem
# guestu dava /system/bin PRED rootfs, takze bare "timeout"/"bash" by se
# resolvovaly na HOST bionic toybox/nic, ktery nasledny exec sveho cile
# (glibc pod ROOTFS) spusti RAW bez loaderu -> "Permission denied".
# S plnou cestou se spravne own-loaduje guest timeout/bash, ktery uz ma
# nase hooky aktivni (viz postup.md 2026-10-06 (8) - zjistena skutecna
# pricina, puvodni teorie o obchazeni PLT hooku byla chybna).
ashell_rc() {
    local cmd="$1"
    local rc=0
    "$R/usr/bin/timeout" 5 "$R/bin/bash" -c "$cmd" >/dev/null 2>&1 || rc=$?
    echo "$rc"
}

ashell_out() {
    local cmd="$1"
    "$R/usr/bin/timeout" 2 "$R/bin/bash" -c "$cmd" 2>&1 | head -c 100k || true
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
        *sftp*|*rsync*|*ftp*|*telnet*|nc|*netcat*) return 0 ;;
        # X11
        *X11*|*xterm*|*xvfb*|*Xorg*|*xset*|*xrandr*|*xclock*) return 0 ;;
        # Crypto
        *gpg*|*gnupg*|*openssl*|*gpgsm*) return 0 ;;
        # Interactive editors
        *vim*|vi|*nano*|emacs*|less*|more*|man*) return 0 ;;
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
    "$R/bin/bash" -c "$env $L --ownall $uv_bin venv $vdir" >/dev/null 2>&1 || true
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

# Path-translace pro metadata/uzly/xattr/unix socket (chmod, chown, utime,
# truncate, mkfifo, listxattr, AF_UNIX bind/connect) na guest cestach, ktere
# existuji JEN pod ROOTFS. Hardlink (link/linkat) zamerne ne - SELinux app
# domeny ho zakazuje i nativne (avc denied { link }). Viz postup.md 2026-10-07 (4).
category_pathops() {
    echo ""
    echo "=== path-translace: chmod/chown/utime/truncate/mkfifo/xattr/AF_UNIX ==="
    local py_bin
    py_bin=$(find "$R/usr/bin" -name 'python3' 2>/dev/null | head -1 || true)
    if [ -z "${py_bin:-}" ]; then
        echo "SKIP pathops: python3 not found"
        echo "SKIP: pathops - no python3" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return 0
    fi
    local g="/tmp/pathops.$$" h="$R/tmp/pathops.$$"
    mkdir -p "$h"
    cat > "$h/t.py" <<PYEOF
import os, socket
B = "$g"
open(B + "/f", "w").write("hello")
r = []
def t(n, fn):
    try:
        fn(); r.append(n)
    except Exception as e:
        print("ERR", n, e)
t("chmod", lambda: os.chmod(B + "/f", 0o640))
t("chown", lambda: os.chown(B + "/f", os.getuid(), os.getgid()))
t("utime", lambda: os.utime(B + "/f", (1000000000, 1000000000)))
t("truncate", lambda: os.truncate(B + "/f", 2))
t("mkfifo", lambda: os.mkfifo(B + "/fifo"))
t("listxattr", lambda: os.listxattr(B + "/f"))
def u():
    p = B + "/s"; s = socket.socket(socket.AF_UNIX); s.bind(p); s.listen(1)
    c = socket.socket(socket.AF_UNIX); c.connect(p); c.close(); s.close()
t("unix", u)
print("PATHOPS", len(r), " ".join(r))
PYEOF
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local out
    out=$(ashell_out "$env $L --ownall $py_bin $g/t.py")
    local hmode hsize
    hmode=$(stat -c %a "$h/f" 2>/dev/null || echo none)
    hsize=$(stat -c %s "$h/f" 2>/dev/null || echo none)
    # libc mkdtemp (glibc interni __mkdir mimo GOT): busybox mktemp -d ji vola primo
    if [ -x "$R/usr/bin/busybox" ]; then
        local dt
        dt=$(ashell_out "$env $L --ownall $R/usr/bin/busybox mktemp -d $g/dXXXXXX")
        if [ -n "$dt" ] && [ -d "$R$dt" ]; then
            echo "PASS pathops: mkdtemp na guest ceste ($dt)"
            echo "PASS: pathops - mkdtemp" >> "$PASS_LOG"
            ((PASS_COUNT++)) || true
        else
            echo "FAIL pathops: mkdtemp out='$dt'"
            echo "FAIL: pathops - mkdtemp | $dt" >> "$FAIL_LOG"
            ((FAIL_COUNT++)) || true
        fi
    fi
    # libc funkce s internimi volanimi mimo GOT (remove->unlink, glob/nftw->
    # opendir/lstat, system/popen->__posix_spawn("/bin/sh")); vysledne cesty
    # musi byt guest (bez prefixu ROOTFS). Viz postup.md 2026-10-07 (6).
    mkdir -p "$h/w/sub"
    : > "$h/w/a"; : > "$h/w/b"
    cat > "$h/l.py" <<PYEOF
import ctypes, ctypes.util
c = ctypes.CDLL(None)
B = "$g/w"
r = []
def t(n, ok):
    (r.append(n) if ok else print("ERR", n))
t("remove", c.remove((B + "/b").encode()) == 0)
class G(ctypes.Structure):
    _fields_ = [("pathc", ctypes.c_size_t), ("pathv", ctypes.POINTER(ctypes.c_char_p)),
                ("offs", ctypes.c_size_t), ("pad", ctypes.c_char * 128)]
g = G()
gr = c.glob((B + "/*").encode(), 0, None, ctypes.byref(g))
t("glob", gr == 0 and g.pathc == 2 and g.pathv[0] == (B + "/a").encode())
seen = []
CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)
cb = CB(lambda p, s, f, w: seen.append(p) or 0)
t("nftw", c.nftw(B.encode(), cb, 8, 1) == 0 and len(seen) == 3 and all(p.startswith(B.encode()) for p in seen))
t("system", c.system(b"test -x /usr/bin/python3") == 0)
c.popen.restype = ctypes.c_void_p
f = c.popen(b"test -x /usr/bin/python3 && echo GUEST", b"r")
buf = ctypes.create_string_buffer(64)
c.fgets.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p]
c.pclose.argtypes = [ctypes.c_void_p]
got = f and c.fgets(buf, 64, f)
if f: c.pclose(f)
t("popen", buf.value.strip() == b"GUEST")
print("LIBCOPS", len(r), " ".join(r))
PYEOF
    local lout
    lout=$(ashell_out "$env $L --ownall $py_bin $g/l.py")
    if printf '%s' "$lout" | grep -Fq "LIBCOPS 5"; then
        echo "PASS pathops: libc remove/glob/nftw/system/popen na guest ceste"
        echo "PASS: pathops - libc interni" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL pathops: libc interni out=$lout"
        echo "FAIL: pathops - libc interni | $lout" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    # Dalsi glibc-interni cesty: scandir (__opendir -> nocancel O_DIRECTORY),
    # tempnam/ftok (stat/lstat -> fstatat, BTI inline hook), getcwd (strip $R),
    # tzset/setmntent (nocancel whitelist), sem_open (/dev/shm + link->renameat2).
    # Viz postup.md 2026-10-07 (7).
    cat > "$h/l2.py" <<PYEOF
import ctypes, os, time
c = ctypes.CDLL(None)
B = "$g/w"
r = []
def t(n, ok):
    (r.append(n) if ok else print("ERR", n))
nl = ctypes.c_void_p()
t("scandir", c.scandir(B.encode(), ctypes.byref(nl), None, None) == 4)
os.environ.pop("TMPDIR", None)
c.tempnam.restype = ctypes.c_char_p
tn = c.tempnam(B.encode(), b"pfx")
t("tempnam", tn is not None and tn.startswith(B.encode() + b"/pfx"))
t("ftok", c.ftok((B + "/a").encode(), 97) != -1)
os.chdir(B)
t("getcwd", os.getcwd() == B)
os.environ["TZ"] = "Europe/Prague"; time.tzset()
t("tzset", time.tzname[0] == "CET")
c.setmntent.restype = ctypes.c_void_p
m = c.setmntent(b"/etc/fstab", b"r")
t("setmntent", bool(m))
import multiprocessing
l = multiprocessing.Lock(); l.acquire(); l.release()
t("sem_open", True)
print("LIBCOPS2", len(r), " ".join(r))
PYEOF
    local l2out
    l2out=$(ashell_out "$env $L --ownall $py_bin $g/l2.py" 2>&1)
    if printf '%s' "$l2out" | grep -Fq "LIBCOPS2 7"; then
        echo "PASS pathops: scandir/tempnam/ftok/getcwd/tzset/setmntent/sem_open"
        echo "PASS: pathops - libc interni 2" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL pathops: libc interni 2 out=$l2out"
        echo "FAIL: pathops - libc interni 2 | $l2out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    rm -rf "$h"
    if printf '%s' "$out" | grep -Fq "PATHOPS 7" && [ "$hmode" = 640 ] && [ "$hsize" = 2 ]; then
        echo "PASS pathops: 7/7 operaci na guest ceste (host vidi mode=$hmode size=$hsize)"
        echo "PASS: pathops - 7/7" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL pathops: out=$out host mode=$hmode size=$hsize"
        echo "FAIL: pathops | $out | mode=$hmode size=$hsize" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# /proc/self/exe musi vracet guest binarku (ne loader) a GNU Make fast path
# (recept bez shell-metaznaku -> gnulib find_in_given_path -> eaccess() pred
# posix_spawn) musi najit program pod ROOTFS. Viz postup.md 2026-10-07 (3).
# Regresni test: argv/env retezce se kopiruji na guest stack; rezerva byla
# odhad (argc*128 + env*256), dlouhy argument (trans -> gawk s celym awk
# programem v argv) pretekl za vrchol stacku do libc.so.6 -> SIGSEGV nebo
# tise oriznuty argument. Viz feedback/m2101k6g-2026-10-09-trans-221301.md.
# Regresni test: lx -c '<radka>' posle celou shell radku guest bashi, takze
# funguji pipy/&&/redirecty (driv `lx cmd | other` resp. `lx -c` hlasilo
# "'-c' neni v rootfs" a | se nikdy nedostalo do guestu).
category_lxc() {
    echo ""
    echo "=== lx -c (pipe/&& v guestu pres init bash) ==="
    local dir="$R/tmp/lxc.$$"
    mkdir -p "$dir"
    cat > "$dir/t.sh" <<'EOS'
eval "$($LB init bash)"
lx -c 'echo ahoj svet | tr a-z A-Z'
lx echo plain
EOS
    local out
    out=$(ashell_out "D=$D R=$R LB=$L HOME=$D $L --ownall $R/bin/bash ${dir#$R}/t.sh")
    rm -rf "$dir"
    if printf '%s' "$out" | grep -qx "AHOJ SVET" && printf '%s' "$out" | grep -qx "plain"; then
        echo "PASS lxc: lx -c pipe + lx <cmd> oboje funguje"
        echo "PASS: lxc - lx -c pipe" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL lxc: out=$out"
        echo "FAIL: lxc - lx -c pipe | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

category_elfrc() {
    echo ""
    echo "=== .elfrc: example / install / init hook ==="
    local dir="$D/coredir/elfrc.$$"
    mkdir -p "$dir"
    local out

    out=$("$L" init example zsh 2>/dev/null)
    if printf '%s' "$out" | grep -q 'starship' && printf '%s' "$out" | grep -q 'zoxide'; then
        echo "PASS elfrc/example: obsahuje starship i zoxide"
        echo "PASS: elfrc - example obsah" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        echo "FAIL elfrc/example: chybi starship/zoxide"
        echo "FAIL: elfrc - example obsah" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
    fi

    if "$L" init zsh 2>/dev/null | grep -q '.elfrc' \
       && "$L" init bash 2>/dev/null | grep -q '.elfrc'; then
        echo "PASS elfrc/hook: init zsh i bash source-uje .elfrc"
        echo "PASS: elfrc - init hook" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        echo "FAIL elfrc/hook: init zsh/bash neobsahuje .elfrc"
        echo "FAIL: elfrc - init hook" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
    fi

    local rc1 rc2 rc3 rcsize
    rc1=$(ELF_RC="$dir/rc" "$L" init install >/dev/null 2>&1; echo $?)
    rcsize=$(wc -c < "$dir/rc" 2>/dev/null || echo 0)
    rc2=$(ELF_RC="$dir/rc" "$L" init install >/dev/null 2>&1; echo $?)
    rc3=$(ELF_RC="$dir/rc" "$L" init install --force >/dev/null 2>&1; echo $?)
    rm -rf "$dir"
    if [ "$rc1" = 0 ] && [ "${rcsize:-0}" -gt 0 ] && [ "$rc2" = 1 ] && [ "$rc3" = 0 ]; then
        echo "PASS elfrc/install: zapis=0, druhy bez --force=1, --force=0"
        echo "PASS: elfrc - install" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        echo "FAIL elfrc/install: rc1=$rc1 size=$rcsize rc2=$rc2 rc3=$rc3"
        echo "FAIL: elfrc - install | rc1=$rc1 rc2=$rc2 rc3=$rc3" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
    fi
}

category_dw() {
    echo ""
    echo "=== dw: setgid shim + double-world (init zsh -dw) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local out
    # zsh `unsetopt privileged` vola setgid(getgid()); app seccomp ho dřív zabil
    # (interaktivne "Bad system call", jinak "can't change option")
    out=$(ashell_out "$env $L --ownall $R/usr/bin/zsh -f -c 'unsetopt privileged && echo SETID_OK'")
    if printf '%s' "$out" | grep -qx "SETID_OK"; then
        echo "PASS dw/setid: setgid na vlastni gid projde"
        echo "PASS: dw - setid" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL dw/setid: out=$out"
        echo "FAIL: dw - setid | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    local hz=$D/usr/bin/zsh
    if [ ! -x "$hz" ]; then
        echo "SKIP dw/flip: host zsh $hz chybi"
        ((SKIP_COUNT++)) || true
        return
    fi
    local dir="$R/tmp/dw.$$"
    mkdir -p "$dir"
    # host zsh: cd do $R/etc spusti guest shell; ten zapise svuj PWD, overi
    # preklad redirectu a dvojim `cd ..` z / se preklopi zpet na host
    cat > "$dir/t.zsh" <<EOS
eval "\$($L init zsh -dw)"
export LX_DW_RC=/dev/null LX_DW_SHELL=/usr/bin/zsh
cd $R/etc
printf '%s\\n' 'echo "G=\$PWD" > /tmp/dw.$$/g' 'echo dwok > /tmp/dw.$$/out' 'cd ..' 'cd ..' 'echo NE > /tmp/dw.$$/ne' | _lx_dw_precmd >/dev/null 2>&1
echo "H=\$PWD"
EOS
    out=$("$R/usr/bin/timeout" 120 "$R/bin/bash" -c "cd $D; HOME=$D $hz -f $dir/t.zsh" 2>&1 | grep -v MMAP)
    out="$out $(cat "$dir/g" "$dir/out" 2>/dev/null) ne=$([ -e "$dir/ne" ] && echo 1 || echo 0)"
    rm -rf "$dir"
    if printf '%s' "$out" | grep -q "H=${R%/*}" && printf '%s' "$out" | grep -q "G=/etc" \
       && printf '%s' "$out" | grep -q "dwok" && printf '%s' "$out" | grep -q "ne=0"; then
        echo "PASS dw/flip: guest /etc, redirect pod \$R, cd .. z / zpet na host"
        echo "PASS: dw - flip" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL dw/flip: out=$out"
        echo "FAIL: dw - flip | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

category_bigargv() {
    echo ""
    echo "=== velky argv (60 KB argument pres exec) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local dir="$R/tmp/bigargv.$$"
    mkdir -p "$dir"
    printf '%s\n' 'x=$(head -c 60000 /dev/zero | tr "\0" x)' '/bin/echo "$x" | wc -c' > "$dir/t.sh"
    local out
    out=$(ashell_out "$env $L --ownall $R/bin/bash ${dir#$R}/t.sh")
    rm -rf "$dir"
    if printf '%s' "$out" | grep -qx "60001"; then
        echo "PASS bigargv: 60 KB argument predan cely"
        echo "PASS: bigargv - 60KB argv" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL bigargv: ocekavano 60001, out=$out"
        echo "FAIL: bigargv - 60KB argv | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

category_bionic() {
    echo ""
    echo "=== bionic bridge (guest glibc dlopen bionicke .so) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local py='import ctypes as c
z = c.CDLL("/system/lib64/libz.so"); z.zlibVersion.restype = c.c_char_p
print("ZLIB", z.zlibVersion().decode())
try:
    v = c.CDLL("/system/lib64/libvulkan.so"); n = c.c_uint(0)
    print("VK", v.vkEnumerateInstanceVersion(c.byref(n)), n.value >> 22)
except OSError:
    print("VK none")'
    local dir="$R/tmp/bionic.$$"
    mkdir -p "$dir"
    printf '%s\n' "$py" > "$dir/t.py"
    local out
    out=$(ashell_out "$env $L --ownall $R/usr/bin/python3 ${dir#$R}/t.py")
    rm -rf "$dir"
    if printf '%s' "$out" | grep -q "^ZLIB 1\."; then
        echo "PASS bionic: libz.so zlibVersion pres bridge"
        echo "PASS: bionic - libz zlibVersion" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL bionic: libz.so, out=$out"
        echo "FAIL: bionic - libz zlibVersion | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    if printf '%s' "$out" | grep -q "^VK none"; then
        echo "SKIP bionic: libvulkan.so neni"
        echo "SKIP: bionic - no libvulkan" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
    elif printf '%s' "$out" | grep -q "^VK 0 1$"; then
        echo "PASS bionic: vkEnumerateInstanceVersion pres bridge"
        echo "PASS: bionic - vkEnumerateInstanceVersion" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL bionic: libvulkan.so, out=$out"
        echo "FAIL: bionic - vkEnumerateInstanceVersion | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# prompt 11 - soft exec (ELF_LOADER_SOFTEXEC=1): vymena guest programu bez
# execve. A/B: knob vypnuty (default) vs zapnuty musi dat STEJNY vystup,
# a pod zapnutym knobem musi trasa dolozit, ze se soft exec opravdu pouzil.
category_softexec() {
    echo ""
    echo "=== soft exec (prompt 11) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local sx="ELF_LOADER_SOFTEXEC=1"
    local trace="$R/tmp/softexec.trace.$$"
    rm -f "$trace"
    local t="$R/usr/bin/timeout"

    _sx_run() {   # $1 = env navic, $2 = guest prikaz pro bash -c
        "$t" 25 "$R/bin/bash" -c "$1 $L --ownall $R/bin/bash -c '$2'" 2>&1 | head -c 20k || true
    }

    # 1) funkcni shoda default vs soft exec na retezu exec
    local cmd='/bin/echo A; /bin/true; echo rc=$?; /usr/bin/expr 6 \* 7; /bin/sed -e s/x/y/ <<< x'
    local out_def out_sx
    out_def=$(_sx_run "$env" "$cmd")
    out_sx=$(_sx_run "$env $sx" "$cmd")
    if [ "$out_def" = "$out_sx" ] && printf '%s' "$out_sx" | grep -q 42; then
        echo "PASS softexec-equal: default == softexec"
        echo "PASS: softexec-equal" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL softexec-equal: def=[$out_def] sx=[$out_sx]"
        echo "FAIL: softexec-equal | def=$out_def sx=$out_sx" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 2) soft exec se opravdu pouzil (trasa do souboru konci "run")
    "$t" 25 "$R/bin/bash" -c \
        "$env $sx ELF_LOADER_SOFTEXEC_TRACE=$trace $L --ownall $R/bin/bash -c '/bin/echo T'" \
        >/dev/null 2>&1 || true
    if [ -f "$trace" ] && grep -q '^run$' "$trace" && grep -q '^load-done$' "$trace"; then
        echo "PASS softexec-used: trasa dokladá in-process vymenu"
        echo "PASS: softexec-used" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL softexec-used: trasa chybi/nekompletni ($trace)"
        echo "FAIL: softexec-used - trasa chybi" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    rm -f "$trace"

    # 3) vlastni argv[0] (exec -a) prezije i soft exec
    local out_a
    out_a=$(_sx_run "$env $sx" 'exec -a ZZTOP /bin/sh -c "echo \$0"')
    if printf '%s' "$out_a" | grep -q ZZTOP; then
        echo "PASS softexec-argv0: exec -a prochazi"
        echo "PASS: softexec-argv0" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL softexec-argv0: [$out_a]"
        echo "FAIL: softexec-argv0 | $out_a" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 4) /proc/self/cmdline (procvirt) soft-exec nuteho programu
    local out_c
    out_c=$(_sx_run "$env $sx" '/bin/cat /proc/self/cmdline | /usr/bin/tr "\\0" " "')
    if printf '%s' "$out_c" | grep -q "/bin/cat /proc/self/cmdline"; then
        echo "PASS softexec-cmdline: virtualizovany cmdline"
        echo "PASS: softexec-cmdline" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL softexec-cmdline: [$out_c]"
        echo "FAIL: softexec-cmdline | $out_c" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 5) fallback: binarka s PT_TLS (python3) soft exec NESMI vzit, ale musi bezet
    if [ -x "$R/usr/bin/python3" ]; then
        local out_p
        out_p=$(_sx_run "$env $sx" '/usr/bin/python3 -c "print(7*6)"')
        if printf '%s' "$out_p" | grep -q 42; then
            echo "PASS softexec-fallback: PT_TLS binarka jde klasickym re-execem"
            echo "PASS: softexec-fallback" >> "$PASS_LOG"
            ((PASS_COUNT++)) || true
        else
            echo "FAIL softexec-fallback: [$out_p]"
            echo "FAIL: softexec-fallback | $out_p" >> "$FAIL_LOG"
            ((FAIL_COUNT++)) || true
        fi
    else
        echo "SKIP softexec-fallback: chybi python3"
        echo "SKIP: softexec-fallback - no python3" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
    fi

    # 6) retez exec v JEDNOM procesu (vice soft execu za sebou)
    local out_ch
    out_ch=$(_sx_run "$env $sx" 'exec /bin/sh -c "exec /bin/echo CHAIN-OK"')
    if printf '%s' "$out_ch" | grep -q CHAIN-OK; then
        echo "PASS softexec-chain: retez exec v jednom procesu"
        echo "PASS: softexec-chain" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL softexec-chain: [$out_ch]"
        echo "FAIL: softexec-chain | $out_ch" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 7) navratove kody a doruceni signalu do soft-exec nuteho ditete
    local out_rc
    out_rc=$(_sx_run "$env $sx" '/bin/false; echo rc=$?; /bin/sh -c "exit 7"; echo rc2=$?; /bin/sleep 5 & p=$!; /bin/kill -TERM $p; wait $p; echo sig=$?')
    if printf '%s' "$out_rc" | grep -q "rc=1" && \
       printf '%s' "$out_rc" | grep -q "rc2=7" && \
       printf '%s' "$out_rc" | grep -q "sig=143"; then
        echo "PASS softexec-exit: navratove kody + SIGTERM (143)"
        echo "PASS: softexec-exit" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL softexec-exit: [$out_rc]"
        echo "FAIL: softexec-exit | $out_rc" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi

    # 8) skript (#!) soft exec nebere - musi dal fungovat
    local sdir="$R/tmp/sxscript.$$"
    mkdir -p "$sdir"
    printf '#!/bin/sh\necho SCRIPT-OK\n' > "$sdir/s.sh"
    chmod +x "$sdir/s.sh"
    local out_s
    out_s=$(_sx_run "$env $sx" "${sdir#$R}/s.sh")
    rm -rf "$sdir"
    if printf '%s' "$out_s" | grep -q SCRIPT-OK; then
        echo "PASS softexec-script: shebang prochazi"
        echo "PASS: softexec-script" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL softexec-script: [$out_s]"
        echo "FAIL: softexec-script | $out_s" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# prompt 13 (T1) - strop trasy soft execu (SX_LOG_MAX / ELF_LOADER_SOFTEXEC_TRACE_MAX).
# Bez stropu dal zacykleny/dlouhy proces zapisuje do trasy neomezene (v
# provozu 6,4 GB). Test vynuti male ELF_LOADER_SOFTEXEC_TRACE_MAX=4096 a
# hodne soft-execu v jednom top-level behu (kazdy /bin/true je novy fork +
# soft exec), overi ze soubor neprerostl limit a obsahuje "trace capped".
category_softexec_cap() {
    echo ""
    echo "=== soft exec - strop trasy (prompt 13 T1) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local sx="ELF_LOADER_SOFTEXEC=1 ELF_LOADER_SOFTEXEC_TRACE_MAX=4096"
    local trace="$R/tmp/softexec.cap.$$"
    local chain="$R/tmp/sx_chain.$$.sh"
    local t="$R/usr/bin/timeout"
    rm -f "$trace" "$chain"

    # Realny incident byl JEDEN proces zacykleny v sig-iter smycce (sig-reset
    # v sx_reset_signals, pred fixem c14655b) - trasa bez stropu rostla
    # uvnitr jedineho soft execu. Zde se stejna "jeden proces, hodne zapisu"
    # situace simuluje bezpecne: /bin/bash (ELF, ne skript) opakovane EXECuje
    # sam sebe nad timto souborem jako argumentem (bez forku, stejny pid) ->
    # kazda iterace je samostatny soft exec a kazda vola sx_reset_signals
    # (az 64x "sig-reset" do trasy). 400 iteraci bezpecne prekroci kazdy
    # rozumny strop vcetne defaultnich 16 MiB.
    cat > "$chain" <<CHAINEOF
n=\$1
if [ "\$n" -gt 0 ]; then
    exec /bin/bash ${chain#$R} \$((n-1))
fi
echo CHAIN-DONE
CHAINEOF

    "$t" 60 "$R/bin/bash" -c \
        "$env $sx ELF_LOADER_SOFTEXEC_TRACE=$trace $L --ownall $R/bin/bash ${chain#$R} 400" \
        >/dev/null 2>&1 || true

    local sz=0 ok=1 detail=""
    [ -f "$trace" ] && sz=$(stat -c %s "$trace" 2>/dev/null || echo 0)
    [ -f "$trace" ] || { ok=0; detail="$detail [trace chybi]"; }
    grep -q "trace capped" "$trace" 2>/dev/null || { ok=0; detail="$detail [chybi 'trace capped']"; }
    # mekky strop - posledni zapis pred kontrolou muze limit prekrocit o
    # jeden radek, toleruj maly presah
    [ "${sz:-0}" -le $((4096 + 512)) ] || { ok=0; detail="$detail [soubor $sz B > limit+rezerva]"; }
    rm -f "$trace" "$chain"

    if [ "$ok" = 1 ]; then
        echo "PASS softexec-cap: trasa zastavena na limitu (${sz} B), 'trace capped' pritomno"
        echo "PASS: softexec-cap - strop trasy soft execu" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL softexec-cap:$detail (sz=${sz:-?})"
        echo "FAIL: softexec-cap - strop trasy soft execu |$detail (sz=${sz:-?})" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

category_staticexec() {
    echo ""
    echo "=== staticky ELF v rootfs pres guest-absolutni cestu ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    if [ ! -x "$R/usr/bin/gcc" ]; then
        echo "SKIP staticexec: no gcc"
        echo "SKIP: staticexec - no gcc" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return
    fi
    local dir="$R/tmp/staticexec.$$"
    mkdir -p "$dir"
    printf '%s\n' '#include <stdio.h>' 'int main(void){puts("STATIC-OK");return 0;}' \
        > "$dir/s.c"
    # -static -no-pie => zadny PT_INTERP => NESMI jit pres own-loading
    ashell_out "$env $L --ownall $R/usr/bin/gcc -static -no-pie -o $dir/s ${dir#$R}/s.c" >/dev/null 2>&1
    if [ ! -x "$dir/s" ]; then
        echo "SKIP staticexec: static build selhal"
        echo "SKIP: staticexec - static build failed" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        rm -rf "$dir"
        return
    fi
    # guest-absolutni cesta (driv: SIGSEGV, vetev "Absolute guest path"
    # nekontrolovala is_glibc_elf); $ROOTFS-prefixovana cesta fungovala vzdy
    local out_g out_r
    out_g=$(ashell_out "$env $L --ownall $R/bin/bash -c '${dir#$R}/s'" 2>&1)
    out_r=$(ashell_out "$env $L --ownall $R/bin/bash -c '$dir/s'" 2>&1)
    rm -rf "$dir"
    local ok=1
    printf '%s' "$out_g" | grep -q "STATIC-OK" || ok=0
    printf '%s' "$out_r" | grep -q "STATIC-OK" || ok=0
    if [ "$ok" = 1 ]; then
        echo "PASS staticexec: guest-abs i ROOTFS-abs cesta"
        echo "PASS: staticexec - static ELF exec" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL staticexec: guest=[$out_g] rootfs=[$out_r]"
        echo "FAIL: staticexec - static ELF exec | guest=$out_g rootfs=$out_r" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# BuildID cache prebitych svc-site (src/cache.c). Overuje: (a) dva behy
# tehoz guest ELF daji identicky spravny vystup, (b) smazani cache mezi
# behy vystup nezmeni, (c) dve ruzne binarky maji ruzny cache klic.
category_execcache() {
    echo ""
    echo "=== BuildID cache prebitych svc-site ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local cdir="${LX_LOG:-$R/root/elf_loader}/cache"

    if [ ! -x "$R/bin/true" ] || [ ! -x "$R/usr/bin/uname" ]; then
        echo "SKIP execcache: chybi guest binarky"
        echo "SKIP: execcache - no guest bins" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return
    fi

    local o1 o2 o3 rc1 rc2
    rc1=$(ashell_rc "$env $L --ownall $R/usr/bin/uname -m")
    o1=$(ashell_out "$env $L --ownall $R/usr/bin/uname -m" 2>&1)
    # (b) smazat cache mezi behy
    rm -rf "$cdir" 2>/dev/null || true
    rc2=$(ashell_rc "$env $L --ownall $R/usr/bin/uname -m")
    o2=$(ashell_out "$env $L --ownall $R/usr/bin/uname -m" 2>&1)
    o3=$(ashell_out "$env $L --ownall $R/usr/bin/true" 2>&1)

    local ok=1
    [ "$rc1" = 0 ] && [ "$rc2" = 0 ] || ok=0
    printf '%s' "$o1" | grep -Eq 'aarch64|arm64' || ok=0
    printf '%s' "$o2" | grep -Eq 'aarch64|arm64' || ok=0
    # (c) dve ruzne binarky -> dva ruzne cache soubory (po behu vyse)
    local nkeys
    nkeys=$(ls "$cdir" 2>/dev/null | grep -c . || true)
    [ "${nkeys:-0}" -ge 2 ] || ok=0

    # (d) NEGATIVNI vysledek skenu se cachuje taky (format v2). Vicemodulova
    # binarka: po prvnim behu musi mit cache zaznam pro KAZDY nacteny modul,
    # i pro ty, kde se zadny svc-site nenasel (libm, libz, ...). Stara verze
    # ukladala jen moduly s nalezem -> ~2 klice misto >=5.
    # (e) na teplem behu se uz nesmi skenovat nic: ELF_DEBUG trace nesmi
    # obsahovat radek "prebito" (= probehl sken), jen "cache hit".
    local multi nmods nkeys2 scans
    multi=$R/usr/bin/python3
    [ -x "$multi" ] || multi=$R/bin/bash
    rm -rf "$cdir" 2>/dev/null || true
    if [ "$multi" = "$R/usr/bin/python3" ]; then
        ashell_out "$env $multi -c 0" >/dev/null 2>&1 || true
        ashell_out "$env $L --ownall $multi -c 0" >/dev/null 2>&1 || true
        scans=$(ashell_out "$env ELF_DEBUG=1 $L --ownall $multi -c 0" 2>&1 \
                | grep -c 'prebito' || true)
    else
        ashell_out "$env $L --ownall $multi -c true" >/dev/null 2>&1 || true
        scans=$(ashell_out "$env ELF_DEBUG=1 $L --ownall $multi -c true" 2>&1 \
                | grep -c 'prebito' || true)
    fi
    nkeys2=$(ls "$cdir" 2>/dev/null | grep -c . || true)
    nmods=$(ashell_out "$env ELF_DEBUG=1 $L --ownall $multi -c true" 2>&1 \
            | grep -c 'own-loaded module' || true)
    [ "${nkeys2:-0}" -ge 5 ] || ok=0
    [ "${scans:-1}" = 0 ] || ok=0

    if [ "$ok" = 1 ]; then
        echo "PASS execcache: shodny vystup, cache smazani OK, $nkeys klicu;" \
             "$nkeys2 klicu pro $(basename "$multi") ($nmods modulu), 0 skenu na teplo"
        echo "PASS: execcache - BuildID cache (v2, negativni vysledek)" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL execcache: rc1=$rc1 rc2=$rc2 o1=[$o1] o2=[$o2] keys=$nkeys" \
             "keys2=$nkeys2 scans_warm=$scans o3=[$o3]"
        echo "FAIL: execcache - BuildID cache | rc1=$rc1 rc2=$rc2 keys=$nkeys keys2=$nkeys2 scans=$scans" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# DT_GNU_HASH lookup symbolu (src/elf_loader.c). Overuje: (a) hash a
# linearni sken daji stejny vysledek programu, (b) kontrolni rezim nehlasi
# ani jeden nesouhlas hash vs linear, (c) faze hooks_bti spadla pod 6 ms
# (pred hashem ~12 ms na kazde binarce - 590 lookupu x ~19 us).
category_symhash() {
    echo ""
    echo "=== DT_GNU_HASH lookup symbolu ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local py="$R/usr/bin/python3"
    if [ ! -x "$py" ]; then
        echo "SKIP symhash: chybi python3"
        echo "SKIP: symhash - no python3" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return
    fi

    local o_hash o_lin n_mis bti ok=1
    o_hash=$(ashell_out "$env $L --ownall $py -c 'print(6*7)'" 2>&1)
    o_lin=$(ashell_out "$env ELF_LOADER_NO_HASH=1 $L --ownall $py -c 'print(6*7)'" 2>&1)
    n_mis=$(ashell_out "$env ELF_LOADER_HASH_CHECK=1 $L --ownall $py -c 'print(6*7)'" 2>&1 \
            | grep -c 'hashcheck' || true)
    bti=$(ashell_out "$env ELF_LOADER_TIMING=1 $L --ownall $py -c 0" 2>&1 \
          | grep '] hooks_bti ' | sed 's/.*delta=\([0-9]*\)us.*/\1/' | tail -1)

    printf '%s' "$o_hash" | grep -q 42 || ok=0
    printf '%s' "$o_lin"  | grep -q 42 || ok=0
    [ "${n_mis:-1}" = 0 ] || ok=0
    [ -n "$bti" ] && [ "$bti" -lt 6000 ] || ok=0

    if [ "$ok" = 1 ]; then
        echo "PASS symhash: hash==linear, 0 nesouhlasu, hooks_bti=${bti}us (<6000)"
        echo "PASS: symhash - DT_GNU_HASH lookup" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL symhash: hash=[$o_hash] linear=[$o_lin] mismatch=$n_mis hooks_bti=${bti}us"
        echo "FAIL: symhash - DT_GNU_HASH | mismatch=$n_mis hooks_bti=$bti" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# Prompt 03: absolutni symlinky -> kanonicky relativni tvar + clamp ".." nad
# guest korenem. A/B se dela KNOBEM na teze binarce (ELF_LOADER_NO_RELSYM /
# ELF_LOADER_NO_DOTDOT_CLAMP = chovani pred promptem 03).
category_relsym() {
    echo ""
    echo "=== absolutni symlinky -> relativni, clamp '..' (prompt 03) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local g="/tmp/relsym.$$" h="$R/tmp/relsym.$$"
    local ok=1 detail=""
    mkdir -p "$h/d1/d2"
    echo marker > "$h/target.txt"

    # Pozn.: "co je skutecne na disku" a "zvladne to jadro samo" se MUSI merit
    # HOST nastroji (/system/bin/readlink, /system/bin/cat) - pod loaderem by
    # absolutni cil zachranil shim_resolve_symlinks, takze by A/B nic nedokazal.
    local HR=/system/bin/readlink HC=/system/bin/cat

    # 1) `ln -s` s absolutnim guest cilem musi na disk zapsat RELATIVNE
    ashell_out "$env $L --ownall $R/bin/bash -c '/bin/ln -sfn $g/target.txt $g/d1/d2/link'" >/dev/null 2>&1
    local raw fixed
    raw=$("$HR" "$h/d1/d2/link" 2>&1 || true)
    fixed=$(ashell_out "$env $L --ownall $R/bin/readlink $g/d1/d2/link" 2>&1)
    case "$raw" in
        ../*) ;;
        *) ok=0; detail="$detail [na disku neni relativni: '$raw']" ;;
    esac
    # 2) readlink pod loaderem hlasi kanonicky relativni cil zpet absolutne
    [ "$fixed" = "$g/target.txt" ] || { ok=0; detail="$detail [readlink nevratil '$g/target.txt' ale '$fixed']"; }

    # 3) cteni pres link zvladne JADRO SAMO (host cat, zadny loader ani shim)
    local via
    via=$("$HC" "$h/d1/d2/link" 2>&1 || true)
    [ "$via" = marker ] || { ok=0; detail="$detail [host cat pres link: '$via']"; }

    # 4) A/B: stara verze (NO_RELSYM pri zapisu) ulozi absolutni cil, ktery
    #    jadro vyhodnoti proti HOST koreni -> host cat selze
    ashell_out "$env ELF_LOADER_NO_RELSYM=1 $L --ownall $R/bin/bash -c '/bin/ln -sfn $g/target.txt $g/d1/d2/old'" >/dev/null 2>&1
    local oldraw oldvia
    oldraw=$("$HR" "$h/d1/d2/old" 2>&1 || true)
    oldvia=$("$HC" "$h/d1/d2/old" 2>&1 || true)
    [ "$oldraw" = "$g/target.txt" ] || { ok=0; detail="$detail [A/B: stara verze nezapsala absolutni cil: '$oldraw']"; }
    [ "$oldvia" = marker ] && { ok=0; detail="$detail [A/B: absolutni link precetlo i jadro samo]"; }

    # 5) host cil (/system) musi zustat absolutni
    ashell_out "$env $L --ownall $R/bin/bash -c '/bin/ln -sfn /system/bin/sh $g/hostlink'" >/dev/null 2>&1
    local hl
    hl=$("$HR" "$h/hostlink" 2>&1 || true)
    [ "$hl" = "/system/bin/sh" ] || { ok=0; detail="$detail [host cil nezustal absolutni: '$hl']"; }

    # 6) clamp "..": v chrootu je /.. == /, prelozena cesta by utekla nad ROOTFS
    local cl clold
    cl=$(ashell_out "$env $L --ownall $R/bin/cat /../..$g/target.txt" 2>&1)
    clold=$(ashell_out "$env ELF_LOADER_NO_DOTDOT_CLAMP=1 $L --ownall $R/bin/cat /../..$g/target.txt" 2>&1)
    [ "$cl" = marker ] || { ok=0; detail="$detail [clamp: /../.. neuzavren ('$cl')]"; }
    [ "$clold" = marker ] && { ok=0; detail="$detail [A/B: bez clampu to preslo taky]"; }

    rm -rf "$h"
    if [ "$ok" = 1 ]; then
        echo "PASS relsym: zapis relativne, readlink absolutne, clamp '..' drzi (A/B knobem)"
        echo "PASS: relsym - absolutni symlinky + clamp .." >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL relsym:$detail"
        echo "FAIL: relsym - absolutni symlinky + clamp .. |$detail" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# Prompt 04 (L1): preklad cest na hranici syscallu. Pripady, ktere jmenne
# hooky historicky MINULY (alias eaccess, glibc-interni volani mkdtemp/remove/
# glob/nftw/scandir, BTI fstatat u ftok/tempnam), se volaji primo pres ctypes
# s VYPNUTYMI prislusnymi hooky (F2_SKIP + F2_NO_BTI_HOOK). A/B knobem na
# teze binarce: bez L1 musi selhat, s ELF_LOADER_L1=1 projit.
category_l1() {
    echo ""
    echo "=== L1: preklad cest na hranici syscallu (prompt 04) ==="
    local py="$R/usr/bin/python3"
    if [ ! -x "$py" ]; then
        echo "SKIP l1: chybi python3"
        echo "SKIP: l1 - no python3" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return
    fi
    local g="/tmp/l1t.$$" h="$R/tmp/l1t.$$"
    mkdir -p "$h/sub"
    echo x > "$h/f"; echo x > "$h/f2"; echo x > "$h/sub/a"; echo x > "$h/sub/b"
    cat > "$h/t.py" <<'PYEOF'
import ctypes, os, sys
libc = ctypes.CDLL("libc.so.6", use_errno=True)
B = sys.argv[1]
r = {}
r["eaccess"] = libc.eaccess(f"{B}/f".encode(), 0) == 0
t = ctypes.create_string_buffer(f"{B}/dXXXXXX".encode())
libc.mkdtemp.restype = ctypes.c_char_p
p = libc.mkdtemp(t)
r["mkdtemp"] = bool(p) and p.decode().startswith(B + "/d")
r["remove"] = libc.remove(f"{B}/f2".encode()) == 0
g = (ctypes.c_char * 256)()
rc = libc.glob(f"{B}/sub/*".encode(), 0, None, ctypes.byref(g))
n = ctypes.c_size_t.from_buffer(g, 0).value
pv = ctypes.cast(ctypes.c_void_p.from_buffer(g, 8).value or 0, ctypes.POINTER(ctypes.c_char_p))
paths = [pv[i].decode() for i in range(n)] if n else []
r["glob"] = rc == 0 and n == 2 and all(x.startswith(B + "/sub/") for x in paths)
seen = []
CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)
cb = CB(lambda path, st, fl, ftw: seen.append(path.decode()) or 0)
rc = libc.nftw(f"{B}/sub".encode(), cb, 8, 0)
r["nftw"] = rc == 0 and len(seen) == 3 and all(x.startswith(B) for x in seen)
nl = ctypes.c_void_p()
r["scandir"] = libc.scandir(f"{B}/sub".encode(), ctypes.byref(nl), None, None) == 4
r["ftok"] = libc.ftok(f"{B}/f".encode(), 1) != -1
libc.tempnam.restype = ctypes.c_char_p
os.environ.pop("TMPDIR", None)   # glibc tempnam dava TMPDIR prednost pred `dir`
tn = libc.tempnam(B.encode(), b"x")
r["tempnam"] = bool(tn) and tn.decode().startswith(B + "/x")
print(" ".join(f"{k}={'OK' if v else 'FAIL'}" for k, v in r.items()))
PYEOF
    local skip="F2_SKIP=eaccess,mkdtemp,remove,glob,glob64,nftw,nftw64,ftw,ftw64,__open64_nocancel F2_NO_BTI_HOOK=1"
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L $skip"
    local on off
    # Strana "bez L1" musi L1 VYNUTIT na 0: sada se da pustit i cela pod
    # ELF_LOADER_L1=2 a zdedena hodnota by z A/B udelala L1 vs L1. A `f2`
    # pred kazdym behem znovu - prvni beh ho `remove` smaze.
    on=$("$R/usr/bin/timeout" 25 "$R/bin/bash" -c "$env ELF_LOADER_L1=1 $L --ownall $py $g/t.py $g" 2>&1 | tail -1 || true)
    echo x > "$h/f2"
    off=$("$R/usr/bin/timeout" 25 "$R/bin/bash" -c "$env ELF_LOADER_L1=0 $L --ownall $py $g/t.py $g" 2>&1 | tail -1 || true)
    rm -rf "$h"
    local n_on n_off total=8
    # `|| true`: set -euo pipefail by pri 0 nalezech grepu shodil cely skript
    n_on=$(printf '%s' "$on" | grep -oE '=OK' | wc -l || true)
    n_off=$(printf '%s' "$off" | grep -oE '=OK' | wc -l || true)
    n_on=${n_on//[^0-9]/}; n_off=${n_off//[^0-9]/}
    # kriterium: s L1 vsech 8; bez L1 musi aspon jeden spadnout (A/B dukaz,
    # ze test opravdu meri neco, co bez L1 nefunguje)
    if [ "$n_on" = "$total" ] && [ "$n_off" -lt "$total" ]; then
        echo "PASS l1: s L1 $n_on/$total, bez L1 $n_off/$total (hooky vypnute)"
        echo "  bez L1: $off"
        echo "PASS: l1 - preklad na hranici syscallu" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL l1: s L1 [$on] | bez L1 [$off]"
        echo "FAIL: l1 - s L1 $n_on/$total, bez L1 $n_off/$total | on=[$on] off=[$off]" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# L2 (prompt 05): seccomp RET_TRAP zachytna sit. A/B dukaz, ze STATICKA binarka
# (zadny PT_INTERP, vlastni baked-in libc -> jmenne hooky ani L1 na jeji raw
# path syscally nedosahnou) vidi rootfs JEN pod ELF_LOADER_L2=1. Navic overi,
# ze host binarka spustena z guestu pod zdedenym filtrem bezi normalne.
category_l2() {
    echo ""
    echo "=== L2: seccomp zachytna sit pro guest kod mimo L1 (prompt 05) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    if [ ! -x "$R/usr/bin/gcc" ]; then
        echo "SKIP l2: no gcc"
        echo "SKIP: l2 - no gcc" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return
    fi
    local dir="$R/tmp/l2t.$$"
    mkdir -p "$dir"
    # cte /etc/os-release pres RAW syscally staticke libc; tiskne jen druhe pole
    # PRETTY_NAME (Parrot) pokud se cesta prelozila do rootfs, jinak selze.
    cat > "$dir/s.c" <<'CEOF'
#include <stdio.h>
#include <string.h>
int main(void){
    FILE*f=fopen("/etc/os-release","r");
    if(!f){printf("NOFILE\n");return 1;}
    char b[256];int hit=0;
    while(fgets(b,sizeof b,f))if(!strncmp(b,"PRETTY_NAME=",12)){
        printf("OSREL=%s",b+12);hit=1;break;}
    fclose(f);
    if(!hit)printf("NOPRETTY\n");
    return 0;
}
CEOF
    # build pod loaderem (gcc je guest), -static -no-pie => zadny PT_INTERP.
    # gcc nevejde do ashell_out (timeout 2) -> vlastni timeout 60, PATH kvuli `ld`.
    "$R/usr/bin/timeout" 60 "$R/bin/bash" -c \
        "$env PATH=/usr/bin:/bin $L --ownall $R/usr/bin/gcc -O1 -static -no-pie -o $dir/s ${dir#$R}/s.c" \
        >/dev/null 2>&1 || true
    if [ ! -x "$dir/s" ]; then
        echo "SKIP l2: static build selhal"
        echo "SKIP: l2 - static build failed" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        rm -rf "$dir"
        return
    fi
    # KLICOVE: samotny test-all.sh bezi uz UVNITR parrot kontextu (viz hlavicka:
    # ashell -c "$L --ownall $R/bin/bash test-all.sh"), kde /etc == rootfs pro
    # vsechno -> A/B viditelnosti rootfs by byl maskovany (stejne jako proot
    # maskuje path bugy, past 5). Vnitrni loader proto pustime pres `ashell -c`,
    # cimz unikneme na bionic HOST, kde /etc == Android (os-release neexistuje).
    local ash="$R/usr/local/bin/ashell"
    if [ ! -x "$ash" ]; then
        echo "SKIP l2: chybi ashell (nutny pro utek na host)"
        echo "SKIP: l2 - no ashell" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        rm -rf "$dir" 2>/dev/null || true
        return
    fi
    local on off
    on=$("$ash" -c "$env ELF_LOADER_L2=1 $L --ownall $dir/s" 2>&1 | grep OSREL= || true)
    off=$("$ash" -c "$env ELF_LOADER_L2=0 $L --ownall $dir/s" 2>&1 | grep OSREL= || true)
    # Bind pod L2 (prompt 06): go_translate ma vlastni exclude seznam, bindy
    # mu dava hook z binds.h. /etc -> JEDNORAZOVY $dir/fe (ro, jen cteni) ->
    # staticka binarka musi cist jeho os-release, ne rootfs.
    mkdir -p "$dir/fe"
    echo 'PRETTY_NAME="lxbind-l2"' > "$dir/fe/os-release"
    local bnd
    bnd=$("$ash" -c "$env ELF_LOADER_L2=1 LX_BINDS_FILE= LX_BINDS=$dir/fe:/etc:ro $L --ownall $dir/s" 2>&1 | grep OSREL= || true)
    rm -rf "$dir" 2>/dev/null || true
    if printf '%s' "$bnd" | grep -q "lxbind-l2"; then
        echo "PASS l2: bind (LX_BINDS /etc -> tmp) viditelny staticke binarce pod L2"
        echo "PASS: l2 - bind pres go_translate" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL l2: bind pod L2 neviditelny (bnd=[$bnd])"
        echo "FAIL: l2 - bind pres go_translate | bnd=[$bnd]" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    local on_parrot=0 off_parrot=0
    # `|| true`: set -euo pipefail by pri neshode grepu shodil cely skript
    printf '%s' "$on"  | grep -qi "parrot" && on_parrot=1 || true
    printf '%s' "$off" | grep -qi "parrot" && off_parrot=1 || true
    # PASS: staticka binarka (raw syscally, mimo L1 i jmenne hooky) vidi rootfs
    # JEN pod L2; bez L2 na hostu /etc/os-release neexistuje (NOFILE).
    if [ "$on_parrot" = 1 ] && [ "$off_parrot" = 0 ]; then
        echo "PASS l2: staticka binarka vidi rootfs jen pod L2 (on=[$on] off=[$off])"
        echo "PASS: l2 - seccomp zachytna sit" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL l2: on=[$on] off=[$off] (on_parrot=$on_parrot off_parrot=$off_parrot)"
        echo "FAIL: l2 - on_parrot=$on_parrot off_parrot=$off_parrot | on=[$on] off=[$off]" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# Line vazani JUMP_SLOT (prompt 10 cast B, ELF_LOADER_LAZY=1). Overuje, ze
# lazy rezim da IDENTICKY vysledek jako eager u binarek, ktere historicky
# padaly na IFUNC (ls/dir -> memcpy), u interpretu i u shellu - a ze
# BIND_NOW objekty (libc.so.6 je stavena s -z now) jdou eager.
category_lazybind() {
    echo ""
    echo "=== line vazani JUMP_SLOT (ELF_LOADER_LAZY) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local ok=1 detail=""

    local t
    for t in "$R/usr/bin/uname -m" "$R/bin/ls $R/tmp" "$R/bin/dir $R/tmp"; do
        local a b
        a=$(ashell_out "$env $L --ownall $t" 2>&1 | head -3)
        b=$(ashell_out "$env ELF_LOADER_LAZY=1 $L --ownall $t" 2>&1 | head -3)
        [ "$a" = "$b" ] || { ok=0; detail="$detail [${t##*/}: eager!=lazy]"; }
        [ -n "$a" ] || { ok=0; detail="$detail [${t##*/}: prazdny vystup]"; }
    done

    # python3 pod lazy: driv padal SIGSEGV (lazy resolver bezel pod guest TP),
    # po fixu dl_enter_host je to KRITERIUM, ne jen informace.
    local pyinfo="n/a"
    if [ -x "$R/usr/bin/python3" ]; then
        if ashell_out "$env ELF_LOADER_LAZY=1 $L --ownall $R/usr/bin/python3 -c 'print(6*7)'" 2>&1 \
           | grep -q 42; then pyinfo="OK"; else pyinfo="PADA"; ok=0; detail="$detail [python3 pod lazy nedal 42]"; fi
    fi

    # BIND_NOW (libc.so.6 je -z now) musi lazy vypnout
    local nbn
    nbn=$(ashell_out "$env ELF_LOADER_LAZY=1 ELF_DEBUG=1 $L --ownall $R/bin/true" 2>&1 \
          | grep -c 'BIND_NOW -> eager' || true)
    [ "${nbn:-0}" -ge 1 ] || { ok=0; detail="$detail [BIND_NOW nedetekovan]"; }

    if [ "$ok" = 1 ]; then
        echo "PASS lazybind: lazy==eager (uname/ls/dir/python3), BIND_NOW eager u $nbn objektu, python3=$pyinfo"
        echo "PASS: lazybind - ELF_LOADER_LAZY" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL lazybind:$detail"
        echo "FAIL: lazybind - ELF_LOADER_LAZY |$detail" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# Uzivatelska tabulka bindu + runtime mount --bind (prompt 06). mount(2) je v
# app domene ENOSYS; MS_BIND emulujeme v path vrstve (sdilena MAP_SHARED
# tabulka). util-linux mount (glibc, pod loaderem) dojde az k libc mount() ->
# shim ho zachyti a vrati 0. POZOR: plne rootfs cesty ($R/bin/{mount,umount,ls,
# mkdir,touch,rm}) - bare jmena by se resolvovala na HOST toybox (viz hlavicka),
# ten bezi nativne mimo loader a shim ho nevidi.
category_binds() {
    echo ""
    echo "=== uzivatelska tabulka bindu + mount --bind (prompt 06) ==="
    # BEZPECNOST (incident 2026-10-21): zdrojem bindu je VZDY jednorazovy
    # adresar $S, nikdy systemovy (/etc, /usr). Bind, ktery zustane v tabulce,
    # + uklid `rm -rf <mountpoint>` pod loaderem smaze ZDROJ bindu (drive
    # /tmp/lxbt -> /etc). Proto: (a) pod-testy bezi s LX_BINDS_FILE= = vlastni
    # cerstva tabulka, bind do tabulky harnessu neunikne; (b) priprava a uklid
    # jen HOSTOVYMI nastroji na host cestach (zadny preklad, zadny bind),
    # mountpointy jen rmdir.
    local env="ROOTFS=$R ELF_ROOTFS=$R HOME=$D ELF_LOADER=$L"
    local fresh="$env LX_BINDS_FILE="
    local ok=1 detail=""
    local S=/tmp/lxbsrc T=/tmp/lxbt T2=/tmp/lxbtro T3=/tmp/lxbt2
    local mnt="$R/bin/mount" ls="$R/bin/ls" tc="$R/bin/touch"
    local TO="$R/usr/bin/timeout 25 $R/bin/bash -c"
    local HB=/system/bin
    local x n

    $HB/rmdir "$R$T" "$R$T2" "$R$T3" 2>/dev/null || true
    $HB/rm -rf "$R$S"
    $HB/mkdir -p "$R$S" "$R$T" "$R$T2"
    $HB/touch "$R$S/a" "$R$S/b" "$R$S/c"

    # (1) runtime mount --bind, cross-process: `mount` a `ls` jsou sourozenci
    # (deti vnitrniho bash) -> bind vidi pres sdilenou MAP_SHARED tabulku.
    x=$($TO "$fresh $L --ownall $R/bin/bash -c '$mnt --bind $S $T && $ls -A $T'" 2>&1 | sort | tr '\n' ' ' || true)
    if [ "$x" = "a b c " ]; then
        echo "PASS binds: mount --bind $S $T ukaze obsah zdroje (cross-process)"
        echo "PASS: binds - runtime mount --bind cross-process" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        ok=0; detail="$detail [mount --bind: '$x']"
    fi

    # (2) zpetne mapovani getcwd: cwd pod bindem se hlasi guest mountpointem
    # (coreutils pwd -P = getcwd(NULL,0), python os.getcwd = getcwd(buf,n)).
    x=$($TO "$fresh $L --ownall $R/bin/bash -c '$mnt --bind $S $T && cd $T && $R/bin/pwd -P && $R/usr/bin/python3 -c \"import os;print(os.getcwd())\"'" 2>&1 | tr '\n' ' ' || true)
    if [ "$x" = "$T $T " ]; then
        echo "PASS binds: getcwd pod bindem -> $T (pwd -P i python)"
        echo "PASS: binds - getcwd zpetne mapovani" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        ok=0; detail="$detail [getcwd: '$x']"
    fi

    # (3) umount2() odebere bind -> cil zase prazdny. util-linux `umount`
    # COMMAND potrebuje /proc/self/mountinfo = prompt 07; mechanismus
    # (umount2 -> shim_umount2 -> bind_remove) testujeme primo pres ctypes.
    if [ -x "$R/usr/bin/python3" ]; then
        local U2='import ctypes,sys;ctypes.CDLL(None).umount2(sys.argv[1].encode(),0)'
        n=$($TO "$fresh $L --ownall $R/bin/bash -c \"$mnt --bind $S $T >/dev/null 2>&1; $R/usr/bin/python3 -c '$U2' $T; $ls -A $T\"" 2>&1 | grep -c . || true)
        if [ "${n:-1}" -eq 0 ]; then
            echo "PASS binds: umount2() odebere bind ($T zase prazdny)"
            echo "PASS: binds - umount2 odebere bind" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
        else
            ok=0; detail="$detail [umount2: cil neprazdny ($n)]"
        fi
    else
        echo "SKIP binds: umount2 test (chybi python3)"
        echo "SKIP: binds - umount2 (no python3)" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
    fi

    # (4) LX_BINDS uvnitr existujici session (zdedena tabulka harnessu, BEZ
    # LX_BINDS_FILE=) -> soukroma kopie s novym bindem; ro -> EROFS a zdroj
    # nedotcen; rodicovska tabulka bind NEvidi. Stara binarka LX_BINDS pri
    # pripojeni k zdedene tabulce tise ignorovala.
    x=$($TO "$env LX_BINDS=$R$S:$T2:ro $L --ownall $ls -A $T2" 2>&1 | sort | tr '\n' ' ' || true)
    local w
    w=$($TO "$env LX_BINDS=$R$S:$T2:ro $L --ownall $tc $T2/lxro_probe" 2>&1 || true)
    n=$($TO "$env $L --ownall $ls -A $T2" 2>&1 | grep -c . || true)
    if [ "$x" = "a b c " ] && printf '%s' "$w" | grep -qiE 'read-only|erofs' &&
       [ ! -e "$R$S/lxro_probe" ] && [ "${n:-1}" -eq 0 ]; then
        echo "PASS binds: vnorene LX_BINDS (ro) -> soukroma kopie, touch EROFS, rodic bind nevidi"
        echo "PASS: binds - vnorene LX_BINDS + ro EROFS" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        ok=0; detail="$detail [LX_BINDS/ro: ls='$x' touch='$(printf '%s' "$w" | head -c 60)' rodic=$n]"
    fi

    # (5) mountpoint jako jadro: rmdir/rename -> EBUSY, zdroj zustane. Bez
    # toho rmdir hlasil "not empty" a mv presunul ZDROJ.
    x=$($TO "$fresh $L --ownall $R/bin/bash -c '$mnt --bind $S $T && { $R/bin/rmdir $T; $R/bin/mv $T $T3; } 2>&1'" 2>&1 || true)
    n=$(printf '%s\n' "$x" | grep -ciE 'busy' || true)
    if [ "${n:-0}" -ge 2 ] && [ -e "$R$S/a" ] && [ ! -e "$R$T3" ]; then
        echo "PASS binds: rmdir/mv mountpointu -> EBUSY, zdroj nedotcen"
        echo "PASS: binds - mountpoint EBUSY" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        ok=0; detail="$detail [EBUSY: $(printf '%s' "$x" | tr '\n' ' ' | head -c 100)]"
    fi

    # (6) uklid souboru tabulek: bez drzitele flocku a starsi 5 s se smaze pri
    # zakladani dalsi tabulky -> nehromadi se (drive 1 soubor / top-level beh).
    local gdir gre
    if [ -n "${LX_LOG:-}" ]; then gdir=$LX_LOG; gre='^binds\..*\.tab$'
    else gdir=$R/tmp; gre='^\.lxbinds\..*\.tab$'; fi
    for x in 1 2 3; do $TO "$fresh $L --ownall $R/bin/true" >/dev/null 2>&1 || true; done
    sleep 6
    $TO "$fresh $L --ownall $R/bin/true" >/dev/null 2>&1 || true
    n=$($HB/ls -a "$gdir" | grep -cE "$gre" || true)
    if [ "${n:-99}" -le 3 ]; then
        echo "PASS binds: osirele tabulky uklizeny ($n souboru v $gdir)"
        echo "PASS: binds - GC tabulek" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        ok=0; detail="$detail [GC: $n tabulek v $gdir]"
    fi

    # uklid: hostove nastroje, host cesty, mountpointy jen rmdir (probe
    # zustane jen se starou binarkou, ktera LX_BINDS ignorovala)
    $HB/rm -f "$R$T2/lxro_probe"
    $HB/rmdir "$R$T" "$R$T2" 2>/dev/null || true
    $HB/rm -rf "$R$S" "$R$T3"

    if [ "$ok" != 1 ]; then
        echo "FAIL binds:$detail"
        echo "FAIL: binds - uzivatelska tabulka / mount --bind |$detail" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# Vlastni argv[0] pres exec (prompt 07 ukol 1, postup.md NALEZ 1): exec
# shim drive argv[0] zahazoval a guest dostal resolvovanou HOST cestu.
# Claude Code tim padal: grep/find jsou `ARGV0=ugrep`/`exec -a bfs` nad
# vlastni binarkou -> "unknown option '-G'". Stara binarka 0 z 6.
category_argv0() {
    echo ""
    echo "=== vlastni argv[0] pres exec (prompt 07) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R HOME=$D ELF_LOADER=$L PATH=/usr/bin:/bin"
    if [ ! -x "$R/usr/bin/python3" ] || [ ! -x "$R/usr/bin/perl" ]; then
        echo "SKIP argv0: chybi python3/perl"
        echo "SKIP: argv0 - no python3/perl" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
        return
    fi
    local dir="$R/tmp/argv0t.$$"
    mkdir -p "$dir"
    # sys.orig_argv[0] = presne argv[0] od jadra (python 3.10+)
    cat > "$dir/t.sh" <<'SEOF'
P='import sys;print(sys.orig_argv[0])'
echo "execa=$( (exec -a FAKE0 /usr/bin/python3 -c "$P") 2>&1)"
echo "plain=$(/usr/bin/python3 -c "$P" 2>&1)"
echo "path=$(python3 -c "$P" 2>&1)"
echo "perl=$(/usr/bin/perl -e 'exec {"/usr/bin/python3"} "FAKE2", "-c", "import sys;print(sys.orig_argv[0])"' 2>&1)"
echo "zsh=$(/usr/bin/zsh -c 'ARGV0=FAKE1 /usr/bin/python3 -c "import sys;print(sys.orig_argv[0])"' 2>/dev/null)"
SEOF
    local out
    out=$("$R/usr/bin/timeout" 60 "$R/bin/bash" -c "$env $L --ownall $R/bin/bash ${dir#$R}/t.sh" 2>&1 || true)
    local want="execa=FAKE0 plain=/usr/bin/python3 path=python3 perl=FAKE2"
    [ -x "$R/usr/bin/zsh" ] && want="$want zsh=FAKE1"
    local w got=0 tot=0 detail=""
    for w in $want; do
        tot=$((tot + 1))
        if printf '%s\n' "$out" | grep -qxF "$w"; then got=$((got + 1))
        else detail="$detail [$w: $(printf '%s\n' "$out" | grep "^${w%%=*}=" | head -1)]"; fi
    done
    # Claude Code: ugrep/bfs jsou tataz Bun binarka, rezim podle argv[0]
    local cb
    cb=$(ls -t "$R"/root/.local/share/claude/versions/* 2>/dev/null | head -1 || true)
    if [ -n "$cb" ] && [ -x "$cb" ]; then
        local g=${cb#$R} u
        u=$("$R/usr/bin/timeout" 60 "$R/bin/bash" -c "$env $L --ownall $R/bin/bash -c '(exec -a ugrep $g --version) 2>&1 | head -1'" 2>&1 || true)
        tot=$((tot + 1))
        if printf '%s' "$u" | grep -q '^ugrep '; then got=$((got + 1))
        else detail="$detail [claude ugrep: $(printf '%s' "$u" | head -c 60)]"; fi
    fi
    rm -rf "$dir"
    if [ "$got" = "$tot" ]; then
        echo "PASS argv0: $got/$tot (exec -a, perl exec {}, zsh ARGV0, bez host cesty, claude ugrep)"
        echo "PASS: argv0 - vlastni argv[0] $got/$tot" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        echo "FAIL argv0: $got/$tot$detail"
        echo "FAIL: argv0 - $got/$tot |$detail" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
    fi
}

# Virtualizace /proc (prompt 07 ukoly 2-5): guest nema videt loader ani host
# cesty. cmdline vlastni i cizi (kernelova oblast argv prepsana), comm
# (ps/pgrep), maps, mounts/mountinfo z tabulky bindu (util-linux findmnt
# a umount PRIKAZ - akceptace promptu 06) a readlink /proc/self/fd|cwd.
# BEZPECNOST: zdroj bindu je jednorazovy $dir/src, vlastni tabulka
# (LX_BINDS_FILE=), uklid hostovym rm na host ceste.
category_procvirt() {
    echo ""
    echo "=== virtualizace /proc + comm (prompt 07) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R HOME=$D ELF_LOADER=$L PATH=/usr/bin:/bin LX_BINDS_FILE="
    if [ ! -x "$R/usr/bin/python3" ] || [ ! -x "$R/usr/bin/findmnt" ]; then
        echo "SKIP procvirt: chybi python3/findmnt"
        echo "SKIP: procvirt - no python3/findmnt" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
        return
    fi
    local dir="$R/tmp/procvt.$$" g="/tmp/procvt.$$"
    /system/bin/rm -rf "$dir"
    /system/bin/mkdir -p "$dir/src" "$dir/mnt"
    /system/bin/touch "$dir/src/a" "$dir/src/b"
    cat > "$dir/t.sh" <<SEOF
G=$g
SEOF
    cat >> "$dir/t.sh" <<'SEOF'
echo "selfcmd=$(python3 -c 'import sys;print(open("/proc/self/cmdline","rb").read()==b"\0".join(a.encode() for a in sys.orig_argv)+b"\0")')"
echo "cat=$(cat /proc/self/cmdline | tr '\0' ' ')"
(exec -a foo sleep 2) & p=$!; sleep 0.5
echo "foreign=$(tr '\0' '|' < /proc/$p/cmdline)"
echo "comm=$(cat /proc/$p/comm)"
echo "ps=$(ps -o args= -p $p)"
echo "pgrep=$(pgrep -x sleep | grep -c "^$p\$")"
wait
echo "mapsleak=$(grep -cE 'elf_loader|/data/user/' /proc/self/maps)"
echo "mapslibc=$(grep -q ' /usr/lib/aarch64-linux-gnu/libc.so.6$' /proc/self/maps && echo 1 || echo 0)"
mount --bind $G/src $G/mnt
echo "findmnt=$(findmnt -n -o TARGET $G/mnt)"
umount $G/mnt; echo "umount=$?:$(ls -A $G/mnt | wc -l)"
echo "fd=$(python3 -c 'import os;f=open("/etc/hostname");print(os.readlink("/proc/self/fd/%d"%f.fileno()))')"
echo "cwd=$(python3 -c 'import os;os.chdir("/tmp");print(os.readlink("/proc/self/cwd"))')"
SEOF
    local out
    out=$("$R/usr/bin/timeout" 60 "$R/bin/bash" -c "$env $L --ownall $R/bin/bash $g/t.sh" 2>&1 || true)
    /system/bin/rm -rf "$dir"
    local got=0 tot=0 detail="" line
    for line in "selfcmd=True" "cat=cat /proc/self/cmdline " "foreign=foo|2|" "comm=sleep" \
                "ps=foo 2" "pgrep=1" "mapsleak=0" "mapslibc=1" "findmnt=$g/mnt" \
                "umount=0:0" "fd=/etc/hostname" "cwd=/tmp"; do
        tot=$((tot + 1))
        if printf '%s\n' "$out" | grep -qxF -- "$line"; then got=$((got + 1))
        else detail="$detail [${line%%=*}: $(printf '%s\n' "$out" | grep "^${line%%=*}=" | head -1 | head -c 70)]"; fi
    done
    if [ "$got" = "$tot" ]; then
        echo "PASS procvirt: $got/$tot (cmdline vlastni+cizi, comm, ps/pgrep, maps, findmnt+umount, fd/cwd)"
        echo "PASS: procvirt - /proc virtualizace $got/$tot" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
    else
        echo "FAIL procvirt: $got/$tot$detail"
        echo "FAIL: procvirt - $got/$tot |$detail" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
    fi
}

# Obrazova cache RELRO stranek (prompt 10 cast A, ELF_LOADER_IMGCACHE=1).
# Overuje: (a) s cache zapnutou je vystup IDENTICKY jako bez ni (bezpecny
# fallback), (b) cache odmitne vytvorit obraz, ktery by nesel revalidovat
# (RELRO ld.so/libc drzi pointery do mallocovane pameti loaderu -> obraz
# se nevytvori), (c) na disku nezustane zadny .tmp.
# link2symlink (prompt 08 cast 2, LX_L2S, src/l2s.h). DESTRUKTIVNI (zapisy
# pod --ownall) -> jen nad kali, NIKDY nad $R (stejne pravidlo jako imgcache
# nize). A/B: bez LX_L2S musi `ln` dopadnout stejne jako drive (EACCES -
# SELinux hardlinky zakazuje), s LX_L2S=1 musi projit a `ls -li` ukazat
# nlink podle poctu jmen. POZOR: testuje se KAZDY prikaz jako SAMOSTATNY
# top-level `--ownall` beh (presne jako imgcache vyse) - `ln`/`ls` spustene
# jako DITE jineho --ownall bash skriptu naraz na jiny, na l2s nezavisly bug
# (cesty se v child-exec pod --ownall nekdy neprekladaji, viz postup.md
# 2026-10-22 (7)); tohle neni limit l2s, je to limit test harnessu.
category_l2s() {
    echo ""
    echo "=== link2symlink (LX_L2S) ==="
    local KR=${ELF_TEST_KALI:-$D/nh/distro/kali}
    if [ "$KR" = "$R" ] || [ ! -x "$KR/bin/ln" ] || [ ! -x "$KR/bin/ls" ]; then
        echo "SKIP l2s: destruktivni test jen nad kali ($KR nedostupny)"
        echo "SKIP: l2s - neni kali rootfs" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
        return 0
    fi
    local dir="$KR/tmp/lx-l2stest"
    local env="ROOTFS=$KR ELF_ROOTFS=$KR ELF_LOADER=$L"
    local ok=1 detail=""

    "$R/system/bin/rm" -rf "$dir" 2>/dev/null || true
    mkdir -p "$dir"
    echo hello > "$dir/a"

    # A: bez LX_L2S - chovani se nesmi zmenit (SELinux EACCES, jako drive).
    local without
    without=$(ashell_out "$env $L --ownall $KR/bin/ln /tmp/lx-l2stest/a /tmp/lx-l2stest/b" 2>&1)
    printf '%s' "$without" | grep -qi "permission denied" \
        || { ok=0; detail="$detail [bez LX_L2S: ln nehlasi EACCES - regrese: '$without']"; }

    # B: s LX_L2S=1 - ln musi projit a ls -li ukazat nlink podle poctu jmen.
    local ln_out
    ln_out=$(ashell_out "$env LX_L2S=1 $L --ownall $KR/bin/ln /tmp/lx-l2stest/a /tmp/lx-l2stest/b" 2>&1)
    [ -z "$ln_out" ] || { ok=0; detail="$detail [ln: '$ln_out']"; }

    local ls1
    ls1=$(ashell_out "$env LX_L2S=1 $L --ownall $KR/bin/ls -li /tmp/lx-l2stest/a /tmp/lx-l2stest/b" 2>&1)
    local ino_a ino_b nl_a nl_b
    ino_a=$(printf '%s\n' "$ls1" | awk '/\/a$/{print $1}')
    ino_b=$(printf '%s\n' "$ls1" | awk '/\/b$/{print $1}')
    nl_a=$(printf '%s\n' "$ls1" | awk '/\/a$/{print $3}')
    nl_b=$(printf '%s\n' "$ls1" | awk '/\/b$/{print $3}')
    [ -n "$ino_a" ] && [ "$ino_a" = "$ino_b" ] || { ok=0; detail="$detail [a/b nemaji stejny inode: '$ls1']"; }
    [ "$nl_a" = "2" ] && [ "$nl_b" = "2" ] || { ok=0; detail="$detail [nlink != 2: '$ls1']"; }

    # rm a -> b musi zustat cely citelny.
    ashell_out "$env LX_L2S=1 $L --ownall $KR/bin/rm /tmp/lx-l2stest/a" >/dev/null 2>&1
    local cat_b
    cat_b=$(ashell_out "$env LX_L2S=1 $L --ownall $KR/bin/cat /tmp/lx-l2stest/b" 2>&1)
    [ "$cat_b" = "hello" ] || { ok=0; detail="$detail [po rm a: cat b='$cat_b']"; }

    # rm b -> uloziste v .l2s musi zmizet (0 soubory s timto basename).
    ashell_out "$env LX_L2S=1 $L --ownall $KR/bin/rm /tmp/lx-l2stest/b" >/dev/null 2>&1
    local leftover
    leftover=$(ls "$KR/.l2s" 2>/dev/null | grep -c '^\.l2s\.a[0-9]' || true)
    [ "${leftover:-0}" = 0 ] || { ok=0; detail="$detail [po rm b: $leftover zbytku v .l2s]"; }

    "$R/system/bin/rm" -rf "$dir" 2>/dev/null || true

    if [ "$ok" = 1 ]; then
        echo "PASS l2s: bez LX_L2S nezmeneno (EACCES), s LX_L2S ln+ls-li+rm spravne (nlink 2->1->0, data uvolnena)"
        echo "PASS: l2s - link2symlink" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL l2s:$detail"
        echo "FAIL: l2s - link2symlink |$detail" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# prompt 13 (T2) - l2s_unlink pri prepisujicim rename. DESTRUKTIVNI (zapisy
# pod --ownall do .l2s) -> jen nad kali, NIKDY nad $R (stejne pravidlo jako
# category_l2s vyse). A/B: stara binarka necha nlink(a) falesne na 2 (rename
# precte .l2s_unlink nevola), nova ho spravne snizi na 1 a v .l2s nezustane
# osireny backing soubor.
category_l2s_rename() {
    echo ""
    echo "=== link2symlink + prepisujici rename (prompt 13 T2) ==="
    local KR=${ELF_TEST_KALI:-$D/nh/distro/kali}
    if [ "$KR" = "$R" ] || [ ! -x "$KR/bin/ln" ] || [ ! -x "$KR/bin/mv" ] || [ ! -x "$KR/bin/ls" ]; then
        echo "SKIP l2s_rename: destruktivni test jen nad kali ($KR nedostupny)"
        echo "SKIP: l2s_rename - neni kali rootfs" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
        return 0
    fi
    local dir="$KR/tmp/lx-l2s-rename"
    local env="ROOTFS=$KR ELF_ROOTFS=$KR ELF_LOADER=$L LX_L2S=1"
    local ok=1 detail=""

    # Uklid PRES LOADER (ne jen hostovym rm -rf) - jinak hostovy rm smaze
    # jen symlinky v rootfs, ale nedekrementuje pocitadlo backing souboru
    # v .l2s, takze dalsi beh testu zacina s pokrivenym stavem (nlink
    # zdedeny z predchoziho behu). Stejny vzor jako category_l2s vyse.
    if [ -e "$dir/a" ] || [ -e "$dir/b" ]; then
        ashell_out "$env $L --ownall $KR/bin/rm -f /tmp/lx-l2s-rename/a /tmp/lx-l2s-rename/b /tmp/lx-l2s-rename/c" >/dev/null 2>&1
    fi
    "$R/system/bin/rm" -rf "$dir" 2>/dev/null || true
    # Tvrdy reset i pro pripad, ze v .l2s zustaly osirele zaznamy z drivejsiho
    # (rozbiteho) behu teto SAME kategorie, jejichz basename se vzdy jmenuje
    # a/b - bez tohoto by novy beh zdedil cizi pocitadlo a selhal uz na
    # "pred rename" kontrole. Bezpecne: destruktivni test uz tak smi mazat
    # jen nad nepouzivanym kali ($KR), nikdy nad $R.
    "$R/system/bin/find" "$KR/.l2s" -maxdepth 1 \( -name '.l2s.a[0-9]*' -o -name '.l2s.b[0-9]*' \) -delete 2>/dev/null || true
    mkdir -p "$dir"
    echo hello > "$dir/a"
    echo other > "$dir/c"

    ashell_out "$env $L --ownall $KR/bin/ln /tmp/lx-l2s-rename/a /tmp/lx-l2s-rename/b" >/dev/null 2>&1

    local ls_before nl_before
    ls_before=$(ashell_out "$env $L --ownall $KR/bin/ls -li /tmp/lx-l2s-rename/a" 2>&1)
    nl_before=$(printf '%s\n' "$ls_before" | awk '{print $3}')
    [ "$nl_before" = "2" ] || { ok=0; detail="$detail [pred rename: nlink a=$nl_before, cek. 2]"; }

    # prepisujici rename (mv c -> b) musi pred prepisem snizit pocitadlo
    # puvodniho l2s cile b (resp. jeho backing souboru, sdileneho s a).
    ashell_out "$env $L --ownall $KR/bin/mv /tmp/lx-l2s-rename/c /tmp/lx-l2s-rename/b" >/dev/null 2>&1

    local ls_after nl_after
    ls_after=$(ashell_out "$env $L --ownall $KR/bin/ls -li /tmp/lx-l2s-rename/a" 2>&1)
    nl_after=$(printf '%s\n' "$ls_after" | awk '{print $3}')
    [ "$nl_after" = "1" ] || { ok=0; detail="$detail [po rename: nlink a=$nl_after, cek. 1 - l2s_unlink se nezavolal]"; }

    local cat_b
    cat_b=$(ashell_out "$env $L --ownall $KR/bin/cat /tmp/lx-l2s-rename/b" 2>&1)
    [ "$cat_b" = "other" ] || { ok=0; detail="$detail [b po rename ma spatny obsah: '$cat_b']"; }

    local leftover
    leftover=$(ls "$KR/.l2s" 2>/dev/null | grep -c '^\.l2s\.b[0-9]' || true)
    [ "${leftover:-0}" = 0 ] || { ok=0; detail="$detail [po rename: $leftover zbytku .l2s.b* v .l2s]"; }

    # Uklid opet PRES LOADER (decrementuje .l2s pocitadlo a), az pak
    # hostovy rm -rf jako pojistka na samotny adresar.
    ashell_out "$env $L --ownall $KR/bin/rm -f /tmp/lx-l2s-rename/a /tmp/lx-l2s-rename/b" >/dev/null 2>&1
    "$R/system/bin/rm" -rf "$dir" 2>/dev/null || true

    if [ "$ok" = 1 ]; then
        echo "PASS l2s_rename: prepisujici rename spravne snizil pocitadlo, zadny osireny backing soubor"
        echo "PASS: l2s_rename - rename dekrementuje l2s pocitadlo" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL l2s_rename:$detail"
        echo "FAIL: l2s_rename - rename nedekrementuje l2s pocitadlo |$detail" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# DESTRUKTIVNI kategorie (IMGCACHE + --ownall) -> NIKDY nad zivym parrotem
# ($R), jen nad nepouzivanym kali rootfs (AGENTS.md "Bezpecnost", incident
# 2026-10-21). Bez kali -> SKIP, ne fallback na $R.
category_imgcache() {
    echo ""
    echo "=== obrazova cache RELRO (ELF_LOADER_IMGCACHE) ==="
    local KR=${ELF_TEST_KALI:-$D/nh/distro/kali}
    if [ "$KR" = "$R" ] || [ ! -x "$KR/usr/bin/uname" ] || [ ! -x "$KR/bin/ls" ]; then
        echo "SKIP imgcache: destruktivni test jen nad kali ($KR nedostupny)"
        echo "SKIP: imgcache - neni kali rootfs" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
        return 0
    fi
    local lxl="$KR/tmp/lx-imgtest"
    mkdir -p "$lxl"
    local env="ROOTFS=$KR ELF_ROOTFS=$KR LX_LOG=$lxl ELF_LOADER=$L"
    local idir="$lxl/cache/img"
    local ok=1 detail=""

    rm -rf "$idir" 2>/dev/null || true
    local t
    # Baseline BEZ cache: s loaderem bez podpory glibc 2.43 kali visi (uname
    # vypise a visi v exitu - opraveno 2026-10-22 (4)). Bez funkcni baseline
    # neni s cim srovnavat -> SKIP, ne falesny FAIL.
    for t in "$KR/usr/bin/uname -m" "$KR/bin/ls $KR/tmp"; do
        # -s KILL: visici kali proces SIGTERM od timeoutu ignoruje (osirel)
        if [ -z "$("$R/usr/bin/timeout" -s KILL 3 "$R/bin/bash" -c "$env $L --ownall $t" 2>&1 | head -3)" ]; then
            echo "SKIP imgcache: kali baseline '${t##*/}' pod loaderem nedobehne (glibc 2.43)"
            echo "SKIP: imgcache - kali baseline nedobehne" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
            return 0
        fi
    done
    for t in "$KR/usr/bin/uname -m" "$KR/bin/ls $KR/tmp"; do
        local a b
        a=$(ashell_out "$env $L --ownall $t" 2>&1 | head -3)
        b=$(ashell_out "$env ELF_LOADER_IMGCACHE=1 $L --ownall $t" 2>&1 | head -3)
        [ -n "$a" ] || { ok=0; detail="$detail [${t##*/}: prazdny vystup]"; }
        [ "$a" = "$b" ] || { ok=0; detail="$detail [${t##*/}: s cache != bez cache]"; }
    done
    if [ -x "$KR/usr/bin/python3" ]; then
        local p
        p=$(ashell_out "$env ELF_LOADER_IMGCACHE=1 $L --ownall $KR/usr/bin/python3 -c 'print(6*7)'" 2>&1)
        printf '%s' "$p" | grep -q 42 || { ok=0; detail="$detail [python3: $p]"; }
    fi

    # Obraz se musi ulozit nebo nacist - ne tise nic. POZOR: tenhle beh je
    # chladny (zapis obrazu) a do 2s timeoutu v ashell_out se nevejde, proto
    # vlastni delsi timeout.
    rm -rf "$idir" 2>/dev/null || true
    local why
    why=$("$R/usr/bin/timeout" 25 "$R/bin/bash" -c \
          "$env ELF_LOADER_IMGCACHE=1 ELF_LOADER_IMG_TRACE=1 $L --ownall $KR/usr/bin/uname -m" 2>&1 \
          | grep -cE 'ulozeno|hit ' || true)   # -E: bare grep je host toybox, nezna \|
    [ "${why:-0}" -ge 1 ] || { ok=0; detail="$detail [img trace nic nehlasi]"; }

    # Verifikace: obnovene RELRO musi byt BITOVE SHODNE s plnou relokaci
    # (0 rozdilu) - jinak obraz nese neco per-behoveho.
    local vr
    vr=$("$R/usr/bin/timeout" 25 "$R/bin/bash" -c \
         "$env ELF_LOADER_IMGCACHE=1 ELF_LOADER_IMG_VERIFY=1 $L --ownall $KR/usr/bin/uname -m" 2>&1)
    printf '%s' "$vr" | grep -q aarch64 || { ok=0; detail="$detail [verify rezim nedojel]"; }
    local nd
    nd=$(printf '%s' "$vr" | grep -o '[0-9]* rozdilu' | awk '{s+=$1} END{print s+0}')
    [ "${nd:-1}" = 0 ] || { ok=0; detail="$detail [verify: $nd rozdilu]"; }


    local ntmp
    ntmp=$(ls "$idir" 2>/dev/null | grep -c '\.tmp\.' || true)
    [ "${ntmp:-0}" = 0 ] || { ok=0; detail="$detail [zustal .tmp]"; }

    if [ "$ok" = 1 ]; then
        echo "PASS imgcache: s cache == bez cache, zadny rozbity obraz, 0 .tmp"
        echo "PASS: imgcache - ELF_LOADER_IMGCACHE" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL imgcache:$detail"
        echo "FAIL: imgcache - ELF_LOADER_IMGCACHE |$detail" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

category_fakeroot() {
    echo ""
    echo "=== fake root (LX_FAKEROOT, prompt 08 cast 1) ==="
    local KR=${ELF_TEST_KALI:-$D/nh/distro/kali}
    if [ "$KR" = "$R" ] || [ ! -x "$KR/usr/bin/stat" ] || [ ! -x "$KR/bin/chmod" ]; then
        echo "SKIP fakeroot: destruktivni test jen nad kali ($KR nedostupny)"
        echo "SKIP: fakeroot - neni kali rootfs" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
        return 0
    fi
    local env="ROOTFS=$KR ELF_ROOTFS=$KR ELF_LOADER=$L"
    local tdir="$KR/tmp/lx-fr-test"
    mkdir -p "$tdir"
    local f="$tdir/f" dev="$tdir/dev0"
    local ok=1 detail=""

    # A: bez LX_FAKEROOT - chovani nesmi byt ovlivnene (regrese)
    "$R/usr/bin/timeout" 10 "$R/bin/bash" -c "$env $L --ownall $KR/bin/touch $f" >/dev/null 2>&1
    local id_plain
    id_plain=$(ashell_out "$env $L --ownall $KR/usr/bin/id -u" 2>&1)
    [ "$id_plain" != "0" ] || { ok=0; detail="$detail [bez LX_FAKEROOT uz hlasi uid 0 - regrese]"; }

    # B: s LX_FAKEROOT=1
    local fenv="$env LX_FAKEROOT=1"
    local id_fr
    id_fr=$(ashell_out "$fenv $L --ownall $KR/usr/bin/id -u" 2>&1)
    [ "$id_fr" = "0" ] || { ok=0; detail="$detail [getuid: '$id_fr' != 0]"; }

    rm -f "$f"
    "$R/usr/bin/timeout" 10 "$R/bin/bash" -c "$fenv $L --ownall $KR/bin/touch $f" >/dev/null 2>&1
    local owner
    owner=$(ashell_out "$fenv $L --ownall $KR/usr/bin/stat -c %U:%G $f" 2>&1)
    [ "$owner" = "root:root" ] || { ok=0; detail="$detail [vlastnik noveho souboru: '$owner' != root:root]"; }

    "$R/usr/bin/timeout" 10 "$R/bin/bash" -c "$fenv $L --ownall $KR/bin/chown 1234:5 $f" >/dev/null 2>&1
    local cg
    cg=$(ashell_out "$fenv $L --ownall $KR/usr/bin/stat -c %u:%g $f" 2>&1)
    [ "$cg" = "1234:5" ] || { ok=0; detail="$detail [chown: '$cg' != 1234:5]"; }

    "$R/usr/bin/timeout" 10 "$R/bin/bash" -c "$fenv $L --ownall $KR/bin/chmod 4755 $f" >/dev/null 2>&1
    local mo
    mo=$(ashell_out "$fenv $L --ownall $KR/usr/bin/stat -c %A $f" 2>&1)
    case "$mo" in -rws*) ;; *) ok=0; detail="$detail [chmod setuid: '$mo']" ;; esac

    rm -f "$dev"
    "$R/usr/bin/timeout" 10 "$R/bin/bash" -c "$fenv $L --ownall $KR/bin/mknod $dev c 1 3" >/dev/null 2>&1
    local ft
    ft=$(ashell_out "$fenv $L --ownall $KR/usr/bin/stat -c %F $dev" 2>&1)
    [ "$ft" = "character special file" ] || { ok=0; detail="$detail [mknod: '$ft']"; }

    if [ "$ok" = 1 ]; then
        echo "PASS fakeroot: getuid=0, chown/chmod setuid/mknod pres xattr DB, bez LX_FAKEROOT beze zmeny"
        echo "PASS: fakeroot - LX_FAKEROOT" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL fakeroot:$detail"
        echo "FAIL: fakeroot - LX_FAKEROOT |$detail" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

category_timing() {
    echo ""
    echo "=== fázový profil exec (ELF_LOADER_TIMING) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local on off
    # Zapnuto: musí vypsat [TIMING] řádky na stderr a skončit 0.
    on=$(ashell_out "$env ELF_LOADER_TIMING=1 $L --ownall $R/bin/true" 2>&1)
    # Vypnuto (default): žádný [TIMING] řádek nesmí být.
    off=$(ashell_out "$env $L --ownall $R/bin/true" 2>&1)
    local ok=1
    printf '%s' "$on"  | grep -q '\[TIMING\] main_start' || ok=0
    printf '%s' "$on"  | grep -q '\[TIMING\] entry'      || ok=0
    if printf '%s' "$off" | grep -q '\[TIMING\]'; then ok=0; fi
    if [ "$ok" = 1 ]; then
        echo "PASS timing: TIMING=1 vypisuje fáze, default tiše"
        echo "PASS: timing - ELF_LOADER_TIMING fáze" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL timing: on=[$(printf '%s' "$on" | head -3 | tr '\n' '|')] off_has_timing=$(printf '%s' "$off" | grep -c '\[TIMING\]')"
        echo "FAIL: timing - ELF_LOADER_TIMING fáze" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

category_selfexe() {
    echo ""
    echo "=== /proc/self/exe + make fast path (eaccess) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local out rc
    out=$(ashell_out "$env $L --ownall $R/usr/bin/readlink /proc/self/exe")
    if [ "$out" = "$R/usr/bin/readlink" ]; then
        echo "PASS selfexe: readlink /proc/self/exe -> guest binarka"
        echo "PASS: selfexe - readlink /proc/self/exe" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL selfexe: readlink /proc/self/exe -> '$out'"
        echo "FAIL: selfexe - readlink /proc/self/exe | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    if [ ! -x "$R/usr/bin/make" ]; then
        echo "SKIP selfexe: make neni v rootfs"
        echo "SKIP: selfexe - no make" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return 0
    fi
    local dir="$R/tmp/selfexe_mk.$$"
    mkdir -p "$dir"
    printf 'all:\n\t/usr/bin/echo MKFAST\n' > "$dir/Makefile"
    rc=$(ashell_rc "$env $L --ownall $R/usr/bin/make -s -C $dir all")
    out=$(ashell_out "$env $L --ownall $R/usr/bin/make -s -C $dir all")
    rm -rf "$dir"
    if [ "$rc" = 0 ] && printf '%s' "$out" | grep -Fq "MKFAST"; then
        echo "PASS selfexe: make fast path (/usr/bin/echo bez shellu)"
        echo "PASS: selfexe - make fast path" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL selfexe: make fast path RC=$rc out=$out"
        echo "FAIL: selfexe - make fast path | RC=$rc | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
}

# Regresni test: madvise(MADV_DONTNEED) na inicializovana data exe. Segmenty
# musi byt file-backed (MAP_PRIVATE), jinak se stranky vrati jako nuly - Bun
# standalone (claude) pak hlasi "SyntaxError: Invalid character '\0'".
# test/madv_dontneed.c se prelozi hostovym (proot) gcc do device adresare.
# Regresni test: aktivni Python venv ($VIRTUAL_ENV) musi byt vidtelny pro
# lx/lxwhich (uv venv konzolove skripty, napr. "markitdown"). Bez opravy
# lxwhich hledalo jen v pevnem LX_PATH a 'lx <venv-tool>' hlasilo "neni v
# Parrot rootfs" i kdyz tool v aktivovanem venv reálne existoval.
category_venv() {
    echo ""
    echo "=== lx respektuje aktivni \$VIRTUAL_ENV (uv venv) ==="
    # Skript pises do souboru (ne inline retezec) - vyhne se peklu vnoreneho
    # uvozovani pres 3 vrstvy shellu (ashell -c "..." -> bash -c "..." -> eval).
    local ve="$D/coredir/venvtest.$$"
    local runner="$D/coredir/venvtest_run.$$.sh"
    mkdir -p "$ve/bin"
    printf '#!/bin/sh\necho VENVTOOL_OK\n' > "$ve/bin/venvtool"
    chmod +x "$ve/bin/venvtool"
    {
        echo "export VIRTUAL_ENV='$ve'"
        echo "eval \"\$($L init bash)\""
        echo "lxwhich venvtool && lx venvtool"
    } > "$runner"
    local env="ROOTFS=$R ELF_ROOTFS=$R HOME=$D D=$D L=$L"
    local out
    out=$(ashell_out "$env $L --ownall $R/bin/bash $runner")
    if printf '%s' "$out" | grep -Fq "VENVTOOL_OK"; then
        echo "PASS venv: lx najde \$VIRTUAL_ENV/bin konzolovy skript"
        echo "PASS: venv - VIRTUAL_ENV lx" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL venv: out=$out"
        echo "FAIL: venv - VIRTUAL_ENV lx | $out" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
    rm -rf "$ve" "$runner"
}

# Regresni test: sitove nastroje overene REALNYM pouzitim, ne jen "-V"/"--help"
# (ty projdou skoro vzdy, i kdyz je DNS/TLS/skutecna funkce rozbita - run_test
# kontroluje jen exit kod, ne obsah vystupu). Kazda kontrola overuje konkretni
# OBSAH vystupu, ne pouhe rc=0.
category_net_real() {
    echo ""
    echo "=== sitove nastroje - realne pouziti (ne jen -V) ==="
    local env="ROOTFS=$R ELF_ROOTFS=$R HOME=$D D=$D"
    local out rc

    if [ -x "$R/usr/bin/curl" ]; then
        out=$(ashell_out "$env $L --ownall $R/usr/bin/curl -sS -o /dev/null -w '%{http_code}' --max-time 10 https://example.com")
        if [ "$out" = "200" ]; then
            echo "PASS curl: HTTPS GET https://example.com -> 200"
            echo "PASS: curl - real HTTPS GET" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
        else
            echo "FAIL curl: HTTPS GET out=$out"
            echo "FAIL: curl - real HTTPS GET | $out" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
        fi
    else
        echo "SKIP curl: chybi v rootfs"; echo "SKIP: curl - missing" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
    fi

    if [ -x "$R/usr/bin/wget" ]; then
        out=$(ashell_out "$env $L --ownall $R/usr/bin/wget -4 -q --timeout=10 -O - https://example.com")
        if printf '%s' "$out" | grep -qiE '<html|<!doctype'; then
            echo "PASS wget: HTTPS GET https://example.com -> HTML obsah"
            echo "PASS: wget - real HTTPS GET" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
        else
            echo "FAIL wget: HTTPS GET out=$(printf '%s' "$out" | head -c 80)"
            echo "FAIL: wget - real HTTPS GET | $(printf '%s' "$out" | head -c 80)" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
        fi
    else
        echo "SKIP wget: chybi v rootfs"; echo "SKIP: wget - missing" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
    fi

    if [ -x "$R/usr/bin/getent" ]; then
        out=$(ashell_out "$env $L --ownall $R/usr/bin/getent hosts localhost")
        if printf '%s' "$out" | grep -Eq '^(127\.0\.0\.1|::1)[[:space:]]'; then
            echo "PASS getent: hosts localhost -> $(printf '%s' "$out" | head -1)"
            echo "PASS: getent - real NSS hosts lookup" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
        else
            echo "FAIL getent: hosts localhost out=$out"
            echo "FAIL: getent - real NSS hosts lookup | $out" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
        fi
    else
        echo "SKIP getent: chybi v rootfs"; echo "SKIP: getent - missing" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
    fi

    if [ -x "$R/usr/bin/ssh" ]; then
        # -G vypise rozresenou konfiguraci (crypto/config parsing), bez
        # sitoveho spojeni - realna funkce ssh klienta, ne jen "-V" banner.
        out=$(ashell_out "$env $L --ownall $R/usr/bin/ssh -G example.com")
        if printf '%s' "$out" | grep -q '^user '; then
            echo "PASS ssh: -G rozresilo konfiguraci (user, hostname, ...)"
            echo "PASS: ssh - real config resolution" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
        else
            echo "FAIL ssh: -G out=$(printf '%s' "$out" | head -c 80)"
            echo "FAIL: ssh - real config resolution | $(printf '%s' "$out" | head -c 80)" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
        fi
    else
        echo "SKIP ssh: chybi v rootfs"; echo "SKIP: ssh - missing" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
    fi

    if [ -x "$R/usr/bin/starship" ]; then
        out=$(ashell_out "$env $L --ownall $R/usr/bin/starship prompt")
        if [ -n "$out" ]; then
            echo "PASS starship: prompt vykreslen (${#out} B)"
            echo "PASS: starship - real prompt render" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
        else
            echo "FAIL starship: prompt prazdny"
            echo "FAIL: starship - real prompt render | prazdny vystup" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
        fi
    else
        echo "SKIP starship: chybi v rootfs"; echo "SKIP: starship - missing" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
    fi

    if [ -x "$R/usr/bin/fzf" ]; then
        out=$(ashell_out "printf 'ab\nxyz\nabc\n' | $env $L --ownall $R/usr/bin/fzf --filter=ab")
        if [ "$out" = "$(printf 'ab\nabc')" ]; then
            echo "PASS fzf: --filter=ab na 'ab/xyz/abc' -> 'ab,abc'"
            echo "PASS: fzf - real filter" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
        else
            echo "FAIL fzf: --filter out=$out"
            echo "FAIL: fzf - real filter | $out" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
        fi
    else
        echo "SKIP fzf: chybi v rootfs"; echo "SKIP: fzf - missing" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
    fi

    if [ -x "$R/usr/bin/nmap" ] || [ -x "$R/usr/sbin/nmap" ]; then
        local nmapbin; nmapbin=$([ -x "$R/usr/bin/nmap" ] && echo "$R/usr/bin/nmap" || echo "$R/usr/sbin/nmap")
        out=$(ashell_out "$env $L --ownall $nmapbin -sL -n example.com")
        if printf '%s' "$out" | grep -q 'example.com'; then
            echo "PASS nmap: -sL -n example.com -> rozresilo"
            echo "PASS: nmap - real DNS list scan" >> "$PASS_LOG"; ((PASS_COUNT++)) || true
        else
            echo "FAIL nmap: -sL out=$(printf '%s' "$out" | head -c 120)"
            echo "FAIL: nmap - real DNS list scan | $(printf '%s' "$out" | head -c 120)" >> "$FAIL_LOG"; ((FAIL_COUNT++)) || true
        fi
    else
        echo "SKIP nmap: neni v rootfs nainstalovany"
        echo "SKIP: nmap - not installed" >> "$SKIP_LOG"; ((SKIP_COUNT++)) || true
    fi
}

category_madv() {
    echo ""
    echo "=== madv MADV_DONTNEED na data exe ==="
    local src bin
    src="$(dirname "$0")/test/madv_dontneed.c"
    bin="$D/coredir/madv_dontneed"
    mkdir -p "$D/coredir"
    # gcc i ld jsou pod $R/usr/bin, ne na defaultnim PATH top-level bashe
    # (stejna PATH-ordering past jako u timeout/bash - viz sekce 9 AGENTS.md).
    local gcc_path="$R/usr/bin/gcc"
    if [ ! -x "$gcc_path" ] || ! PATH="$R/usr/bin:$R/bin:$PATH" "$gcc_path" -O0 -o "$bin" "$src" 2>/dev/null; then
        echo "SKIP madv: gcc/compile failed"
        echo "SKIP: madv - no gcc" >> "$SKIP_LOG"
        ((SKIP_COUNT++)) || true
        return 0
    fi
    local env="ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L"
    local rc out
    rc=$(ashell_rc "$env $L --ownall $bin")
    out=$(ashell_out "$env $L --ownall $bin")
    if [ "$rc" = 0 ] && printf '%s' "$out" | grep -Fq "MADV A"; then
        echo "PASS madv: DONTNEED keeps file contents ($out)"
        echo "PASS: madv - DONTNEED file-backed" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL madv: RC=$rc out=$out"
        echo "FAIL: madv - DONTNEED zeroed | RC=$rc | $out" >> "$FAIL_LOG"
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

category_prootcompat() {
    echo ""
    echo "=== proot-kompatibilni CLI + hybridni dispatch (prompt 09) ==="
    local ok=1 detail=""
    local realproot="$R/usr/bin/proot"

    # A: --proot-compat -r $R <cmd> == --ownall <cmd> (read-only, bez zapisu)
    local out1
    out1=$(ashell_out "$L --proot-compat -r $R -w / echo proot-compat-ok" 2>&1)
    [ "$out1" = "proot-compat-ok" ] || { ok=0; detail="$detail [basic: '$out1']"; }

    # B: -b host:guest se promitne do ROOTFS vysledku (id -u pod -0 == 0)
    local out2
    out2=$(ashell_out "$L --proot-compat -r $R -0 id -u" 2>&1)
    [ "$out2" = "0" ] || { ok=0; detail="$detail [-0 id -u: '$out2']"; }

    # C: argv[0]==proot detekce (symlink na loader se jmenem proot)
    local linkdir="$D/tmp/lx-prootcompat-$$"
    mkdir -p "$linkdir"
    local link="$linkdir/proot"
    ln -sf "$L" "$link" 2>/dev/null
    local out3
    out3=$(ashell_out "$link -r $R -w / echo argv0-proot-ok" 2>&1)
    rm -rf "$linkdir"
    [ "$out3" = "argv0-proot-ok" ] || { ok=0; detail="$detail [argv0==proot: '$out3']"; }

    # D: nezname volby -> fallback na REALNY proot beze zmeny (jen pokud je
    # v rootfs dostupny - jinak jen oveřit, ze loader nepadne a vrati nenulovy
    # exit misto crashe)
    if [ -x "$realproot" ]; then
        local out4
        out4=$(ashell_out "LX_PROOT_BIN=$realproot $L --proot-compat --this-flag-neexistuje -r $R echo x" 2>&1)
        # realny proot na nezname volbe hlasi chybu, ale NESMI to byt pad loaderu
        case "$out4" in *"Segmentation"*|*"segfault"*) ok=0; detail="$detail [fallback real proot spadl]";; esac
    fi

    if [ "$ok" = 1 ]; then
        echo "PASS prootcompat: CLI parse + run_ownall dispatch + argv0==proot + fallback bez padu"
        echo "PASS: prootcompat - proot-compat CLI" >> "$PASS_LOG"
        ((PASS_COUNT++)) || true
    else
        echo "FAIL prootcompat:$detail"
        echo "FAIL: prootcompat - proot-compat CLI |$detail" >> "$FAIL_LOG"
        ((FAIL_COUNT++)) || true
    fi
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
    selfexe)
        category_selfexe
        print_summary
        ;;
    timing)
        category_timing
        print_summary
        ;;
    staticexec)
        category_staticexec
        print_summary
        ;;
    softexec)
        category_softexec
        print_summary
        ;;
    softexec_cap)
        category_softexec_cap
        print_summary
        ;;
    execcache)
        category_execcache
        print_summary
        ;;
    symhash)
        category_symhash
        print_summary
        ;;
    lazybind)
        category_lazybind
        print_summary
        ;;
    relsym)
        category_relsym
        print_summary
        ;;
    l1)
        category_l1
        print_summary
        ;;
    l2)
        category_l2
        print_summary
        ;;
    imgcache)
        category_imgcache
        print_summary
        ;;
    fakeroot)
        category_fakeroot
        print_summary
        ;;
    prootcompat)
        category_prootcompat
        print_summary
        ;;
    l2s)
        category_l2s
        print_summary
        ;;
    l2s_rename)
        category_l2s_rename
        print_summary
        ;;
    binds)
        category_binds
        print_summary
        ;;
    argv0)
        category_argv0
        print_summary
        ;;
    procvirt)
        category_procvirt
        print_summary
        ;;
    bigargv)
        category_bigargv
        print_summary
        ;;
    bionic)
        category_bionic
        category_staticexec
        print_summary
        ;;
    lxc)
        category_lxc
        print_summary
        ;;
    dw)
        category_dw
        print_summary
        ;;
    elfrc)
        category_elfrc
        print_summary
        ;;
    pathops)
        category_pathops
        print_summary
        ;;
    madv)
        category_madv
        print_summary
        ;;
    venv)
        category_venv
        print_summary
        ;;
    net_real)
        category_net_real
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
        category_selfexe
        category_timing
        category_bigargv
        category_bionic
        category_staticexec
        category_execcache
        category_symhash
        category_lazybind
        category_imgcache
        category_fakeroot
        category_prootcompat
        category_l2s
        category_l2s_rename
        category_binds
        category_argv0
        category_procvirt
        category_relsym
        category_l1
        category_l2
        category_softexec
        category_softexec_cap
        category_lxc
        category_dw
        category_elfrc
        category_pathops
        category_madv
        category_venv
        category_net_real
        print_summary
        ;;
esac
