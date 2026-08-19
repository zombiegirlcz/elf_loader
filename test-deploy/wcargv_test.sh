#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
echo "== debug argv of the wc exec from bash =="
/data/local/tmp/bsh -c 'INTERP_WRAPPER_DEBUG=1 wc -l /data/user/0/com.linux_core/files/nh/distro/parrot/etc/passwd' 2>&1 | grep -E '^\[iw\]|^\[-\]|^open|BASH'
echo "BASH=$?"