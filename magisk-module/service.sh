#!/system/bin/sh
# Parrot ELF Loader - service.sh (bezi po bootu jako root).

MODDIR=${0%/*}

# 1) Rootfs konfigurace: /data/adb/parrot_root obsahuje cestu k rootfs.
if [ ! -f /data/adb/parrot_root ]; then
    mkdir -p /data/adb
    if [ -d /data/user/0/com.linux_core/files/nh/distro/parrot ]; then
        echo "/data/user/0/com.linux_core/files/nh/distro/parrot" > /data/adb/parrot_root
    else
        echo "/data/adb/parrot" > /data/adb/parrot_root
    fi
fi

# 2) linuxsh dostupny hned po bootu (kopie mimo Magisk mount, viditelna
#    ve vsech namespace - Magisk mounty nejsou videt v app namespace).
if [ -f "$MODDIR/system/bin/linuxsh" ] && [ ! -f /data/adb/linuxsh ]; then
    cp "$MODDIR/system/bin/linuxsh"     /data/adb/linuxsh      2>/dev/null
    cp "$MODDIR/system/bin/linuxsh-root" /data/adb/linuxsh-root 2>/dev/null
    chmod 755 /data/adb/linuxsh /data/adb/linuxsh-root 2>/dev/null
fi
if [ -f "$MODDIR/system/bin/gbsh" ] && [ ! -f /data/adb/gbsh ]; then
    cp "$MODDIR/system/bin/gbsh" /data/adb/gbsh 2>/dev/null
    chmod 755 /data/adb/gbsh 2>/dev/null
fi

exit 0
