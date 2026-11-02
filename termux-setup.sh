#!/data/data/com.termux/files/usr/bin/bash
# termux-setup.sh — nakonfiguruje Termux jako na referenčním zařízení.
#
# Co udělá:
#   1. najde rootfs (env ROOTFS, jinak se zeptá a zapíše do ~/.elfrc)
#   2. nainstaluje zsh (+ volitelně starship/zoxide/fzf do Termuxu)
#   3. najde elf_loader a udělá z něj `proot` (proot -> proot.orig, symlink)
#   4. napíše ~/.zshrc, ~/.bashrc, ~/.elfrc, ~/.config/starship.toml
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

# ─── 5. ~/.elfrc ───────────────────────────────────────────────────────────
# ~/.elfrc je JEDINÁ uživatelská vrstva. Obsahuje všechno, co má platit
# v obou světech (Termux bionic i guest glibc pod elf_loaderem): loader,
# ROOTFS, PATH, lx/lxwhich, aliasy, helpery, starship/zoxide/fzf.
# ~/.zshrc obsahuje jen zsh-specifické věci a na konci .elfrc sourcuje.
say "Píšu ~/.elfrc"
backup "$HOME/.elfrc"
cat > "$HOME/.elfrc" <<'LXEOF'
# ~/.elfrc — uživatelská konfigurace, společná pro zsh i bash.
# Načítá ho `elf_loader init zsh|bash` (na konci svého výstupu) a v guestu
# ~/.zshrc / ~/.bashrc přímo. ELF_RC=... přepíše, LX_NO_RC=1 vypne.
# NEPATŘÍ sem `init` — init .elfrc sám sourcuje (byla by to rekurze).
#
# PRAVIDLO: starship a zoxide se berou Z ROOTFS (guest glibc binárky pod
# elf_loader), ne z Termuxu. Proto `lxwhich` (najde cestu v rootfs) a `lx`
# (spustí ji pod loaderem) — nikdy `command -v`.

