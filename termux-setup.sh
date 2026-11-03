#!/data/data/com.termux/files/usr/bin/bash
# termux-setup.sh — nakonfiguruje Termux tak, aby `proot` byl `elf_loader`.
#
# Co udělá:
#   1. najde elf_loader
#   2. najde rootfs ($ROOTFS, proot-distro layouty; jinak se zeptá)
#   3. doplní zsh
#   4. proot -> proot.orig, proot -> symlink na elf_loader
#   5. do ~/.elfrc přidá blok s proměnnými a voláním `elf_loader init`
#   6. do ~/.zshrc přidá `source ~/.elfrc`
#   7. do ~/.bashrc přidá `source ~/.elfrc` (interaktivní předá zsh)
#   8. ověří
#
# Nic nepřepisuje: do rc souborů jen přidává/aktualizuje svůj blok
# (mezi markery `# >>> elf_loader (termux-setup.sh) >>>` a `<<<`).
# Žádné zálohy, žádný starship.toml, žádné kopírování rc do rootfs.
#
# Spuštění:   bash termux-setup.sh
# Bez otázek: ROOTFS=/cesta bash termux-setup.sh --yes

set -euo pipefail

ASSUME_YES=0
for a in "$@"; do
  case $a in
    -y|--yes) ASSUME_YES=1 ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "neznámý argument: $a" >&2; exit 2 ;;
  esac
done

BEGIN='# >>> elf_loader (termux-setup.sh) >>>'
END='# <<< elf_loader (termux-setup.sh) <<<'

# ─── 0. jsme v Termuxu? ────────────────────────────────────────────────────
if [ -z "${PREFIX:-}" ] || [ ! -d "${PREFIX:-/nonexistent}/bin" ]; then
  echo "CHYBA: tohle nevypadá na Termux (PREFIX není nastavené)." >&2
  echo "       Spusť skript v Termuxu, ne v prootu ani na hostiteli." >&2
  exit 1
fi

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '  \033[1;32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[1;33m!\033[0m %s\n' "$*"; }
die()  { printf '\n\033[1;31mCHYBA:\033[0m %s\n' "$*" >&2; exit 1; }

# managed_block <soubor> <soubor-s-obsahem>
# Do <soubor> vloží obsah mezi markery. Když tam blok už je, nahradí ho.
# Když není, připíše ho na konec. Nic jiného v souboru nemění.
managed_block() {
  local f=$1 body=$2 tmp
  tmp="$f.lxtmp.$$"
  touch "$f"
  if grep -qF "$BEGIN" "$f" 2>/dev/null; then
    awk -v b="$BEGIN" -v e="$END" -v bf="$body" '
      $0 == b { print; while ((getline l < bf) > 0) print l; close(bf); skip=1; next }
      skip && $0 == e { print; skip=0; next }
      skip { next }
      { print }
    ' "$f" > "$tmp"
    mv "$tmp" "$f"
  else
    [ -s "$f" ] && echo >> "$f"
    { echo "$BEGIN"; cat "$body"; echo "$END"; } >> "$f"
  fi
  rm -f "$body"
}

ask() {  # ask <prompt> <default>
  local prompt=$1 def=${2:-} ans
  if [ "$ASSUME_YES" = 1 ] && [ -n "$def" ]; then printf '%s' "$def"; return; fi
  printf '%s' "$prompt" >&2
  IFS= read -r ans || true
  [ -n "$ans" ] || ans=$def
  printf '%s' "$ans"
}

# ─── 1. elf_loader ────────────────────────────────────────────────────────
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

# ─── 2. ROOTFS ────────────────────────────────────────────────────────────
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

# ─── 3. balíčky ───────────────────────────────────────────────────────────
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

# ─── 4. proot -> proot.orig, symlink na elf_loader ────────────────────────
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

# Ověření symlinku. `proot --version` NENÍ platný test: `--version` je pro
# proot-compat neznámá volba, loader spadne do fallbacku na skutečný proot
# (teď proot.orig) a hlásí "execve(proot) failed".
if "$PROOT" -r "$ROOTFS_VAL" /usr/bin/true >/dev/null 2>&1; then
  ok "ověřeno: proot -r rootfs /usr/bin/true prošlo loaderem"
else
  warn "proot -r rootfs /usr/bin/true selhalo (zkontroluj ručně)"
fi

