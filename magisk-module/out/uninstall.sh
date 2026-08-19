#!/system/bin/sh
# Parrot ELF Loader - uninstall cleanup.

# restore the /lib interpreter mount we created (best effort)
umount /lib/ld-linux-aarch64.so.1 2>/dev/null

# keep /data/adb/parrot_root so a reinstall keeps the same rootfs config
exit 0