#!/system/bin/sh
# Parrot ELF Loader - post-fs-data.sh (bezi pred Zygote).
#
# POZNAMKA: stary hack (mkdir /lib + bind mount ld-linux-aarch64.so.1) je
# odstranen - na Androidu neni / zapisovatelny, takze /lib/ld-linux-aarch64.so.1
# nelze vytvorit. Transparentni fork/exec glibc binarek resi linuxsh
# (chroot v privatnim mount NS - interp existuje UVNITRI rootfs).
# Bez rootu / mimo linuxsh se binarky spousteji pres `elf` wrapper (elf_loader).
MODDIR=${0%/*}

# nic - vse resi service.sh a linuxsh
exit 0
