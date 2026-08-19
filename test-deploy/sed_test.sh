#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
echo "== sed via kernel interp, direct =="
/data/local/tmp/sed -n 2p "$R/etc/passwd" 2>&1 | tail -3
echo "SED=$?"
echo "== sed from rootfs path via kernel interp =="
"$R/usr/bin/sed" -n 2p "$R/etc/passwd" 2>&1 | tail -3
echo "SED2=$?"