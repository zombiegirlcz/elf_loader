#!/bin/bash
# TDD regression test: child process (fork+exec) must inherit rootfs search
# paths even when ROOTFS is NOT exported by the caller.
#
# Root cause: tar -czf forks+execs gzip via shim_execve/execvp. The re-exec'd
# elf_loader child only finds the guest libc if ROOTFS/ELF_ROOTFS/LD_LIBRARY_PATH
# are present in the environment. If the caller (ashell -c, ad-hoc launcher)
# does not export ROOTFS, the child loader cannot locate the parrot libdirs and
# dies with "[-] dep libc.so not found".
#
# The loader already derives the distro root from the exe path
# (derive_distro_libdirs/find_distro_root); it must also propagate that to the
# environment handed to children.
#
# RED before fix, GREEN after.
set -uo pipefail

L=${L:-/data/user/0/com.linux_core/files/usr/bin/elf_loader}
R=${R:-/data/user/0/com.linux_core/files/nh/distro/parrot}

FAIL=0

# Note: NO `export ROOTFS=...` here on purpose.
ashell -c "\
T=$R/tmp/child_env_t; \
/system/bin/rm -rf \$T; /system/bin/mkdir -p \$T/d; /system/bin/echo hello > \$T/d/a; \
$L --ownall $R/usr/bin/tar -czf \$T/t.tgz -C \$T d 2>\$T/err.txt; echo tar_rc=\$?; \
$L --ownall $R/usr/bin/tar -tzf \$T/t.tgz 2>>\$T/err.txt; echo list_rc=\$?; \
/system/bin/ls -la \$T/t.tgz; \
/system/bin/cat \$T/err.txt" > /tmp/child_env.out 2>&1

echo "--- test output ---"
cat /tmp/child_env.out

grep -q 'tar_rc=0' /tmp/child_env.out || { echo "FAIL: tar -czf rc != 0"; FAIL=1; }
grep -q 'list_rc=0' /tmp/child_env.out || { echo "FAIL: tar -tzf rc != 0"; FAIL=1; }
grep -q 'dep libc.so not found' /tmp/child_env.out && { echo "FAIL: child loader lost rootfs (dep libc.so not found)"; FAIL=1; }
grep -qE 'd/a' /tmp/child_env.out || { echo "FAIL: archive does not list d/a (child gzip failed)"; FAIL=1; }

if [ "$FAIL" = 0 ]; then
    echo "PASS: child inherited rootfs search paths without ROOTFS in env"
    exit 0
fi
echo "RED: child does not inherit rootfs search paths"
exit 1
