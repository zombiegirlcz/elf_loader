#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader
BASH=/data/local/tmp/bsh

export LD_LIBRARY_PATH=$L

echo "==== Direct loader tests ===="
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/ls $R/etc >/dev/null
echo "ls exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/sed -n 2p $R/etc/passwd >/dev/null
echo "sed exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/cat $R/etc/passwd >/dev/null
echo "cat exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/date >/dev/null 2>&1
echo "date exit: $?"

echo "==== Bridge tests (patched PT_INTERP) ===="
$BASH -c "ls $R/etc" >/dev/null
echo "bridge ls exit: $?"

$BASH -c "sed -n 2p $R/etc/passwd" >/dev/null
echo "bridge sed exit: $?"