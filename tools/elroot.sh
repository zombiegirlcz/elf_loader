#!/system/bin/sh
# elroot - PRoot-like launcher pro libovolny glibc rootfs (vyuziva elf_loader + gbsh).
# Spusti prikaz z rootfs:
#   - preferovane pres ROOT chroot (plny fs + zadny seccomp) -> vse funguje
#   - fallback na elf_loader --ownall (non-root)
# Vse se bere z ENV (univerzalni, bez hardcoded cest k aplikaci):
#   ROOTFS     cesta k distro rootfs (napr. /data/.../nh/distro/parrot)
#   ELF_LOADER cesta k elf_loader binarce (vychozi: $ROOTFS/../usr/bin/elf_loader, pak /system/bin/elf_loader)
#   GBSH       cesta k gbsh (vychozi: $ROOTFS/../usr/bin/gbsh, pak /system/bin/gbsh)
#   SU         cesta k su (vychozi /product/bin/su)
# Pouziti: elroot [--ownall|-n] [--chroot|-r] [--rootfs DIR] <prikaz> [args...]
R="${ROOTFS:-}"
if [ -z "$R" ]; then
  echo "elroot: ROOTFS není nastaven — export ROOTFS=/cesta/k/distro (nebo --rootfs DIR)"; exit 1
fi
L="${ELF_LOADER:-}"
# Hledame loader v nekolika kandidatch (zavisi na deployment layoutu):
#  - $R/../usr/bin/elf_loader      ... rootfs a usr jsou siblingove (puvodni predpoklad)
#  - $R/../../../usr/bin/elf_loader ... files/ layout: rootfs v nh/distro/parrot, loader v usr/bin
#  - $R/usr/bin/elf_loader         ... loader primo uvnitr rootfs
#  - /system/bin/elf_loader        ... Magisk modul fallback
if [ -z "$L" ]; then
  for cand in "$R/../usr/bin/elf_loader" "$R/../../../usr/bin/elf_loader" "$R/usr/bin/elf_loader" "/system/bin/elf_loader"; do
    if [ -x "$cand" ]; then L="$cand"; break; fi
  done
fi
[ -x "$L" ] || L=/system/bin/elf_loader
G="${GBSH:-}"
if [ -z "$G" ]; then
  for cand in "$R/../usr/bin/gbsh" "$R/../../../usr/bin/gbsh" "$R/usr/bin/gbsh" "/system/bin/gbsh"; do
    if [ -x "$cand" ]; then G="$cand"; break; fi
  done
fi
[ -x "$G" ] || G=/system/bin/gbsh
SU="${SU:-/product/bin/su}"

# cesta: pro shim/ownall preferujeme ROOTFS bin (parrot binarky), aby guest
# prikazy resil /usr/bin/ls (ne Android /system/bin/ls, ktery by pod dedicenym
# seccomp filtrem SIGSYSnul a nedostal by SIGSYS handler pres re-exec loaderu).
# /system/bin nechavame jako fallback. Pro --chroot si PATH resi chroot sam.
export PATH="$R/usr/bin:$R/bin:/system/bin:/system/xbin"

MODE=auto
while [ $# -gt 0 ]; do
  case "$1" in
    --ownall|-n) MODE=ownall; shift ;;
    --chroot|-r) MODE=chroot; shift ;;
    --shim|-s)    MODE=shim; shift ;;
    --rootfs)    R="$2"; shift 2 ;;
    --help|-h)   echo "elroot [--ownall|-n|--chroot|-r|--shim|-s] [--rootfs DIR] <prikaz> [args]"; exit 0 ;;
    *) break ;;
  esac
done

[ $# -eq 0 ] && { echo "elroot: zadej prikaz, napr. 'elroot bash' nebo 'elroot gcc --version'"; exit 1; }

cmd="$1"; shift
# parrot-relativni cesta binarky (pro chroot mod)
case "$cmd" in
  "$R"/*) PBIN="${cmd#$R}" ;;   # plna device cesta -> strip $R
  /*)      PBIN="$cmd" ;;        # absolutni parrot cesta
  *)       PBIN="/usr/bin/$cmd" ;; # holy nazev
esac
BIN="$R$PBIN"                       # device cesta (pro loader mod)

# detekce rootu (magisk su) - JEN pro auto/chroot rozhodnuti. --shim i --ownall
# bezi non-root a su NEpotrebuji; kdyby se detekce spoustela i pro ne, kazdy
# 'elroot --shim' by vyvolal root 'su -c /system/bin/id -u' (notifikace su/root).
ROOT_OK=0
if { [ "$MODE" = "auto" ] || [ "$MODE" = "chroot" ]; } && [ -x "$SU" ]; then
  if "$SU" -c '/system/bin/id -u' 2>/dev/null | /system/bin/grep -q '^0$'; then ROOT_OK=1; fi
fi

if [ "$MODE" = "chroot" ] && [ "$ROOT_OK" -eq 0 ]; then
  echo "elroot: --chroot vyžaduje root (su není dostupný)"; exit 1
fi
# ownall/shim zustavaji jak jsou; jinak auto -> chroot (root) / ownall
if [ "$MODE" != "ownall" ] && [ "$MODE" != "shim" ]; then
  if [ "$ROOT_OK" -eq 1 ]; then MODE=chroot; else MODE=ownall; fi
fi

if [ "$MODE" = "chroot" ]; then
  ARGS="$@"
  exec "$SU" -c "ROOTFS=$R '$G' --chroot -c '$PBIN $ARGS'"
elif [ "$MODE" = "shim" ]; then
  # F2 seccomp path-filter se ZDE vypina: pri --shim re-execu dedi dite
  # (napr. bash spousti kazdy prikaz pres execve) zdedy filtr, ale SIGSYS
  # handler je po execve SIG_DFL -> dite umre na SIGSYS ("badsyscall", RC=159).
  # Preklad cest v --shim rezimu zajistuji PLT override + inline hooky +
  # explicitni reseni symlinku v elf_load (bez F2 filtru). F2_FILTER=1 lze
  # pouzit jen pro jednoprocesove spusteni mimo elroot --shim (prime volani
  # elf_loader --shim bez re-execu potomku).
  unset F2_FILTER
  ROOTFS="$R" TERMINFO="$R/usr/share/terminfo" ELF_LOADER="$L" exec "$L" --shim "$BIN" "$@"
else
  TERMINFO="$R/usr/share/terminfo" ELF_LOADER="$L" exec "$L" --ownall "$BIN" "$@"
fi
