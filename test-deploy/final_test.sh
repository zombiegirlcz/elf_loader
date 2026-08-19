#!/system/bin/sh
# Final end-to-end validation of the module components on the device.
R=/data/user/0/com.linux_core/files/nh/distro/parrot
L=$R/usr/lib/aarch64-linux-gnu
D=$R/root/elf_loader/test-deploy

echo "== 1. parrot-script equivalent (direct load, no bridge) =="
export LD_LIBRARY_PATH=$L
"$L/ld-linux-aarch64.so.1" --library-path "$L" /data/local/tmp/elf_loader --ownall "$R/usr/bin/wc" -l "$R/etc/passwd" 2>&1 | grep -vE '^\[dbg\]|libc mp|^\[\+\]|relocated' | tail -1
echo "parrot_wc=$?"
unset LD_LIBRARY_PATH

echo "== 2. kernel interpreter bridge (patched rootfs wc) =="
"$R/usr/bin/wc" -l "$R/etc/passwd" 2>&1 | grep -vE '^\[dbg\]|libc mp|^\[\+\]|relocated' | tail -1
echo "bridge_wc=$?"

echo "== 3. bash fork/exec sed through bridge =="
/data/local/tmp/bsh -c "sed -n 2p $R/etc/passwd" 2>&1 | grep -vE '^\[dbg\]|libc mp|^\[\+\]|relocated' | tail -1
echo "bash_sed=$?"

echo "== 4. bridge debug argv =="
INTERP_WRAPPER_DEBUG=1 "$R/usr/bin/wc" -l "$R/etc/passwd" 2>&1 | grep -E '^\[iw\] (execfn|argv\[5\])'