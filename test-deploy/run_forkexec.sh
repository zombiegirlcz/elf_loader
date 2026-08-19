#!/system/bin/sh
sh /data/user/0/com.linux_core/files/nh/distro/parrot/root/elf_loader/test-deploy/forkexec_test.sh 2>&1 | grep -vE '\[\+\]|\[dbg\]|libc mp|relocated'