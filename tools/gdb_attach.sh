#!/system/bin/sh
# 2) chroot POUZE pro gdb (attach k jiz bezicimu PID, ne exec).
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
BB=/data/adb/magisk/busybox
PID=$(cat $D/tmp/gdb_pid.txt)
command -v toybox >/dev/null 2>&1 && UNS="toybox unshare" || UNS="unshare"
exec $UNS -m sh -c '
R="'"$R"'"
BB="'"$BB"'"
PID="'"$PID"'"
if [ -x "$BB" ]; then
    "$BB" mount --make-rprivate / 2>/dev/null || "$BB" mount --make-rslave / 2>/dev/null
fi
/system/bin/mkdir -p "$R/proc" 2>/dev/null
/system/bin/mount --bind /proc "$R/proc" 2>&1
chroot "$R" /usr/bin/gdb -q -batch \
    -ex "set pagination off" \
    -ex "handle SIGSEGV stop print nopass" \
    -ex "attach $PID" \
    -ex "continue" \
    -ex "echo ===STOPPED===\n" \
    -ex "info registers" \
    -ex "bt full" \
    -ex "x/10i \$pc"
'
