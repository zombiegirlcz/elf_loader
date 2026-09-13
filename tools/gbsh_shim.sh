#!/system/bin/sh
# gbsh_shim — launch a guest shell (zsh/bash) under elf_loader with the
# exec_shim LD_PRELOAD whitelist shim active.
#
# The shim (exec_shim.so) is a GLIBC shared library, so it must NOT be placed
# into LD_PRELOAD directly: the bionic linker64 that starts elf_loader would
# abort with "CANNOT LINK EXECUTABLE ... libc.so.6 not found". Instead we set
# ELF_LOADER_PRELOAD, which elf_loader turns into LD_PRELOAD only for the guest
# glibc ld.so (see elf_guest_envp in src/main.c).
#
# Usage:
#   gbsh_shim [--ownall|-n | --shim|-s] [shell-path-or-name] [args...]
#
# Environment:
#   ROOTFS           guest rootfs (required)
#   ELF_LOADER       elf_loader binary (auto-detected if unset)
#   SHIM_SO          exec_shim.so path (auto-detected if unset)
#   SHIM_WHITELIST   whitelist file path (auto-detected if unset)

R="${ROOTFS:-}"
[ -z "$R" ] && { echo "gbsh_shim: ROOTFS není nastaven" >&2; exit 1; }

# --- elf_loader location -----------------------------------------------------
L="${ELF_LOADER:-}"
if [ -z "$L" ]; then
  for c in "$R/../usr/bin/elf_loader" "$R/../../../usr/bin/elf_loader" "/system/bin/elf_loader"; do
    if [ -x "$c" ]; then L="$c"; break; fi
  done
fi
[ -x "$L" ] || { echo "gbsh_shim: elf_loader nenalezen" >&2; exit 1; }

# --- shim .so ----------------------------------------------------------------
S="${SHIM_SO:-}"
if [ -z "$S" ]; then
  for c in "$R/../../../usr/lib/exec_shim.so" "$R/../usr/lib/exec_shim.so" \
           "$L/../lib/exec_shim.so" "/system/lib64/exec_shim.so"; do
    if [ -f "$c" ]; then S="$c"; break; fi
  done
fi
[ -f "$S" ] || { echo "gbsh_shim: exec_shim.so nenalezen" >&2; exit 1; }

# --- whitelist ----------------------------------------------------------------
W="${SHIM_WHITELIST:-}"
if [ -z "$W" ]; then
  for c in "$S.whitelist" "$R/../../../usr/lib/exec_shim.whitelist" \
           "$R/../usr/lib/exec_shim.whitelist" "$R/usr/lib/exec_shim.whitelist"; do
    if [ -f "$c" ]; then W="$c"; break; fi
  done
fi

# --- mode / shell -------------------------------------------------------------
MODE=--ownall
case "${1:-}" in
  --shim|-s)   MODE=--shim;   shift ;;
  --ownall|-n) MODE=--ownall; shift ;;
esac

SHELL_BIN="${1:-}"
if [ -z "$SHELL_BIN" ]; then
  for c in "$R/usr/bin/zsh" "$R/bin/zsh" "$R/usr/bin/bash" "$R/bin/bash"; do
    if [ -x "$c" ]; then SHELL_BIN="$c"; break; fi
  done
fi
[ -x "$SHELL_BIN" ] || { echo "gbsh_shim: žádný guest shell (zsh/bash) nenalezen" >&2; exit 1; }
shift 2>/dev/null || true

export ROOTFS="$R"
export ELF_LOADER="$L"
export ELF_LOADER_PRELOAD="$S"      # guest-only preload (bionic-safe)
export SHIM_WHITELIST="$W"
export SHIM_EXEC_MODE="$MODE"
[ -d "$R/usr/share/terminfo" ] && export TERMINFO="$R/usr/share/terminfo"

exec "$L" "$MODE" "$SHELL_BIN" "$@"
