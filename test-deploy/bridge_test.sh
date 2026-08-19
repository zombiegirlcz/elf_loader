#!/system/bin/sh
R=/data/user/0/com.linux_core/files/nh/distro/parrot
echo "== bash via kernel interp =="
/data/local/tmp/bsh -c 'echo fork-exec-works'
echo "BASH=$?"
echo "== bash builtin + external wc (fork/exec) =="
/data/local/tmp/bsh -c 'wc -l $R/etc/passwd'
echo "BASH2=$?"
echo "== sed via kernel interp =="
/data/local/tmp/sed -n 2p "$R/etc/passwd"
echo "SED=$?"
echo "== grep via kernel interp =="
/data/local/tmp/grep -c daemon "$R/etc/passwd"
echo "GREP=$?"