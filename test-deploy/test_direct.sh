#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader

export LD_LIBRARY_PATH=$L
echo "==== Direct loader cat (no pipe) ===="
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/cat $R/etc/passwd
echo "cat exit: $?"

echo "==== Direct loader wc ===="
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/wc -l $R/etc/passwd
echo "wc exit: $?"

echo "==== Direct loader ls ===="
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/ls $R/etc
echo "ls exit: $?"

echo "==== Direct loader date ===="
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/date
echo "date exit: $?"