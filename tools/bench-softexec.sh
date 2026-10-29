#!/system/bin/sh
# Prompt 11 - A/B mereni soft execu (ELF_LOADER_SOFTEXEC=1).
#
# Spoustet z HOSTA: ashell -c "sh $R/root/elf_loader/tools/bench-softexec.sh"
# (pod loaderem by se merilo zanoreno - viz past z promptu 01).
#
# Promenne: N = pocet execu v smycce (default 500), CHECKS = pocet "checku"
# v configure-like zatezi (default 80, 5 execu na check), ROUNDS = kola.
#
# Poradi rezimu se mezi koly obraci: CPU governor na tomhle zarizeni meni
# absolutni cisla o desitky procent, takze plati jen A/B v ramci jednoho kola.
R=${ROOTFS:-/data/user/0/com.linux_core/files/nh/distro/parrot}
L=${L:-/data/user/0/com.linux_core/files/usr/bin/elf_loader}
P=${P:-/data/user/0/com.linux_core/files/usr/bin/proot}
N=${N:-500}
CHECKS=${CHECKS:-80}
ROUNDS=${ROUNDS:-2}
WORK=$R/tmp/bench-softexec.$$
mkdir -p "$WORK" || exit 1

cat > "$WORK/fakeconf.sh" <<'CONF'
# configure-like zatez: 5 execu na "check"
N=${CHECKS:-80}
i=0
while [ $i -lt $N ]; do
  printf 'int main(void){return 0;}\n' > conftest.c
  /bin/cat conftest.c > /dev/null
  /bin/grep -q main conftest.c
  /bin/sed -e 's/main/MAIN/' conftest.c > conftest.out
  /usr/bin/expr $i + 1 > /dev/null
  /bin/rm -f conftest.c conftest.out
  i=$((i+1))
done
CONF

LOOP="i=0; while [ \$i -lt $N ]; do /bin/true; i=\$((i+1)); done"
FORK="i=0; while [ \$i -lt $N ]; do ( : ) ; i=\$((i+1)); done"
PR_ENV="PROOT_LOADER=${PROOT_LOADER:-/data/user/0/com.linux_core/files/usr/bin/loader} PROOT_TMP_DIR=$R/tmp"

k=1
while [ $k -le $ROUNDS ]; do
    echo "######## kolo $k (N=$N, CHECKS=$CHECKS) ########"
    for mode in softexec default proot; do
        case $mode in
          softexec) pre="ROOTFS=$R ELF_LOADER_SOFTEXEC=1 $L --ownall $R/bin/sh" ;;
          default)  pre="ROOTFS=$R $L --ownall $R/bin/sh" ;;
          proot)    pre="env $PR_ENV $P -r $R -w / -b /dev -b /proc /bin/sh" ;;
        esac
        echo "-- $mode: ${N}x /bin/true --"
        time env $pre -c "$LOOP"
        echo "-- $mode: ${N}x fork bez execu --"
        time env $pre -c "$FORK"
        echo "-- $mode: fakeconf ($((CHECKS*5)) execu) --"
        cd "$WORK"
        case $mode in
          proot) time env CHECKS=$CHECKS $PR_ENV $P -r $R -w "${WORK#$R}" -b /dev -b /proc \
                      /bin/sh "${WORK#$R}/fakeconf.sh" ;;
          *)     time env CHECKS=$CHECKS $pre "${WORK#$R}/fakeconf.sh" ;;
        esac
    done
    k=$((k+1))
done
rm -rf "$WORK"
