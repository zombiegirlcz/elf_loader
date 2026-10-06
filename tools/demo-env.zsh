#!/data/user/0/com.linux_core/files/usr/bin/zsh
# elf_loader — demo "from scratch": starts in an EMPTY environment (env -i)
# and progressively builds the paths, then runs starship, python, uv, node, pi.
#
# Run (authentic empty environment):
#   env -i HOME=/data/user/0/com.linux_core/files \
#       /data/user/0/com.linux_core/files/usr/bin/zsh \
#       /data/user/0/com.linux_core/files/tmp/demo-env.zsh
#
# Point: same kernel (Android), two userspaces (bionic host + glibc guest),
# without root, without chroot, without PRoot — through one elf_loader binary.
emulate -L zsh
setopt no_nomatch

# ── pause via host /system/bin/sleep (independent of PATH in env -i) ────
SLEEP=/system/bin/sleep
# pause <centiseconds>  (120 = 1.2 s)
pause() { [ -x $SLEEP ] && $SLEEP "$(printf '%.2f' $(( ${1:-60} / 100.0 )))" 2>/dev/null || : }

C=$'\e[36m'; G=$'\e[32m'; B=$'\e[1m'; GY=$'\e[90m'; N=$'\e[0m'
step=0
title() { ((step++)); print; print "${B}${C}▐ ${step}) $1${N}"; pause 70 }
run()   { print "${GY}\$ $1${N}"; pause 80; eval "$1"; pause 70 }
ok()    { print "${G}  ✓ $1${N}"; pause 60 }

print "${B}${C}  elf_loader${N}${B} — from an empty environment to a glibc guest${N}"
print "  ${GY}no root · no chroot · no PRoot${N}"
pause 120

# ── 1) proof we really start from nothing ──────────────────────────────
title "Starting in an empty environment (env -i)"
run "print -l \${(k)env}"
run "print \"HOME=\$HOME PATH=[\$PATH]\""

# ── 2) set only the minimum — derived from HOME ────────────────────────
title "Set up the minimum (paths derived from \$HOME)"
run "export D=\$HOME"
run "export ROOTFS=\$D/nh/distro/parrot"
run "export R=\$ROOTFS"
run "export L=\$D/usr/bin/elf_loader"
run "export LX_LOG=\$HOME/.cache/lx"
run "export PATH=\$D/usr/bin:/system/bin:\$PATH"

# ── 3) one eval and we have the whole lx environment ───────────────────
title "One eval replaces manual setup — lx/lxwhich/LOCPATH/LC_ALL"
run "eval \"\$(\$L init zsh)\""
run "print \"LOCPATH=\$LOCPATH\""
run "whence -w lx lxwhich lxtest help"
ok "single source of truth: elf_loader init zsh"

# ── 4) the loader's own help ───────────────────────────────────────────
title "The loader's own help (elf_loader --help)"
run "\$L --help 2>&1 | head -18"
ok "one binary, documented interface"

# ── 5) shell helper help (lxhelp) ──────────────────────────────────────
title "Shell helper help (lxhelp)"
run "help"
ok "the help system is part of the environment too"

# ── 6) starship ────────────────────────────────────────────────────────
title "Set up starship (a glibc binary from the rootfs)"
run "export STARSHIP_CONFIG=\$R/root/.config/starship.toml"
run "export STARSHIP_CACHE=\$HOME/.cache/starship"
run "lxwhich starship"
run "lx starship --version </dev/null 2>&1"
ok "starship runs as a glibc process on the bionic host"

# ── 7) python ──────────────────────────────────────────────────────────
title "Guest Python (glibc 2.41) under the loader"
run "lx python3 -c 'import sys,platform;print(\"  Python\",sys.version.split()[0],\"|\",platform.libc_ver())' </dev/null"
ok "guest Python knows its glibc — it runs under the loader, not bionic"
run "\$R/usr/bin/python3 -c 'print(1)' </dev/null"
ok "running the same Python directly (no loader) fails (no ld-linux)"

# ── 8) uv ──────────────────────────────────────────────────────────────
title "uv (Rust, glibc) — Python environment manager"
run "lxwhich uv"
run "lx uv --version </dev/null"

# ── 9) node ────────────────────────────────────────────────────────────
title "Node.js from nvm in the rootfs (glibc)"
run "lx node -v </dev/null"
run "lx node -e 'console.log(\"  node computed 6*7 =\", 6*7)' </dev/null"
ok "Node (glibc) and the V8 JIT work under the loader"

# ── 10) pi ─────────────────────────────────────────────────────────────
title "pi (Node TUI) as a glibc Node app under the loader"
# pi needs PI_CODING_AGENT_DIR (= $R/root/.pi/agent), NOT install.
# Without it the launcher tries npm install, which only works under proot.
run "export PI_CODING_AGENT_DIR=\$R/root/.pi/agent"
run "lx pi --version </dev/null 2>&1"
ok "pi loads under the loader from the rootfs"

# ── 11) hand over to an interactive zsh with the starship prompt ───────
title "Handing over to an interactive zsh — the starship prompt starts"
unset PREFIX   # nvm otherwise warns ("not compatible with PREFIX")
print "${GY}  zsh loads ~/.zshrc -> starship prompt (glibc binary under the loader)${N}"
pause 120
exec ${SHELL:-$D/usr/bin/zsh} -i