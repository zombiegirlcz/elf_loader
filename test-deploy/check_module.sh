#!/system/bin/sh
echo "==== Magisk path ===="
/product/bin/magisk --path
echo "==== Check module files ===="
ls -la /data/adb/modules/parrot_elf_loader/ 2>&1
ls -la /data/adb/modules/parrot_elf_loader/system/lib/ 2>&1