# ─── 5. ~/.elfrc ──────────────────────────────────────────────────────────
# Jen proměnné pro loader + `init` + nástroje z rootfs. Žádné aliasy, žádný
# starship.toml, žádná kosmetika (tu si uživatel dává do ~/.zshrc).
say "Doplňuji ~/.elfrc"

DISTRO_NAME=$(basename "$(dirname "$ROOTFS_VAL")")
[ "$DISTRO_NAME" = "proot-distro" ] && DISTRO_NAME=ubuntu

ELFRC_BODY=$(mktemp)
cat > "$ELFRC_BODY" <<'LXEOF'
# Konfigurace elf_loaderu. Vlastní nastavení piš POD tento blok.
#
# `elf_loader init` na konci svého výstupu sourcuje tento soubor ZNOVU,
# proto je hned na začátku guard proti rekurzi. Na konci se guard zruší,
# aby šel soubor znovu načíst (reload).

[ -n "${_LX_ELF_INIT:-}" ] && return 0 2>/dev/null
_LX_ELF_INIT=1

export ROOTFS="@ROOTFS@"
export LX_DISTRO="@DISTRO@"
export ELF_LOADER="@LOADER@"
# `init` čte $L (ne $ELF_LOADER) a $D; bez toho by $L skončilo na
# neexistující cestě $D/usr/bin/elf_loader.
export L="@LOADER@"
export D="${D:-$HOME}"

if [ -n "${ZSH_VERSION:-}" ]; then _LX_SHELL=zsh; else _LX_SHELL=bash; fi

# Až po exportu ROOTFS/L — jinak by init autodetekoval rootfs podle
# LX_DISTRO (default parrot) a R/LX_PATH by ukazovaly mimo.
if [ -x "$ELF_LOADER" ]; then
  eval "$("$ELF_LOADER" init "$_LX_SHELL" 2>/dev/null)" 2>/dev/null
fi

if ! command -v lxwhich >/dev/null 2>&1; then
  unset _LX_ELF_INIT
  return 0 2>/dev/null
fi

# --- nástroje z rootfs (guest glibc binárky pod loaderem) ------------------
# Berou se přes lxwhich/lx (rootfs), nikdy přes command -v (Termux).
# Když v rootfs nejsou, blok se přeskočí.
if lxwhich zoxide >/dev/null 2>&1; then
  zoxide() { lx zoxide "$@"; }
  export _ZO_DATA_DIR="${_ZO_DATA_DIR:-$HOME/.local/share/zoxide}"
  _zc=$HOME/.cache/zoxide-init.$_LX_SHELL
  if [ ! -s "$_zc" ] || [ "$_zc" -ot "$(lxwhich zoxide)" ]; then
    lx zoxide init "$_LX_SHELL" --cmd z 2>/dev/null | sed 's/command zoxide/zoxide/g' > "$_zc"
  fi
  [ -r "$_zc" ] && . "$_zc"
  unset _zc
fi

if [ "$_LX_SHELL" = zsh ] && lxwhich fzf >/dev/null 2>&1; then
  fzf() { lx fzf "$@"; }
  _fz=$HOME/.cache/fzf-init.zsh
  if [ ! -s "$_fz" ] || [ "$_fz" -ot "$(lxwhich fzf)" ]; then
    lx fzf --zsh 2>/dev/null | sed 's/command fzf/fzf/g' > "$_fz"
  fi
  [ -r "$_fz" ] && . "$_fz"
  unset _fz
fi

# starship: config i cache jsou V ROOTFS (guest cesty, loader je přeloží).
# Setup sám žádný starship.toml nepíše ani nepřepisuje.
if [ "$_LX_SHELL" = zsh ] && lxwhich starship >/dev/null 2>&1; then
  export STARSHIP_CONFIG="${STARSHIP_CONFIG:-/root/.config/starship.toml}"
  export STARSHIP_CACHE="${STARSHIP_CACHE:-/root/.cache/starship}"
  starship() { lx starship "$@"; }
  _ssc=$HOME/.cache/starship-init.zsh
  if [ ! -s "$_ssc" ] || [ "$_ssc" -ot "$(lxwhich starship)" ]; then
    lx starship init zsh 2>/dev/null | sed 's/command starship/starship/g' > "$_ssc"
  fi
  [ -r "$_ssc" ] && . "$_ssc"
  unset _ssc
fi

unset _LX_ELF_INIT
LXEOF

sed -e "s|@ROOTFS@|$ROOTFS_VAL|g" \
    -e "s|@DISTRO@|${DISTRO_NAME:-ubuntu}|g" \
    -e "s|@LOADER@|$ELF|g" \
    "$ELFRC_BODY" > "$ELFRC_BODY.2" && mv "$ELFRC_BODY.2" "$ELFRC_BODY"

