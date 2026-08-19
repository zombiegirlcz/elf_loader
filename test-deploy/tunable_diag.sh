#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader

echo "==== ls (raw stderr) ===="
env LD_LIBRARY_PATH=$L $L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/ls $R/etc 2>&1 | head -30