#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader

echo "==== Direct loader sed ===="
export LD_LIBRARY_PATH=$L
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/sed -n 2p $R/etc/passwd
echo "sed exit: $?"

echo "==== Direct loader cat ===="
$L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/cat $R/etc/passwd | head -3
echo "cat exit: $?"