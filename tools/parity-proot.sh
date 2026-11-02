#!/bin/sh
# Srovnavaci matice: co vsechno musi guest videt/umet. Bezi stejne pod
# real proot -0 i pod loaderem --proot-compat -0.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
P=/mbtest
say(){ printf '%s\n' "$*"; }

say "### A) IDENTITA"
say "A1 id=$(id -u):$(id -g)"
say "A2 id_full=$(id)"
say "A3 groups=$(id -G)"
say "A4 sh_uid=$(cat /proc/self/status | sed -n 's/^Uid:.//p')"

say "### B) FAKEROOT OPERACE"
rm -f $P/f1 $P/f2 $P/f3 $P/n1 $P/n2 2>/dev/null
: > $P/f1
chown 1234:5678 $P/f1; say "B1 chown_rc=$? stat=$(stat -c '%u:%g' $P/f1)"
chown 0:0 $P/f1;    say "B2 chown0_rc=$? stat=$(stat -c '%u:%g' $P/f1)"
chgrp 999 $P/f1;    say "B3 chgrp_rc=$? stat=$(stat -c '%u:%g' $P/f1)"
chmod 4755 $P/f1;   say "B4 chmod_suid=$? mode=$(stat -c '%a' $P/f1)"
chmod 2750 $P/f1;   say "B5 chmod_sgid=$? mode=$(stat -c '%a' $P/f1)"
chmod 1777 $P/f1;   say "B6 chmod_sticky=$? mode=$(stat -c '%a' $P/f1)"
mknod $P/n1 c 1 3;  say "B7 mknod_c_rc=$? -> $(stat -c '%F %t:%T' $P/n1 2>&1)"
mknod $P/n2 b 7 0;  say "B8 mknod_b_rc=$? -> $(stat -c '%F %t:%T' $P/n2 2>&1)"
say "B9 touch_root=$(touch $P/f2; echo rc=$?) stat=$(stat -c '%u:%g' $P/f2)"
say "B10 etc_shadow=$(stat -c '%u:%g %a' /etc/shadow 2>&1)"

say "### C) BIND MOUNT uvnitr guesta"
rm -rf $P/src $P/dst $P/ro; mkdir -p $P/src $P/dst $P/ro
echo hello > $P/src/f.txt
mount --bind $P/src $P/dst; say "C1 mount_rc=$?"
say "C2 cat=$(cat $P/dst/f.txt 2>&1)"
echo world > $P/dst/w.txt; say "C3 write_rc=$? src_sees=$(cat $P/src/w.txt 2>&1)"
mount --bind -o ro $P/src $P/ro; say "C4 ro_mount_rc=$?"
echo x > $P/ro/x.txt 2>/dev/null; say "C5 ro_write_rc=$?"
mount -o remount,ro $P/dst 2>/dev/null; say "C6 remount_ro_rc=$?"
say "C7 rm_on_bind=$(rm -rf $P/dst 2>&1; echo rc=$?)"
say "C8 src_survived=$(ls $P/src 2>&1 | tr '\n' ',')"
umount $P/dst 2>/dev/null; say "C9 umount_rc=$?"
umount $P/ro 2>/dev/null;  say "C10 umount_ro_rc=$?"

say "### D) POHLED NA MOUNTY (nesmi videt host)"
say "D1 findmnt=$(findmnt -n -T $P 2>&1 | head -1)"
say "D2 proc_mounts=$(grep -c . /proc/mounts 2>&1) lines; mbtest=$(grep -c mbtest /proc/mounts 2>&1)"
say "D3 mountinfo_lines=$(grep -c . /proc/self/mountinfo 2>&1)"
say "D4 mtab=$(grep -c . /etc/mtab 2>&1)"

say "### E) NESMI SI MYSLET, ZE JE VENKU"
say "E1 cwd=$(pwd)"
say "E2 ls_root=$(ls / 2>&1 | tr '\n' ' ')"
say "E3 root_stat=$(stat -c '%u:%g' / 2>&1)"
say "E4 host_path_visible=$([ -e /data/user/0/com.linux_core/files/nh/distro/parrot/etc/passwd ] && echo ANO || echo ne)"
say "E5 tmp_is=$(stat -c '%u:%g %a' /tmp 2>&1)"
say "E6 sh=$(readlink -f /bin/sh 2>&1)"
say "E7 hostname=$(hostname 2>&1)"
say "E8 uname=$(uname -r 2>&1)"
