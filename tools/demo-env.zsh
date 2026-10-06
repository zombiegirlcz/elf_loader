#!/data/user/0/com.linux_core/files/usr/bin/zsh
# elf_loader — demo „od nuly": začínáme v PRÁZDNÉM prostředí (env -i)
# a postupně stavíme cesty, pustíme starship, python, uv, node a nakonec pi.
#
# Spuštění (autentické, prázdné prostředí):
#   env -i HOME=/data/user/0/com.linux_core/files \
#       /data/user/0/com.linux_core/files/usr/bin/zsh \
#       /data/user/0/com.linux_core/files/tmp/demo-env.zsh
#
# Pointa: stejné jádro (Android), dva userspace (bionic host + glibc guest),
# bez rootu, bez chroot, bez PRootu — jen přes jednu binárku elf_loader.
emulate -L zsh
setopt no_nomatch

# ── pauza přes host /system/bin/sleep (nezávisí na PATH v env -i) ───────
SLEEP=/system/bin/sleep
# pause <setiny>  (120 = 1,2 s)
pause() { [ -x $SLEEP ] && $SLEEP "$(printf '%.2f' $(( ${1:-60} / 100.0 )))" 2>/dev/null || : }

C=$'\e[36m'; G=$'\e[32m'; B=$'\e[1m'; GY=$'\e[90m'; N=$'\e[0m'
step=0
title() { ((step++)); print; print "${B}${C}▐ ${step}) $1${N}"; pause 70 }
run()   { print "${GY}\$ $1${N}"; pause 80; eval "$1"; pause 70 }
ok()    { print "${G}  ✓ $1${N}"; pause 60 }

print "${B}${C}  elf_loader${N}${B} — z prázdného prostředí ke glibc guestu${N}"
print "  ${GY}bez rootu · bez chroot · bez PRootu${N}"
pause 120

# ── 1) Důkaz, že opravdu začínáme od nuly ──────────────────────────────
title "Začínáme v prázdném prostředí (env -i)"
run "print -l \${(k)env}"
run "print \"HOME=\$HOME PATH=[\$PATH]\""

# ── 2) Nastavíme jen minimum — odvodí se z HOME ────────────────────────
title "Postupně nastavíme prostředí (jediné minimum — cesty z \$HOME)"
run "export D=\$HOME"
run "export ROOTFS=\$D/nh/distro/parrot"
run "export R=\$ROOTFS"
run "export L=\$D/usr/bin/elf_loader"
run "export LX_LOG=\$HOME/.cache/lx"
run "export PATH=\$D/usr/bin:/system/bin:\$PATH"

# ── 3) Jedno eval a máme celé lx prostředí ─────────────────────────────
title "Jedno eval nahradí ruční nastavování — lx/lxwhich/LOCPATH/LC_ALL"
run "eval \"\$(\$L init zsh)\""
run "print \"LOCPATH=\$LOCPATH\""
run "whence -w lx lxwhich lxtest help"
ok "jediný zdroj pravdy: elf_loader init zsh"

# ── 4) starship — guest prompt, host terminál ──────────────────────────
title "Nastavíme starship (glibc binárka z rootfs) a necháme ho naběhnout"
run "export STARSHIP_CONFIG=\$R/root/.config/starship.toml"
run "export STARSHIP_CACHE=\$HOME/.cache/starship"
run "lxwhich starship"
run "lx starship --version </dev/null 2>&1"
ok "starship běží jako glibc binárka nad bionickým hostem"

# ── 5) Python z rootfs (glibc) ─────────────────────────────────────────
title "Guest Python (glibc 2.41) pod loaderem"
run "lx python3 -c 'import sys,platform;print(\"  Python\",sys.version.split()[0],\"|\",platform.libc_ver())' </dev/null"
ok "guest Python zná svou glibc — běží pod loaderem, ne pod bionic"
run "\$R/usr/bin/python3 -c 'print(1)' </dev/null"
ok "přímé spuštění téhož pythonu bez loaderu selže (chybí ld-linux)"

# ── 6) uv ──────────────────────────────────────────────────────────────
title "uv (Rust, glibc) — správce Python prostředí"
run "lxwhich uv"
run "lx uv --version </dev/null"

# ── 7) Node ────────────────────────────────────────────────────────────
title "Node.js z nvm v rootfs (glibc)"
run "lx node -v </dev/null"
run "lx node -e 'console.log(\"  node spočítal 6*7 =\", 6*7)' </dev/null"
ok "Node (glibc) i V8 JIT fungují pod loaderem"

# ── 8) pi (Node TUI) — jen ověření verze ──────────────────────────────
title "A pi (Node TUI) jako glibc Node aplikace pod loaderem"
# pi potřebuje PI_CODING_AGENT_DIR (= $R/root/.pi/agent), NE install.
# Bez něj launcher zkusí npm install, což funguje jen pod prootem.
run "export PI_CODING_AGENT_DIR=\$R/root/.pi/agent"
run "lx pi --version </dev/null 2>&1"
ok "pi nabíhá pod loaderem z rootfs (interaktivně si ho pustíš sám)"

# ── 9) Předání interaktivnímu zsh se starship promptem ─────────────────
title "Předávám řízení interaktivnímu zsh — naběhne starship prompt"
unset PREFIX   # nvm jinak varuje ("not compatible with PREFIX")
print "${GY}  zsh načte ~/.zshrc → starship prompt (glibc binárka pod loaderem)${N}"
print "${GY}  pi si spusť ručně:   lx pi${N}"
pause 120
exec ${SHELL:-$D/usr/bin/zsh} -i