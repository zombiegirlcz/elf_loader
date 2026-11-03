#!/data/data/com.termux/files/usr/bin/bash
# termux-setup.sh — nakonfiguruje Termux jako na referenčním zařízení.
#
# Co udělá:
#   1. najde rootfs (env ROOTFS, jinak se zeptá a zapíše do ~/.elfrc)
#   2. nainstaluje zsh (+ volitelně starship/zoxide/fzf do Termuxu)
#   3. najde elf_loader a udělá z něj `proot` (proot -> proot.orig, symlink)
#   4. napíše ~/.elfrc (proměnné + init), ~/.zshrc (jen source .elfrc),
#      ~/.bashrc, ~/.config/lx/zshrc (zsh-only) a ~/.config/starship.toml
#   5. volitelně nakopíruje guest rc do $ROOTFS/root
#   6. ověří, že to funguje
#
# Idempotentní: existující soubory zálohuje s příponou .bak.<timestamp>.
# Spuštění:  bash termux-setup.sh
# Bez otázek: ROOTFS=/cesta bash termux-setup.sh --yes

set -euo pipefail

ASSUME_YES=0
for a in "$@"; do
  case $a in
    -y|--yes) ASSUME_YES=1 ;;
    -h|--help)
      sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "neznámý argument: $a" >&2; exit 2 ;;
  esac
done

# ─── 0. jsme v Termuxu? ─────────────────────────────────────────────────────
if [ -z "${PREFIX:-}" ] || [ ! -d "${PREFIX:-/nonexistent}/bin" ]; then
  echo "CHYBA: tohle nevypadá na Termux (PREFIX není nastavené)." >&2
  echo "       Spusť skript v Termuxu, ne v prootu ani na hostiteli." >&2
  exit 1
fi

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '  \033[1;32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[1;33m!\033[0m %s\n' "$*"; }
die()  { printf '\n\033[1;31mCHYBA:\033[0m %s\n' "$*" >&2; exit 1; }

backup() {
  [ -e "$1" ] || return 0
  local b="$1.bak.$(date +%s)"
  cp -a "$1" "$b" && warn "záloha: $b"
}

ask() {  # ask <prompt> <default>
  local prompt=$1 def=${2:-} ans
  if [ "$ASSUME_YES" = 1 ] && [ -n "$def" ]; then printf '%s' "$def"; return; fi
  printf '%s' "$prompt" >&2
  IFS= read -r ans || true
  [ -n "$ans" ] || ans=$def
  printf '%s' "$ans"
}

# ─── 1. elf_loader ─────────────────────────────────────────────────────────
say "Hledám elf_loader"
ELF=""
for c in "${ELF_LOADER:-}" "$HOME/eltest/elf_loader" "$PREFIX/bin/elf_loader" "$HOME/usr/bin/elf_loader"; do
  if [ -n "$c" ] && [ -x "$c" ]; then ELF=$c; break; fi
done
if [ -z "$ELF" ]; then
  c=$(command -v elf_loader 2>/dev/null || true)
  [ -n "$c" ] && ELF=$c
fi
[ -n "$ELF" ] || die "elf_loader nenalezen. Zkopíruj ho do \$PREFIX/bin/elf_loader a spusť znovu."
ok "loader: $ELF ($("$ELF" --version 2>/dev/null | head -1))"

# ─── 2. ROOTFS ─────────────────────────────────────────────────────────────
say "Zjistuji rootfs"
ROOTFS_VAL="${ROOTFS:-}"

valid_rootfs() { [ -n "${1:-}" ] && [ -d "$1/usr/bin" ] && [ -d "$1/etc" ]; }

# proot-distro >= 4 má containers/<distro>/rootfs, starší installed-rootfs/<distro>
if [ -z "$ROOTFS_VAL" ]; then
  for d in ubuntu debian kali parrot archlinux alpine; do
    for c in "$PREFIX/var/lib/proot-distro/containers/$d/rootfs" \
             "$PREFIX/var/lib/proot-distro/installed-rootfs/$d" \
             "$HOME/nh/distro/$d"; do
      if valid_rootfs "$c"; then ROOTFS_VAL=$c; break 2; fi
    done
  done
