#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader
BASH=/data/local/tmp/bsh

export LD_LIBRARY_PATH=$L

echo "==== parrot-fix-exec approach (PT_INTERP patch) ===="
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "wc -l $R/etc/passwd" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated"
echo "wc exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "grep daemon $R/etc/passwd" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated"
echo "grep exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "stat $R/etc/passwd" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated"
echo "stat exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "ls $R/etc" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated" | head -5
echo "ls exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "ls $R/etc | wc -l" 2>&1 | grep -vE "^\[dbg\]|libc mp|^\[\+\]|relocated"
echo "pipe exit: $?"