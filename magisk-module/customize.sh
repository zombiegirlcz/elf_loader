#!/system/bin/sh
# ELF Loader - install-time setup.
#
# Co se dela:
#   1) zjisti rootfs a zapise ho do /data/adb/elf_loader_root (stary nazev
#      /data/adb/parrot_root se cte kvuli migraci, novy se zapisuje)
#   2) zkopiruje bionic zsh + support do /data/adb/zsh-bionic (Magisk mount
#      /system neni videt v app namespace - viz README)
#   3) uklidi pozustatky starych verzi modulu (parrot/linuxsh/patchelf)
MODDIR=${0%/*}

mkdir -p /data/adb 2>/dev/null

# --- 1) rootfs detekce -------------------------------------------------------
# univerzalni: adresar, ktery vypada jako glibc rootfs (ma libc.so.6)
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
    if [ -f /data/adb/parrot_root ]; then
        cp -f /data/adb/parrot_root /data/adb/elf_loader_root
        echo "elf_loader: prevzat rootfs z /data/adb/parrot_root"
    elif RF=$(detect_rootfs); then
        echo "$RF" > /data/adb/elf_loader_root
        echo "elf_loader: nalezen rootfs $RF"
    else
        echo "elf_loader: rootfs nenalezen - nastav ho pozdeji:"
        echo "            echo /cesta/k/rootfs > /data/adb/elf_loader_root"
    fi
else
    echo "elf_loader: zachovavam existujici /data/adb/elf_loader_root"
fi

# --- 2) bionic zsh (host shell) + support --------------------------------
if [ -f "$MODDIR/system/bin/zsh" ]; then
    cp "$MODDIR/system/bin/zsh"  /data/adb/zsh  2>/dev/null
    cp "$MODDIR/system/bin/bzsh" /data/adb/bzsh 2>/dev/null
    chmod 755 /data/adb/zsh /data/adb/bzsh 2>/dev/null
    rm -rf /data/adb/zsh-bionic 2>/dev/null
    cp -r "$MODDIR/system/etc/zsh-bionic" /data/adb/zsh-bionic 2>/dev/null
fi

# --- 3) uklid starych verzi modulu --------------------------------------
# parrot wrapper, linuxsh (chroot), PT_INTERP patcher - uz nejsou soucasti
rm -f /data/adb/linuxsh /data/adb/linuxsh-root /data/adb/gbsh \
      /data/adb/patchelf_interp /data/adb/parrot-fix-exec 2>/dev/null
# stary interpreter bridge (bind mount, uz nepouzivany)
umount /lib/ld-linux-aarch64.so.1 2>/dev/null

exit 0