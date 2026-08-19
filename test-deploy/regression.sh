#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader
BASH=/data/local/tmp/bsh

echo "==== Direct loader tests ===="
export LD_LIBRARY_PATH=$L
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/wc -l $R/etc/passwd
echo "wc exit: $?"

$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/ls $R/etc
echo "ls exit: $?"

echo "==== Bridge tests (patched PT_INTERP) ===="
$BASH -c "wc -l $R/etc/passwd"
echo "bridge wc exit: $?"

$BASH -c "sed -n 2p $R/etc/passwd"
echo "bridge sed exit: $?"