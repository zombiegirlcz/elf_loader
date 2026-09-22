#!/system/bin/sh
# 1) spusti elf_loader+node na REALNEM android rootu (bionic linker), zamrazene
#    na 30s pres PAUSE_CALL, na pozadi.
#
# Pouziti (viz postup.md "PRUELOM - funkcni GDB..."): zkopirovat na device
# (cp do /mnt/app/files/tmp/), pak:
#   ashell -c 'su 0 -c "sh $FILES/tmp/gdb_launch.sh"'   (na pozadi, drzi 40s)
#   ashell -c 'su 0 -c "sh $FILES/tmp/gdb_attach.sh"'   (par sekund pak, attach)
# Vyzaduje `apt-get install gdb` v tomto (Parrot rootfs) prostredi - viz
# [[elf-loader-open-bugs]] pro proc primy chroot-exec elf_loaderu NEfunguje
# (bionic PT_INTERP + APEX mounty) a proc se misto toho pouziva attach-by-PID.
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
N=$R/root/.nvm/versions/node/v26.8.2/bin/node
rm -f $D/tmp/gdb_pid.txt
ELF_LOADER_PAUSE_CALL=0xde9854 $L --ownall $N -e "console.log(42)" > $D/tmp/target_out.txt 2>&1 &
echo $! > $D/tmp/gdb_pid.txt
echo "started pid=$(cat $D/tmp/gdb_pid.txt)"
sleep 40
echo "launch session ending"