fi

if [ -z "$ROOTFS_VAL" ]; then
  warn "Žádný rootfs jsem nenašel (hledal jsem proot-distro i ~/nh/distro)."
  echo "  Nainstaluj ho, např.:  pkg install proot-distro && proot-distro install ubuntu" >&2
  ROOTFS_VAL=$(ask "  Zadej cestu k rootfs (musí obsahovat usr/bin a etc): " "")
fi

while ! valid_rootfs "$ROOTFS_VAL"; do
  warn "'$ROOTFS_VAL' nevypadá jako rootfs (chybí usr/bin nebo etc)."
  if [ "$ASSUME_YES" = 1 ]; then die "--yes zadané, ale ROOTFS je neplatný."; fi
  ROOTFS_VAL=$(ask "  Zadej cestu k rootfs: " "")
done

# kanonická cesta (loader porovnává cesty textově, viz AGENTS.md)
ROOTFS_VAL=$(cd "$ROOTFS_VAL" && pwd -P)
ok "rootfs: $ROOTFS_VAL"
[ -r "$ROOTFS_VAL/etc/os-release" ] && ok "        $(. "$ROOTFS_VAL/etc/os-release" 2>/dev/null; echo "${PRETTY_NAME:-?}")"

# ─── 3. balíčky ────────────────────────────────────────────────────────────
say "Kontroluji balíčky"
need_pkg() {
  local p=$1
  if command -v "$p" >/dev/null 2>&1; then ok "$p"; return; fi
  warn "$p chybí, instaluji"
  # `pkg install` vrací nenulový kód i když je balíček už nainstalovaný,
  # takže rozhoduje až `command -v` níž.
  yes | pkg install -y "$p" >/dev/null 2>&1 || true
  command -v "$p" >/dev/null 2>&1 || die "pkg install $p selhal"
  ok "$p nainstalováno"
}
need_pkg zsh

# ─── 4. proot -> proot.orig, symlink na elf_loader ─────────────────────────
say "Přepínám proot na elf_loader"
PROOT="$PREFIX/bin/proot"
if [ -L "$PROOT" ]; then
  tgt=$(readlink -f "$PROOT" 2>/dev/null || true)
  if [ "$tgt" = "$(readlink -f "$ELF")" ]; then
    ok "proot už je symlink na elf_loader"
  else
    warn "proot je symlink na $tgt, přepisuji"
    rm -f "$PROOT"; ln -s "$ELF" "$PROOT"; ok "proot -> $ELF"
  fi
elif [ -e "$PROOT" ]; then
  if [ -e "$PREFIX/bin/proot.orig" ]; then
    warn "proot.orig už existuje, proot ponechávám (záloha: proot.orig)"
  else
    mv "$PROOT" "$PREFIX/bin/proot.orig"
    ok "proot -> proot.orig"
  fi
  ln -sf "$ELF" "$PROOT"
  ok "proot -> $ELF (symlink)"
else
  ln -sf "$ELF" "$PROOT"
  ok "proot -> $ELF (symlink, proot nebyl)"
fi

# ověření, že symlink funguje.
# POZOR: `proot --version` NEDĚLÁ to, co by člověk čekal — `--version` je pro
# proot-compat neznámá volba, takže loader spadne do fallbacku na SKUTEČNÝ
# proot (= proot.orig). Ten se ale jmenuje jinak, takže to hlásí
# "execve(proot) failed". Testovat se musí skutečný průchod: `-r rootfs cmd`.
if "$PROOT" -r "$ROOTFS_VAL" /usr/bin/true >/dev/null 2>&1; then
  ok "ověřeno: proot -r rootfs /usr/bin/true prošlo loaderem"
else
  warn "proot -r rootfs /usr/bin/true selhalo (zkontroluj ručně)"
