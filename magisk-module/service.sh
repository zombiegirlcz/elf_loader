#!/system/bin/sh
# Parrot ELF Loader - service.sh (runs late after boot as root).
# Experimental: expose /lib/ld-linux-aarch64.so.1 so fork/exec of parrot
# binaries from within a parrot shell also works.  The kernel needs the
# interpreter path to exist at the literal path.
MODDIR=${0%/*}

# 1) Rootfs config sanity
if [ ! -f /data/adb/parrot_root ]; then
    mkdir -p /data/adb
    if [ -d /data/user/0/com.linux_core/files/nh/distro/parrot ]; then
        echo "/data/user/0/com.linux_core/files/nh/distro/parrot" > /data/adb/parrot_root
    else
        echo "/data/adb/parrot" > /data/adb/parrot_root
    fi
fi

exit 0