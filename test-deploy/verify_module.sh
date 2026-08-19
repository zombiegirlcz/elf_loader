#!/system/bin/sh
echo "==== 1. Modul status & bind mount ===="
ls -la /lib/ld-linux-aarch64.so.1 2>&1 || echo "NOT FOUND"
ls -la /data/adb/modules/ 2>&1 | head -5
cat /data/adb/modules/parrot_elf_loader/module.prop 2>&1 || echo "module.prop not found"
echo "==== Magisk status ===="
which magisk
magisk -v 2>&1
magisk --list 2>&1 | head -10
echo "==== service.sh content ===="
cat /data/adb/modules/parrot_elf_loader/service.sh 2>&1 || echo "service.sh not found"