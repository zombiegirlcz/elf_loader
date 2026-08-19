#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
ELF=/data/local/tmp/elf_loader
BASH=/data/local/tmp/bsh

echo "==== Direct parrot shell fork/exec (no grep pipes) ===="

# Test 1: wc
env LD_LIBRARY_PATH=$L $L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "wc -l $R/etc/passwd"
echo "wc exit: $?"

# Test 2: grep (using parrot grep, not host)
env LD_LIBRARY_PATH=$L $L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "$R/usr/bin/grep daemon $R/etc/passwd"
echo "grep exit: $?"

# Test 3: stat
env LD_LIBRARY_PATH=$L $L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "stat $R/etc/passwd"
echo "stat exit: $?"

# Test 4: ls
env LD_LIBRARY_PATH=$L $L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "ls $R/etc"
echo "ls exit: $?"

# Test 5: pipe with explicit parrot tools
env LD_LIBRARY_PATH=$L $L/ld-linux-aarch64.so.1 --library-path $L $ELF --ownall $BASH -c "$R/usr/bin/ls $R/etc | $R/usr/bin/wc -l"
echo "pipe exit: $?"