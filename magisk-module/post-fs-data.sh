#!/system/bin/sh
# Parrot ELF Loader - post-fs-data.sh (runs early before Zygote starts).
# Interpreter bridge for fork/exec from within parrot shell.
MODDIR=${0%/*}

# Interpreter bridge for fork/exec
if [ -f "$MODDIR/system/lib/ld-linux-aarch64.so.1" ]; then
    mkdir -p /lib
    if ! mountpoint -q /lib/ld-linux-aarch64.so.1 2>/dev/null; then
        mount -o bind "$MODDIR/system/lib/ld-linux-aarch64.so.1" \
            /lib/ld-linux-aarch64.so.1 2>/dev/null
    fi
fi

exit 0