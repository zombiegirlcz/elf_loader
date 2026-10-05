#!/data/user/0/com.linux_core/files/usr/bin/zsh
# elf_loader — vizuální demo pro screen recording / prezentaci.
#
# Ukazuje pointu projektu: STEJNÉ jádro, DVA userspace.
#   - host  = Android (bionic, žádné /etc/os-release, žádný glibc)
#   - guest = Parrot/Debian rootfs (glibc 2.41) spuštěný BEZ rootu,
#             BEZ chrootu a BEZ PRootu — jen přes elf_loader (--ownall).
#
# Cesty se berou z jediného zdroje pravdy (`elf_loader init zsh`), žádný
# hardcode. Lze přebít:  ROOTFS=/jinam/rootfs lx-demo
emulate -L zsh
setopt no_nomatch

eval "$(${L:-$HOME/usr/bin/elf_loader} init zsh)"

C=$'\e[36m'; G=$'\e[32m'; B=$'\e[1m'; GY=$'\e[90m'; N=$'\e[0m'
step=0
title() { ((step++)); print; print "${B}${C}▐ ${step}) $1${N}"; sleep 0.7 }
run()   { print "${GY}\$ $1${N}"; sleep 0.9; eval "$1"; sleep 0.8 }
ok()    { print "${G}  ✓ $1${N}"; sleep 0.6 }

print "${B}${C}  elf_loader${N}${B} — glibc (Linux) binárky pod Androidem${N}"
print "  ${GY}bez rootu · bez chroot · bez PRootu${N}"
sleep 1.5

title "Pořád jsme na Androidu (bionic host)"
run "uname -srm"
run "echo \"  Android \$(getprop ro.build.version.release 2>/dev/null)\""

title "Android není Linux distro — nemá /etc/os-release"
run "cat /etc/os-release 2>&1 | head -1"

title "Glibc binárku přímo nespustíme — chybí /lib/ld-linux-aarch64.so.1"
run "\$R/usr/bin/python3 -c 'print(1)' 2>&1 | head -1"

title "Přes loader pustíme celý Parrot/Debian userspace (glibc 2.41)"
run "lx cat /etc/os-release 2>&1 | head -2"
run "lx python3 -c 'import platform;print(\"  glibc\", platform.libc_ver()[1])' </dev/null"
ok "stejné jádro, ale uživatelský prostor z rootfs"

title "Reálný nástroj: zkompiluj a spusť C program uvnitř guestu"
run "mkdir -p \$R/tmp/demo"
run "printf '%s\\n' '#include <stdio.h>' 'int main(void){printf(\"hello z glibc, %d\\\\n\",6*7);}' > \$R/tmp/demo/h.c"
run "lx gcc \$R/tmp/demo/h.c -o \$R/tmp/demo/h && echo '  zkompilováno guest gcc 14.2 (Debian)'"
run "lx \$R/tmp/demo/h 2>&1 | tail -1"

print
print "${G}${B}  Hotovo — jedna binárka elf_loader, dva userspace.${N}"
print