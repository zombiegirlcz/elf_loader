#!/system/bin/sh
# ELF Loader - service.sh (bezi po bootu jako root).
#
# Jen idempotentni doplneni toho, co customize.sh uz udelal - kopie do
# /data/adb prezijou reinstalaci modulu i kdyz Magisk mount jeste neni aktivni.
MODDIR=${0%/*}

# univerzalni detekce rootfs (stejna logika jako customize.sh)
detect_rootfs() {
    for base in /data/user/0/*/files /data/data/*/files /data/adb/*/files; do
        if [ -f "$base/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so.6" ]; then
            echo "$base/nh/distro/parrot"; return 0
        fi
    done
    for cand in /data/adb/rootfs /data/adb/elf_rootfs; do
        if [ -f "$cand/usr/lib/aarch64-linux-gnu/libc.so.6" ]; then
            echo "$cand"; return 0
        fi
    done
    return 1
}

if [ ! -f /data/adb/elf_loader_root ]; then
    mkdir -p /data/adb
    if [ -f /data/adb/parrot_root ]; then
        cp -f /data/adb/parrot_root /data/adb/elf_loader_root
    elif RF=$(detect_rootfs); then
        echo "$RF" > /data/adb/elf_loader_root
    fi
fi

# bionic zsh + support (kopie mimo Magisk mount, viditelna ve vsech namespace)
if [ -f "$MODDIR/system/bin/zsh" ] && [ ! -f /data/adb/zsh ]; then
    cp "$MODDIR/system/bin/zsh"  /data/adb/zsh  2>/dev/null
    cp "$MODDIR/system/bin/bzsh" /data/adb/bzsh 2>/dev/null
    chmod 755 /data/adb/zsh /data/adb/bzsh 2>/dev/null
    [ -d /data/adb/zsh-bionic ] || cp -r "$MODDIR/system/etc/zsh-bionic" /data/adb/zsh-bionic 2>/dev/null
fi

exit 0