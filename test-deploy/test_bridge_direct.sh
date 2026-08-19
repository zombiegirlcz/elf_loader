#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader
LDL=/data/local/tmp/ldl

echo "==== Direct bridge test ===="
$LDL --library-path $L $ELF --ownall $R/usr/bin/wc -l $R/etc/passwd
echo "bridge wc exit: $?"