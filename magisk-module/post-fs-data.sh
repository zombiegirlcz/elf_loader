#!/system/bin/sh
# ELF Loader - post-fs-data.sh (bezi pred Zygote).
#
# Zamerne prazdne. Zadny bind mount / chroot se nedela - elf_loader preklada
# cesty sam, takze neni potreba nic lepit do /system ani do /lib.
MODDIR=${0%/*}

exit 0