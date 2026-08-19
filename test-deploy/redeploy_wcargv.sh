#!/system/bin/sh
D=/data/user/0/com.linux_core/files/nh/distro/parrot/root/elf_loader/test-deploy
cp $D/interp_wrapper_static /data/local/tmp/ldl
chmod 755 /data/local/tmp/ldl
sh $D/wcargv_test.sh 2>&1 | grep -E '^\[iw\]|^\[-\]|^open|BASH'