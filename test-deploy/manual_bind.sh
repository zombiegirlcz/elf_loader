#!/system/bin/sh
echo "==== Manual bind mount test ===="
MODDIR=/debug_ramdisk/parrot_elf_loader
if [ ! -d "$MODDIR" ]; then
    echo "Module not mounted at $MODDIR"
    ls -la /debug_ramdisk/
    exit 1
fi

echo "Module dir: $MODDIR"
ls -la "$MODDIR/system/lib/ld-linux-aarch64.so.1"

mkdir -p /lib
if ! mountpoint -q /lib/ld-linux-aarch64.so.1 2>/dev/null; then
    mount -o bind "$MODDIR/system/lib/ld-linux-aarch64.so.1" /lib/ld-linux-aarch64.so.1 2>/dev/null
    echo "Bind mount created"
else
    echo "Already mounted"
fi

ls -la /lib/ld-linux-aarch64.so.1