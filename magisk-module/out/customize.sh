#!/system/bin/sh
# Parrot ELF Loader - install-time setup.
# 1) Write /data/adb/parrot_root if it does not exist yet:
#    - reuse the app-provided rootfs if present
#    - otherwise default to /data/adb/parrot (user must place a rootfs there)
MODDIR=${0%/*}

mkdir -p /data/adb 2>/dev/null

if [ ! -f /data/adb/parrot_root ]; then
    if [ -d /data/user/0/com.linux_core/files/nh/distro/parrot ]; then
        echo "/data/user/0/com.linux_core/files/nh/distro/parrot" > /data/adb/parrot_root
        echo "parrot: using existing app rootfs"
    else
        echo "/data/adb/parrot" > /data/adb/parrot_root
        echo "parrot: default rootfs /data/adb/parrot"
        echo "       place a Debian/parrot aarch64 rootfs there (see README)."
    fi
else
    echo "parrot: keeping existing /data/adb/parrot_root"
fi

exit 0