# .elfrc se načítá DVĚMA způsoby:
#   1. `elf_loader init zsh|bash` ho sourcuje na konci svého výstupu (host)
#   2. guest shell (pod loaderem) ho sourcuje z ~/.zshrc přímo
# V obou případech už jsou ROOTFS/R/L/LX_PATH nastavené — tady se jen
# dopočítá guest varianta lx/lxwhich, protože bionický loader v guestu
# nespustíme.
if [ -z "${PREFIX:-}" ] && ! command -v lxwhich >/dev/null 2>&1 && [ -n "${L:-}" ]; then
  lxwhich() {
    _c=$1
    case $_c in
      "$ROOTFS"/*) [ -e "$_c" ] && { printf '%s\n' "$_c"; return 0; }; return 1 ;;
      /*) [ -e "$ROOTFS$_c" ] && { printf '%s\n' "$ROOTFS$_c"; return 0; }; return 1 ;;
    esac
    for _d in "$ROOTFS/usr/local/sbin" "$ROOTFS/usr/local/bin" \
              "$ROOTFS/usr/sbin" "$ROOTFS/usr/bin" \
              "$ROOTFS/sbin" "$ROOTFS/bin"; do
      [ -x "$_d/$_c" ] && [ ! -d "$_d/$_c" ] && { printf '%s\n' "$_d/$_c"; return 0; }
    done
    return 1
  }
  lx() { _c=$1; shift; _p=$(lxwhich "$_c") || _p=$_c; "$L" --ownall "$_p" "$@"; }
fi

# --- který shell? (volby --cmd/init generují zsh nebo bash variantu) ---
# Musí být tady: `init` sice exportuje spoustu věcí, ale $_LX_SHELL ne.
# Bez toho se cache ukládá jako "starship-init." (prázdná přípona) a nenačte se.
if [ -n "${ZSH_VERSION:-}" ]; then _LX_SHELL=zsh; else _LX_SHELL=bash; fi

# --- po přepsání ROOTFS znovu odvodit R a LX_PATH ---
# `init` autodetekuje rootfs (default LX_DISTRO=parrot) a spočítá R/LX_PATH
# z něj. Až POTOM sourcuje .elfrc, kde je správný ROOTFS. Bez přepočtu by
# R ukazoval na neexistující rootfs a `lx` by hlásil "not found".
if [ -n "${ROOTFS:-}" ] && [ "${R:-}" != "$ROOTFS" ]; then
  R=$ROOTFS
  LX_PATH="$R/usr/local/sbin $R/usr/local/bin $R/usr/sbin $R/usr/bin $R/sbin $R/bin"
  for _nvm in "$R${LX_GUEST_HOME:-/root}/.nvm/versions/node"/*/bin; do
    [ -d "$_nvm" ] && LX_PATH="$LX_PATH $_nvm"
  done
  unset _nvm
  [ -d "$R${LX_GUEST_HOME:-/root}/.local/bin" ] && LX_PATH="$LX_PATH $R${LX_GUEST_HOME:-/root}/.local/bin"
fi
export ROOTFS R LX_PATH

# --- aliasy a helpery (platí v obou světech) ---
if [ -t 1 ]; then
  alias grep='grep --color=auto'
  alias fgrep='fgrep --color=auto'
  alias egrep='egrep --color=auto'
  alias diff='diff --color=auto'
  alias ip='ip --color=auto'
fi
mkcd() { mkdir -p "$1" && cd "$1"; }
fnd() {
  if command -v rg >/dev/null 2>&1; then rg --files | rg "$1"
  else find . -type f -name "*$1*" 2>/dev/null; fi
}

# Bez lx/lxwhich (chybí loader) nemá zbytek smysl.
command -v lxwhich >/dev/null 2>&1 || return 0 2>/dev/null || exit 0

# --- zoxide (z = cd s pamětí; data v hostím HOME) ---
if lxwhich zoxide >/dev/null 2>&1; then
  zoxide() { lx zoxide "$@" 2> >(grep -v '^\[MMAP\]' >&2); }
  export _ZO_DATA_DIR="${_ZO_DATA_DIR:-$HOME/.local/share/zoxide}"
  _zocache=$HOME/.cache/zoxide-init.$_LX_SHELL
  if [ ! -s "$_zocache" ] || [ "$_zocache" -ot "$(lxwhich zoxide)" ]; then
    lx zoxide init "$_LX_SHELL" --cmd z | sed 's/\bcommand zoxide\b/zoxide/g' > "$_zocache"
  fi
  [ -r "$_zocache" ] && . "$_zocache"
  unset _zocache
fi

# --- fzf (klávesové zkratky; binárka z rootfs) ---
if lxwhich fzf >/dev/null 2>&1; then
  fzf() { lx fzf "$@" 2> >(grep -v '^\[MMAP\]' >&2); }
  _fz=$HOME/.cache/fzf-init.$_LX_SHELL
  if [ ! -s "$_fz" ] || [ "$_fz" -ot "$(lxwhich fzf)" ]; then
    if [ "$_LX_SHELL" = zsh ]; then
      lx fzf --zsh 2>/dev/null | sed -e "s|$R/usr/bin/fzf|fzf|g" -e "s|$L |fzf |g" > "$_fz"
    else
      lx fzf --bash 2>/dev/null | sed -e "s|$R/usr/bin/fzf|fzf|g" -e "s|$L |fzf |g" > "$_fz"
    fi
  fi
  [ -r "$_fz" ] && . "$_fz"
  unset _fz
fi

# --- starship (POSLEDNÍ: přepisuje PS1/PROMPT) ---
if lxwhich starship >/dev/null 2>&1; then
  export STARSHIP_CONFIG="${STARSHIP_CONFIG:-$HOME/.config/starship.toml}"
  export STARSHIP_CACHE="${STARSHIP_CACHE:-$HOME/.cache/starship}"
  starship() {
    if [ "$1" = prompt ]; then
      _ssnow=$(date +%s)
      if [ -n "${_STARSHIP_PROMPT_CACHE:-}" ] && [ $((_ssnow - ${_STARSHIP_PROMPT_TIME:-0})) -lt 2 ]; then
        printf '%s' "$_STARSHIP_PROMPT_CACHE"
        return 0
      fi
      _ssout=$(lx starship "$@" 2> >(grep -v '^\[MMAP\]' >&2))
      if [ -n "$_ssout" ]; then
        if [ -n "${ZSH_VERSION:-}" ]; then
          typeset -g _STARSHIP_PROMPT_CACHE="$_ssout" _STARSHIP_PROMPT_TIME=$_ssnow
        else
          declare -g _STARSHIP_PROMPT_CACHE="$_ssout" _STARSHIP_PROMPT_TIME=$_ssnow
        fi
      fi
      printf '%s' "$_ssout"
      return 0
    fi
    lx starship "$@"
  }
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

# --- tipy ---
# jiný soubor:  ELF_RC=~/muj.elfrc
# přeskočit:    LX_NO_RC=1
# regenerace:   rm ~/.cache/*-init.*
LXEOF

# doplnit ROOTFS/LX_DISTRO (spravovaný blok na začátku)
DISTRO_NAME=$(basename "$(dirname "$ROOTFS_VAL")")
[ "$DISTRO_NAME" = "proot-distro" ] && DISTRO_NAME=ubuntu
{
  echo "# --- spravuje termux-setup.sh (needituj ručně, přepíše se) ---"
  echo "export ROOTFS=\"$ROOTFS_VAL\""
  echo "export LX_DISTRO=\"${DISTRO_NAME:-ubuntu}\""
  echo "export ELF_LOADER=\"$ELF\""
  echo "# --- konec spravovaného bloku ---"
  echo
  cat "$HOME/.elfrc"
} > "$HOME/.elfrc.new"
mv "$HOME/.elfrc.new" "$HOME/.elfrc"
chmod 600 "$HOME/.elfrc"
ok "$HOME/.elfrc ($(wc -l < "$HOME/.elfrc") řádků)"

# ─── 6. ~/.zshrc — JEN zsh vrstva ──────────────────────────────────────────
# Tady nesmí být nic, co patří do .elfrc (PATH, lx, starship, aliasy).
say "Píšu ~/.zshrc"
backup "$HOME/.zshrc"
cat > "$HOME/.zshrc" <<'LXEOF'
# ~/.zshrc — POUZE zsh vrstva (setopt, bindkey, completion, pluginy)
# + jeden `init zsh`, který nastaví proměnné, lx/lxwhich a načte ~/.elfrc.
# Všechno ostatní (ROOTFS, PATH, aliasy, starship, zoxide, fzf) je v ~/.elfrc.
# Jeden rc pro oba světy: žádné /root/... natvrdo, vždy $HOME.

# ─── základní chování ──────────────────────────────────────────────────────
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

# ─── completion ───────────────────────────────────────────────────────────
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

# ─── elf_loader: L/ROOTFS/lx/lxwhich + uživatelská vrstva ─────────────────
# HOST: `init zsh` nastaví proměnné, definuje lx/lxwhich a NA KONCI SVÉHO
#       VÝSTUPU sám načte ~/.elfrc. Proto se .elfrc tady nesourcuje ručně
#       (načetl by se dvakrát, starship by se inicializoval 2×).
# GUEST: jsme pod loaderem, bionický loader nespustíme -> .elfrc načteme přímo.
if [ -z "${L:-}" ]; then
  for _lc in "${ELF_LOADER:-}" "$HOME/eltest/elf_loader" "$PREFIX/bin/elf_loader" "$HOME/usr/bin/elf_loader"; do
    [ -n "$_lc" ] && [ -x "$_lc" ] && { L=$_lc; break }
  done
  unset _lc
fi
if [ -n "${PREFIX:-}" ] && [ -x "${L:-/nonexistent}" ] && [ -z "${LX_NO_RC:-}" ]; then
  eval "$("$L" init zsh 2>/dev/null)" 2>/dev/null
elif [ -z "${LX_NO_RC:-}" ] && [ -r "${ELF_RC:-$HOME/.elfrc}" ]; then
  . "${ELF_RC:-$HOME/.elfrc}"
fi

# ─── pomocníci specifictí pro zsh ─────────────────────────────────────────
reload() { source "$HOME/.zshrc"; }
zshrc() { ${EDITOR:-nano} "$HOME/.zshrc"; }
elfrc() { ${EDITOR:-nano} "${ELF_RC:-$HOME/.elfrc}"; }

# ─── pluginy, pokud jsou ──────────────────────────────────────────────────
for _f in "$PREFIX/share/zsh/plugins/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh" \
          /usr/share/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh; do
  [ -r "$_f" ] && { . "$_f"; break }
done
unset _f
if (( ${+ZSH_HIGHLIGHT_STYLES} )); then
  ZSH_HIGHLIGHT_HIGHLIGHTERS=(main brackets)
  ZSH_HIGHLIGHT_MAXLENGTH=500
  ZSH_HIGHLIGHT_STYLES[path]=fg=blue,bold
  ZSH_HIGHLIGHT_STYLES[unknown-token]=underline
fi
for _f in "$PREFIX/share/zsh/plugins/zsh-autosuggestions/zsh-autosuggestions.zsh" \
          /usr/share/zsh-autosuggestions/zsh-autosuggestions.zsh; do
  [ -r "$_f" ] && { . "$_f"; break }
done
unset _f
[ -n "${ZSH_AUTOSUGGEST_HIGHLIGHT_STYLE:-}" ] && ZSH_AUTOSUGGEST_HIGHLIGHT_STYLE='fg=244'
[ -f /etc/zsh_command_not_found ] && . /etc/zsh_command_not_found
LXEOF
chmod 600 "$HOME/.zshrc"
zsh -n "$HOME/.zshrc" && ok "$HOME/.zshrc (jen zsh vrstva, syntaxe OK)"

# ─── 7. ~/.bashrc ─────────────────────────────────────────────────────────
say "Píšu ~/.bashrc"
backup "$HOME/.bashrc"
cat > "$HOME/.bashrc" <<'LXEOF'
# ~/.bashrc — Termux. Interaktivní shell předá zsh (viz ~/.zshrc).
# Neinteraktivní shelly (skripty, `ssh host cmd`) zůstávají bash, ale dostanou
# stejnou uživatelskou vrstvu (.elfrc) jako zsh.
case $- in
  *i*)
    if [ -x "$PREFIX/bin/zsh" ] && [ -z "${LX_NO_ZSH:-}" ]; then
      exec zsh -l
    fi
    ;;
