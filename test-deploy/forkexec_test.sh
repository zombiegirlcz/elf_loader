#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
D=$R/root/elf_loader/test-deploy
cp "$R/usr/bin/wc" "$R/usr/bin/wc.orig"
python3 "$D/patch_interp.py" "$R/usr/bin/wc" "$R/usr/bin/wc" /data/local/tmp/ldl
echo "== bash fork/exec of rootfs wc via interp bridge =="
/data/local/tmp/bsh -c "wc -l $R/etc/passwd"
echo "BASH=$?"
echo "== bash exec of sed =="
/data/local/tmp/bsh -c "sed -n 2p $R/etc/passwd"
echo "SED=$?"