fi

# ─── 5. ~/.elfrc — jediný rc (proměnné + zsh nastavení + init) ────────────
# Uživatel: ".zshrc dostane jen source ~/.elfrc, nic víc; zbytek (init
# a proměnné) je v .elfrc". Takže .elfrc je jediný skutečný rc a .zshrc
# i .bashrc jsou jen třířádkové vouchery.
# ─── 5a. ~/.config/lx/zshrc — zsh-only nastavení ──────────────────────────
# Sem patří jen to, co bash neumí naparsovat (setopt, bindkey, zstyle,
# compinit, zsh pluginy). ~/.elfrc ho načte jen když běží zsh.
mkdir -p "$HOME/.config/lx"
backup "$HOME/.config/lx/zshrc"
cat > "$HOME/.config/lx/zshrc" <<'LXEOF'
# ~/.config/lx/zshrc — zsh-only nastavení (načítá ~/.elfrc jen v zsh).

setopt autocd correct interactivecomments magicequalsubst nonomatch notify
setopt numericglobsort promptsubst
WORDCHARS='_-'
PROMPT_EOL_MARK=""
setopt SHARE_HISTORY HIST_IGNORE_ALL_DUPS HIST_IGNORE_SPACE
HISTFILE=~/.zsh_history
HISTSIZE=10000
SAVEHIST=10000

bindkey -e
bindkey ' ' magic-space
bindkey '^U' backward-kill-line
bindkey '^[[3;5~' kill-word
bindkey '^[[3~' delete-char
bindkey '^[[1;5C' forward-word
bindkey '^[[1;5D' backward-word
bindkey '^[[5~' beginning-of-buffer-or-history
bindkey '^[[6~' end-of-buffer-or-history
bindkey '^[[H' beginning-of-line
bindkey '^[[F' end-of-line
bindkey '^[[Z' undo