esac
# stejna logika jako ~/.zshrc: host jde pres `init bash` (ten na konci
# sam nacte ~/.elfrc), guest sourcuje .elfrc primo
if [ -z "${LX_NO_RC:-}" ]; then
  if [ -z "${L:-}" ]; then
    for _lc in "${ELF_LOADER:-}" "$HOME/eltest/elf_loader" "$PREFIX/bin/elf_loader" \
               "$HOME/usr/bin/elf_loader"; do
      [ -n "$_lc" ] && [ -x "$_lc" ] && { L=$_lc; break; }
    done
    unset _lc
  fi
  if [ -n "${PREFIX:-}" ] && [ -x "${L:-/nonexistent}" ]; then
    eval "$("$L" init bash 2>/dev/null)" 2>/dev/null
  elif [ -r "${ELF_RC:-$HOME/.elfrc}" ]; then
    . "${ELF_RC:-$HOME/.elfrc}"
  fi
fi
[ -t 1 ] && alias ls='ls --color=auto'
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
# Guest dostane STEJNÉ soubory (jeden rc pro oba světy). `.elfrc` sám pozná
# kontext podle $PREFIX: host spouští `init` + `lx` pod loaderem, guest
# (pod loaderem, bez PREFIX) používá guest PATH a volá binárky přímo.
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
      chmod 600 "$ROOTFS_VAL$GH/.zshrc"
      ok "guest .zshrc (kopie)"

      # guest .elfrc = stejný soubor; `.elfrc` sám pozná, že už jsme
      # POD loaderem (lxwhich chybí -> postaví si ho ručně, init nespouští).
      cp -f "$HOME/.elfrc" "$ROOTFS_VAL$GH/.elfrc"
      chmod 600 "$ROOTFS_VAL$GH/.elfrc"
      zsh -n "$ROOTFS_VAL$GH/.zshrc" && ok "guest .zshrc (syntaxe OK)"
      if ( unset LD_PRELOAD; zsh -n "$ROOTFS_VAL$GH/.elfrc" ) 2>/dev/null; then
        ok "guest .elfrc (syntaxe OK)"
      else
        warn "guest .elfrc se nepodařilo ověřit"
      fi
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
