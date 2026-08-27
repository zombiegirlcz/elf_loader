#!/system/bin/sh
# Parrot ELF Loader - install-time setup.
# 1) Write /data/adb/parrot_root if it does not exist yet:
#    - reuse the app-provided rootfs if present
#    - otherwise default to /data/adb/parrot (user must place a rootfs there)
# 2) Copy linuxsh wrapper to /data/adb so it is available immediately,
#    even before reboot applies the Magisk mounts.
MODDIR=${0%/*}

# univerzalni detekce rootfs: prvni adresar obsahujici nh/distro/parrot
# pod beznyma app cestama (bez hardcoded jmena aplikace)
detect_rootfs() {
    for base in /data/user/0/*/files /data/data/*/files /data/adb/*/files; do
        if [ -d "$base/nh/distro/parrot" ]; then
            echo "$base/nh/distro/parrot"; return 0
        fi
    done
    return 1
}

mkdir -p /data/adb 2>/dev/null

if [ ! -f /data/adb/parrot_root ]; then
    if RF=$(detect_rootfs); then
        echo "$RF" > /data/adb/parrot_root
        echo "parrot: using detected rootfs $RF"
    else
        echo "/data/adb/parrot" > /data/adb/parrot_root
        echo "parrot: default rootfs /data/adb/parrot"
        echo "       place a Debian/parrot aarch64 rootfs there (see README)."
    fi
else
    echo "parrot: keeping existing /data/adb/parrot_root"
fi
# linuxsh + gbsh: nativni shelly - dostupne i pred rebootem
if [ -f "$MODDIR/system/bin/linuxsh" ]; then
    cp "$MODDIR/system/bin/linuxsh"      /data/adb/linuxsh      2>/dev/null
    cp "$MODDIR/system/bin/linuxsh-root" /data/adb/linuxsh-root 2>/dev/null
    chmod 755 /data/adb/linuxsh /data/adb/linuxsh-root 2>/dev/null
fi
if [ -f "$MODDIR/system/bin/gbsh" ]; then
    cp "$MODDIR/system/bin/gbsh" /data/adb/gbsh 2>/dev/null
    chmod 755 /data/adb/gbsh 2>/dev/null
fi

exit 0
