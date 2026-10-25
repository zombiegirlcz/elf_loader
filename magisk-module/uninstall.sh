#!/system/bin/sh
# ELF Loader - uninstall cleanup.
#
# /data/adb/elf_loader_root se ZAMERNE nechava - reinstalace tak zachova
# stejnou konfiguraci rootfs.
rm -f /data/adb/zsh /data/adb/bzsh 2>/dev/null
rm -rf /data/adb/zsh-bionic 2>/dev/null

exit 0