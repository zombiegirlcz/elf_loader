#!/system/bin/sh
echo "==== Extract module and manual bind mount ===="
cd /data/local/tmp
unzip -o /data/user/0/com.linux_core/files/nh/distro/parrot/root/elf_loader/magisk-module/parrot_elf_loader.zip -d /data/local/tmp/parrot_module 2>&1 | tail -5

MODDIR=/data/local/tmp/parrot_module
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

echo "==== Test fork/exec from parrot shell ===="
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader
BASH=/data/local/tmp/bsh

# Start parrot bash and test fork/exec
export LD_LIBRARY_PATH=$L
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "wc -l $R/etc/passwd" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated"
echo "wc exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "grep daemon $R/etc/passwd" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated"
echo "grep exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "stat $R/etc/passwd" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated"
echo "stat exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "ls $R/etc" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated" | head -5
echo "ls exit: $?"

# Test pipe (two fork/exec)
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "ls $R/etc | wc -l" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated"
echo "pipe exit: $?"