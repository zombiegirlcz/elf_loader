#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader

echo "==== Test with MALLOC_MMAP_MAX_=2048 override (known present) ===="
env MALLOC_MMAP_MAX_=2048 LD_LIBRARY_PATH=$L $L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $R/usr/bin/wc -l $R/etc/passwd 2>&1 | grep -E "tunable.*getenv|tunable.*override|wc exit"
echo "wc exit: $?"