autoload -Uz compinit
mkdir -p ~/.cache 2>/dev/null
_zcd_old=( ~/.cache/zcompdump(N.mh+24) )
if (( ${#_zcd_old} )); then compinit -d ~/.cache/zcompdump; else compinit -C -d ~/.cache/zcompdump; fi
unset _zcd_old
zmodload -i zsh/complist 2>/dev/null
zstyle ':completion:*' completer _expand _complete
zstyle ':completion:*' format 'Completing %d'
zstyle ':completion:*' group-name ''
zstyle ':completion:*' list-colors ''
zstyle ':completion:*' matcher-list 'm:{a-zA-Z}={A-Za-z}'
zstyle ':completion:*' use-compctl false
zstyle ':completion:*' verbose true
zstyle ':completion:*' menu select

for _f in "${PREFIX:-}/share/zsh/plugins/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh" \
          /usr/share/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh; do
  [ -r "$_f" ] && { . "$_f"; break; }
done
unset _f
if (( ${+ZSH_HIGHLIGHT_STYLES} )); then
  ZSH_HIGHLIGHT_HIGHLIGHTERS=(main brackets)
  ZSH_HIGHLIGHT_MAXLENGTH=500
  ZSH_HIGHLIGHT_STYLES[path]=fg=blue,bold
  ZSH_HIGHLIGHT_STYLES[unknown-token]=underline
fi
for _f in "${PREFIX:-}/share/zsh/plugins/zsh-autosuggestions/zsh-autosuggestions.zsh" \
          /usr/share/zsh-autosuggestions/zsh-autosuggestions.zsh; do
  [ -r "$_f" ] && { . "$_f"; break; }
done
unset _f
[ -f /etc/zsh_command_not_found ] && . /etc/zsh_command_not_found
LXEOF
chmod 600 "$HOME/.config/lx/zshrc"
zsh -n "$HOME/.config/lx/zshrc" && ok "$HOME/.config/lx/zshrc (syntaxe OK)"

say "Píšu ~/.elfrc"
backup "$HOME/.elfrc"

DISTRO_NAME=$(basename "$(dirname "$ROOTFS_VAL")")
[ "$DISTRO_NAME" = "proot-distro" ] && DISTRO_NAME=ubuntu

# Quoted heredoc = žádná expanze při běhu setupu. Zástupné symboly
# @ROOTFS@/@DISTRO@/@LOADER@ nahradíme až po zápisu (sed).
cat > "$HOME/.elfrc" <<'LXEOF'
# ~/.elfrc — jediný rc: proměnné pro loader, zsh nastavení a init.
# Načítá ho ~/.zshrc a ~/.bashrc (jen `source`). `init` na konci svého
# výstupu sourcuje tento soubor ZNOVU — proto je hned na začátku guard.

# ── rekurzní guard ────────────────────────────────────────────────────────
[ -n "${_LX_ELF_INIT:-}" ] && return 0 2>/dev/null
_LX_ELF_INIT=1

# ── proměnné pro loader ───────────────────────────────────────────────────
# `init` čte $L (ne $ELF_LOADER) a $D (app files dir). Když je nenajde,
# zkusí fallback $D/usr/bin/elf_loader a $L skončí na neexistující cestě.
export ROOTFS="@ROOTFS@"
export LX_DISTRO="@DISTRO@"
export ELF_LOADER="@LOADER@"
export L="@LOADER@"
export D="$HOME"
if [ -n "${ZSH_VERSION:-}" ]; then _LX_SHELL=zsh; else _LX_SHELL=bash; fi

# ── zsh vrstva ────────────────────────────────────────────────────────────
# Bash neumí parsovat zsh syntaxi ani uvnitř `if`, proto je zsh nastavení
# v samostatném souboru, který se v bash vůbec nenačte.
[ -n "${ZSH_VERSION:-}" ] && [ -r "$HOME/.config/lx/zshrc" ] \
  && . "$HOME/.config/lx/zshrc"

# ── zbytek nastaví loader (R, LX_PATH, lx/lxwhich, …) ─────────────────────
# AŽ PO exportu ROOTFS výše — jinak by init autodetekoval rootfs podle
# LX_DISTRO (default parrot) a `lx` by hlásil "not found in guest rootfs".
# Guard nahoře zajistí, že vnořené sourcování z initu se hned vrátí.
if [ -x "${ELF_LOADER:-/nonexistent}" ]; then
  eval "$("$ELF_LOADER" init "$_LX_SHELL" 2>/dev/null)" 2>/dev/null
fi

# ── nástroje z rootfs (volitelné) ────────────────────────────────────────
# starship/zoxide/fzf se berou Z ROOTFS (guest glibc pod loaderem), ne
# z Termuxu. Proto lxwhich (hledá v $ROOTFS) a lx (spustí pod loaderem).
# Když v rootfs nejsou, blok se přeskočí. Zápis je POSIX, aby ho zvládl
# i bash (žádné typeset -g ani process substitution).
command -v lxwhich >/dev/null 2>&1 || return 0 2>/dev/null

if lxwhich zoxide >/dev/null 2>&1; then
  zoxide() { lx zoxide "$@"; }
  _ZO_DATA_DIR="${_ZO_DATA_DIR:-$HOME/.local/share/zoxide}"
  export _ZO_DATA_DIR
  _zocache=$HOME/.cache/zoxide-init.$_LX_SHELL
  if [ ! -s "$_zocache" ] || [ "$_zocache" -ot "$(lxwhich zoxide)" ]; then
    lx zoxide init "$_LX_SHELL" --cmd z 2>/dev/null | sed 's/command zoxide/zoxide/g' > "$_zocache"
  fi
  [ -r "$_zocache" ] && . "$_zocache"
  unset _zocache
fi

if [ "$_LX_SHELL" = zsh ] && lxwhich fzf >/dev/null 2>&1; then
  fzf() { lx fzf "$@"; }
  _fz=$HOME/.cache/fzf-init.$_LX_SHELL
  if [ ! -s "$_fz" ] || [ "$_fz" -ot "$(lxwhich fzf)" ]; then
    lx fzf --zsh 2>/dev/null | sed -e "s|$R/usr/bin/fzf|fzf|g" -e "s|$L |fzf |g" > "$_fz"
  fi
  [ -r "$_fz" ] && . "$_fz"
  unset _fz
fi

# starship jen v zsh: v neinteraktivnim bash je prompt zbytecny a cache by
# obsahovala absolutni guest cestu, kterou bionic bash nespusti.
if [ "$_LX_SHELL" = zsh ] && lxwhich starship >/dev/null 2>&1; then
  STARSHIP_CONFIG="${STARSHIP_CONFIG:-$HOME/.config/starship.toml}"
  STARSHIP_CACHE="${STARSHIP_CACHE:-$HOME/.cache/starship}"
  export STARSHIP_CONFIG STARSHIP_CACHE
  starship() { lx starship "$@"; }
  _sscache=$HOME/.cache/starship-init.$_LX_SHELL
  if [ ! -s "$_sscache" ] || [ "$_sscache" -ot "$(lxwhich starship)" ]; then
    lx starship init "$_LX_SHELL" 2>/dev/null \
      | sed -e "s|$R/usr/bin/starship|starship|g" -e "s|$L|starship|g" > "$_sscache"
  fi
  [ -r "$_sscache" ] && . "$_sscache"
  unset _sscache
  _LX_STARSHIP_DONE=1
  export _LX_STARSHIP_DONE
fi

# ── sem patří tvoje vlastní nastavení (aliasy, …) ──────────────────────────
LXEOF

# doplň cesty (quoted heredoc je neexpandoval)
python3 - "$HOME/.elfrc" "$ROOTFS_VAL" "${DISTRO_NAME:-ubuntu}" "$ELF" <<'PYSUBS'
import sys
path, rootfs, distro, loader = sys.argv[1:5]
t = open(path, encoding='utf-8').read()
t = t.replace('@ROOTFS@', rootfs).replace('@DISTRO@', distro).replace('@LOADER@', loader)
open(path, 'w', encoding='utf-8').write(t)
PYSUBS

chmod 600 "$HOME/.elfrc"
if ( unset LD_PRELOAD; zsh -n "$HOME/.elfrc" ) 2>/dev/null; then
  ok "$HOME/.elfrc ($(wc -l < "$HOME/.elfrc") řádků, syntaxe OK)"
else
  warn "$HOME/.elfrc se nepodařilo ověřit"
fi

# ─── 6. ~/.zshrc — jen `source ~/.elfrc` ──────────────────────────────────
# Uživatel: "zshrc v setupu dostane jen source $HOME/.elfrc, nic víc —
# zbytek (init a proměnné) jsou v elfrc". Přesně tak.
say "Píšu ~/.zshrc"
backup "$HOME/.zshrc"
cat > "$HOME/.zshrc" <<'LXEOF'
# ~/.zshrc — veškerá konfigurace je v ~/.elfrc.
# ELF_RC=<cesta> přepíše, LX_NO_RC=1 vypne.
[ -z "${LX_NO_RC:-}" ] && [ -r "${ELF_RC:-$HOME/.elfrc}" ] && . "${ELF_RC:-$HOME/.elfrc}"
LXEOF
chmod 600 "$HOME/.zshrc"
zsh -n "$HOME/.zshrc" && ok "$HOME/.zshrc (jen source .elfrc)"

# ─── 7. ~/.bashrc ─────────────────────────────────────────────────────────
# Interaktivní bash předá zsh (ten si načte .elfrc sám), neinteraktivní
# zůstává bash a načte .elfrc přímo.
say "Píšu ~/.bashrc"
backup "$HOME/.bashrc"
cat > "$HOME/.bashrc" <<'LXEOF'
# ~/.bashrc — interaktivní shell předá zsh; konfigurace je v ~/.elfrc.
case $- in
  *i*)
    if [ -x "$PREFIX/bin/zsh" ] && [ -z "${LX_NO_ZSH:-}" ]; then
      exec zsh -l
    fi
    ;;
esac
[ -z "${LX_NO_RC:-}" ] && [ -r "${ELF_RC:-$HOME/.elfrc}" ] && . "${ELF_RC:-$HOME/.elfrc}"
PS1='\w \$ '
LXEOF
chmod 600 "$HOME/.bashrc"
bash -n "$HOME/.bashrc" && ok "$HOME/.bashrc (syntaxe OK)"

# ─── 8. starship.toml ─────────────────────────────────────────────────────
say "Píšu ~/.config/starship.toml"
mkdir -p "$HOME/.config"
backup "$HOME/.config/starship.toml"
cat > "$HOME/.config/starship.toml" <<'LXEOF'
command_timeout = 1000
LXEOF
chmod 600 "$HOME/.config/starship.toml"
ok "$HOME/.config/starship.toml"

# ─── 9. guest rc do rootfs (volitelně) ────────────────────────────────────
# Guest dostane stejnou zsh vrstvu a STEJNÝ .elfrc (jen proměnné). V guestu
# `init` nespouštíme — už jsme pod loaderem, re-exec by jen zacyklil.
say "Guest rc v rootfs"
GH=${LX_GUEST_HOME:-/root}
if [ -d "$ROOTFS_VAL$GH" ]; then
  if [ "$ASSUME_YES" = 0 ]; then
    ans=$(ask "  Nakopírovat rc do $ROOTFS_VAL$GH? [A/n] " "A")
  else
    ans=A
  fi
  case "$ans" in
    [Aa]*)
      backup "$ROOTFS_VAL$GH/.zshrc"
      backup "$ROOTFS_VAL$GH/.elfrc"
      cp -f "$HOME/.zshrc" "$ROOTFS_VAL$GH/.zshrc"
      cp -f "$HOME/.elfrc" "$ROOTFS_VAL$GH/.elfrc"
      chmod 600 "$ROOTFS_VAL$GH/.zshrc" "$ROOTFS_VAL$GH/.elfrc"
      zsh -n "$ROOTFS_VAL$GH/.zshrc" && ok "guest .zshrc (syntaxe OK)"
      ( unset LD_PRELOAD; zsh -n "$ROOTFS_VAL$GH/.elfrc" ) 2>/dev/null \
        && ok "guest .elfrc (syntaxe OK)"
      ;;
    *) warn "guest rc přeskočen" ;;
  esac
