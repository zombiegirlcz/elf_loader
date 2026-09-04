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

print_help() {
    cat <<'EOF'
elroot - PRoot-like launcher for Android (non-root preferred)
Usage:
  elroot --help | -h
  elroot --version | -V
  elroot --check <file>
  elroot --list [bin|lib|python]
  elroot [--ownall|-n] [--chroot|-r] [--rootfs DIR] <prikaz> [args...]
  elroot [--ownall|-n] [--chroot|-r] [--rootfs DIR] -- <prikaz> [args...]
Modes:
  --ownall / -n   force non-root own-loading mode
  --chroot / -r   force root chroot mode (requires su)
  <prikaz>        auto-detect: chroot if root available, else ownall
Options:
  --rootfs DIR    guest rootfs directory
  --help / -h     show this help
  --version / -V  show launcher version
  --check <f>     validate ELF file via loader
  --list <what>   list rootfs contents: bin, lib, python
Environment:
  ROOTFS          guest rootfs path
  ELF_LOADER      loader binary path
  GBSH            gbsh binary path
  SU              su binary path
Examples:
  elroot bash
  elroot --ownall bash
  elroot --chroot bash
  elroot --rootfs /data/.../parrot ls -la /etc
  elroot --check /data/.../parrot/bin/ls
  elroot --list bin | head
EOF
}

print_version() {
    echo "elroot 1.0"
    echo "  loader: elf_loader (bionic NDK build)"
    echo "  shell:  gbsh (native bionic shell)"
    echo "  chroot: linuxsh-root (Magisk su)"
}

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
# prikazy resolovali /usr/bin/ls (ne Android /system/bin/ls, ktery by pod dedicenym
# seccomp filtrem SIGSYSnul a nedostal by SIGSYS handler pres re-exec loaderu).
# /system/bin nechavame jako fallback. Pro --chroot si PATH resi chroot sam.
export PATH="$R/usr/bin:$R/bin:/system/bin:/system/xbin"

MODE=auto
LIST_MODE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ownall|-n) MODE=ownall; shift ;;
    --chroot|-r) MODE=chroot; shift ;;
    --shim|-s)    MODE=shim; shift ;;
    --rootfs)    R="$2"; shift 2 ;;
    --help|-h)   print_help; exit 0 ;;
    --version|-V) print_version; exit 0 ;;
    --check)
      if [ -z "${2:-}" ]; then
        echo "elroot: --check requires a file path" >&2
        exit 1
      fi
      exec "$L" --check "$2"
      ;;
    --list)
      LIST_MODE="${2:-bin}"
      shift 2 || true
      ;;
    --) shift; break ;;
    *) break ;;
  esac
done

if [ -n "$LIST_MODE" ]; then
    case "$LIST_MODE" in
        bin)
            find "$R/bin" "$R/usr/bin" "$R/usr/sbin" "$R/sbin" -maxdepth 1 -type f 2>/dev/null | sort
            ;;
        lib)
            find "$R/lib" "$R/usr/lib" "$R/usr/lib/aarch64-linux-gnu" -maxdepth 1 -type f 2>/dev/null | sort
            ;;
        python)
            find "$R/bin" "$R/usr/bin" -maxdepth 1 -type f \( -name 'python*' -o -name 'pip*' \) 2>/dev/null | sort
            ;;
        *)
            echo "elroot: unknown list mode: $LIST_MODE (use bin|lib|python)" >&2
            exit 1
            ;;
    esac
    exit 0
fi

[ $# -eq 0 ] && { echo "elroot: zadej prikaz, napr. 'elroot bash' nebo 'elroot gcc --version'"; exit 1; }

cmd="$1"; shift
# parrot-relativni cesta binarky (pro chroot mod)
case "$cmd" in
  "$R"/*) PBIN="${cmd#$R}" ;;   # plna device cesta -> strip $R
  /*)      PBIN="$cmd" ;;        # absolutni parrot cesta
  *)       PBIN="/usr/bin/$cmd" ;; # holý nazev
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
