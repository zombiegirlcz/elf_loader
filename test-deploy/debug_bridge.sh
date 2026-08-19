#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader
BASH=/data/local/tmp/bsh

export LD_LIBRARY_PATH=$L
echo "==== Bridge wc ===="
$BASH -c "wc -l $R/etc/passwd"
echo "wc exit: $?"