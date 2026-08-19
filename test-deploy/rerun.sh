#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
D=$R/root/elf_loader/test-deploy
cp $D/interp_wrapper_static /data/local/tmp/ldl
chmod 755 /data/local/tmp/ldl
sh $D/run_forkexec.sh 2>&1 | grep -E '^\[iw\]|^==|^BASH|^SED|^open|^\[-\]'