else
  warn "$ROOTFS_VAL$GH neexistuje, guest rc přeskočen"
fi

# ─── 10. ověření ────────────────────────────────────────────────────────
say "Ověřuji"
if ( unset LD_PRELOAD; zsh -i -c 'true' ) >/dev/null 2>&1; then ok "zsh -i se načte bez chyby"; else warn "zsh -i hlásí problém"; fi
if ( unset LD_PRELOAD; zsh -i -c 'lxwhich starship' ) 2>/dev/null | grep -q .; then
  ok "starship z rootfs: $( ( unset LD_PRELOAD; zsh -i -c 'lxwhich starship' ) 2>/dev/null )"
else
  warn "starship v rootfs není — prompt pojede bez něj (apt install starship)"
fi
if ( unset LD_PRELOAD; zsh -i -c 'lx /bin/true' ) >/dev/null 2>&1; then ok "lx /bin/true funguje"; else warn "lx /bin/true selhal"; fi
if "$PROOT" -r "$ROOTFS_VAL" /usr/bin/true >/dev/null 2>&1; then ok "proot (= elf_loader) prochazi rootfs"; fi

cat <<EOF

\033[1;32mHotovo.\033[0m Spusť nový shell:

  exec zsh -l

nebo jen zavři a otevři Termux. Prompty/starship se berou z rootfs:
  $ROOTFS_VAL
Zálohy starých souborů mají příponu .bak.<timestamp>.
EOF