managed_block "$HOME/.elfrc" "$ELFRC_BODY"
( unset LD_PRELOAD; zsh -n "$HOME/.elfrc" ) 2>/dev/null \
  && ok "$HOME/.elfrc (blok aktualizován, syntaxe OK)" \
  || warn "$HOME/.elfrc se nepodařilo ověřit"

# ─── 6. ~/.zshrc — jen source ─────────────────────────────────────────────
# Nic víc. Kosmetiku (setopt, bindkey, compinit) si uživatel dává sám.
say "Doplňuji ~/.zshrc"
ZSHRC_BODY=$(mktemp)
cat > "$ZSHRC_BODY" <<'LXEOF'
# Načte konfiguraci elf_loaderu (proměnné + init + nástroje z rootfs).
[ -z "${LX_NO_RC:-}" ] && [ -r "${ELF_RC:-$HOME/.elfrc}" ] && . "${ELF_RC:-$HOME/.elfrc}"
LXEOF
managed_block "$HOME/.zshrc" "$ZSHRC_BODY"
zsh -n "$HOME/.zshrc" && ok "$HOME/.zshrc (blok aktualizován)"

# ─── 7. ~/.bashrc — interaktivní předá zsh, jinak source ──────────────────
say "Doplňuji ~/.bashrc"
BASHRC_BODY=$(mktemp)
cat > "$BASHRC_BODY" <<'LXEOF'
# Interaktivní bash předá zsh (ten si .elfrc načte sám), neinteraktivní
# (skripty, `ssh host cmd`) zůstává bash a načte .elfrc přímo.
case $- in
  *i*)
    if [ -x "$PREFIX/bin/zsh" ] && [ -z "${LX_NO_ZSH:-}" ]; then
      exec zsh -l
    fi
    ;;
esac
[ -z "${LX_NO_RC:-}" ] && [ -r "${ELF_RC:-$HOME/.elfrc}" ] && . "${ELF_RC:-$HOME/.elfrc}"
LXEOF
managed_block "$HOME/.bashrc" "$BASHRC_BODY"
bash -n "$HOME/.bashrc" && ok "$HOME/.bashrc (blok aktualizován)"

# ─── 8. ověření ───────────────────────────────────────────────────────────
say "Ověřuji"
DIAG=$( ( unset LD_PRELOAD; zsh -i -c 'echo "L=$L"; echo "R=$R"; echo "PATH_OK=$([ -n "$LX_PATH" ] && echo yes || echo no)"; command -v lxwhich >/dev/null && echo "lxwhich=yes" || echo "lxwhich=no"; lxwhich starship 2>/dev/null || echo "starship=none"' ) 2>&1 | tail -6 )
printf '%s\n' "$DIAG" | sed 's/^/     /'

if printf '%s\n' "$DIAG" | grep -q '^lxwhich=yes'; then
  ok "lxwhich je definované"
else
  warn "lxwhich NENÍ definované — .elfrc se nenačetl nebo init selhal"
fi

SS=$(printf '%s\n' "$DIAG" | sed -n 's/^\(.*\/starship\)$/\1/p')
if [ -n "$SS" ]; then
  ok "starship z rootfs: $SS"
  if [ -x "$SS" ]; then
    if ( unset LD_PRELOAD; zsh -i -c 'starship --version' ) >/dev/null 2>&1; then
      ok "starship se pod loaderem spustí"
    else
      warn "starship nalezen, ale nespustí se (chybí závislosti v rootfs?)"
    fi
  fi
else
  warn "starship v rootfs není (apt install starship) — prompt pojede bez něj"
  printf '     rootfs: %s\n' "$ROOTFS_VAL"
  printf '     zkus:  %s --ownall %s/usr/bin/apt-get install -y starship\n' "$ELF" "$ROOTFS_VAL"
fi

if ( unset LD_PRELOAD; zsh -i -c 'lx /bin/true' ) >/dev/null 2>&1; then
  ok "lx /bin/true funguje"
else
  warn "lx /bin/true selhal"
fi

cat <<EOF

\033[1;32mHotovo.\033[0m Spusť nový shell:

  exec zsh -l

Do rc souborů byl vložen blok mezi markery
  $BEGIN
  $END
Ostatní obsah rc souborů zůstal nedotčený. Loader: $ELF
Rootfs: $ROOTFS_VAL
EOF