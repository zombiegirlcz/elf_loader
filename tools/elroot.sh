#!/system/bin/sh
# elroot - PRoot-like launcher pro parrot rootfs (vyuziva elf_loader + gbsh).
# Spusti prikaz z parrot rootfs:
#   - preferovane pres ROOT chroot (plny parrot fs + zadny seccomp) -> vse funguje
#   - fallback na elf_loader --ownall (non-root, 409/412 binarek v provozu)
# Pouziti: elroot [--ownall|-n] [--chroot|-r] [--rootfs DIR] <prikaz> [args...]
F=/data/user/0/com.linux_core/files
R=${ROOTFS:-$F/nh/distro/parrot}
L=$F/usr/bin/elf_loader
G=$F/usr/bin/gbsh
SU=/product/bin/su

# cista cesta pro interni prikazy (ashell ma v PATH parrot cesty)
export PATH=/system/bin:/system/xbin

MODE=auto
while [ $# -gt 0 ]; do
  case "$1" in
    --ownall|-n) MODE=ownall; shift ;;
    --chroot|-r) MODE=chroot; shift ;;
    --rootfs)    R="$2"; shift 2 ;;
    --help|-h)   echo "elroot [--ownall|--chroot] [--rootfs DIR] <prikaz> [args]"; exit 0 ;;
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

# detekce rootu (magisk su)
ROOT_OK=0
if [ "$MODE" != "ownall" ] && [ -x "$SU" ]; then
  if "$SU" -c '/system/bin/id -u' 2>/dev/null | /system/bin/grep -q '^0$'; then ROOT_OK=1; fi
fi

if [ "$MODE" = "ownall" ]; then
  :
elif [ "$MODE" = "chroot" ] && [ "$ROOT_OK" -eq 0 ]; then
  echo "elroot: --chroot vyžaduje root (su není dostupný)"; exit 1
elif [ "$ROOT_OK" -eq 1 ]; then
  MODE=chroot
else
  MODE=ownall
fi

if [ "$MODE" = "chroot" ]; then
  ARGS="$@"
  exec "$SU" -c "ROOTFS=$R '$G' --chroot -c '$PBIN $ARGS'"
else
  TERMINFO="$R/usr/share/terminfo" exec "$L" --ownall "$BIN" "$@"
fi
