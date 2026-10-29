#!/bin/sh
# Postavi oba kusy nativniho modu (prompt 12, prototyp).
#
# Pousti se V ROOTFS (pod loaderem nebo prootem), protoze oboji prekladá
# rootfs gcc - NE NDK, NE Modal. `lxnative` je freestanding staticka ET_EXEC
# binarka spustitelna v hostovem bionic kontextu, `liblx-audit.so` je obycejna
# glibc knihovna, kterou nacita rootfs ld.so.
set -eu

OUT="${1:-/data/user/0/com.linux_core/files/tmp/p12}"
SRC=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"

# -Wl,... nepouzivame: prazdna .data vyrobi PT_LOAD s p_filesz == 0 a jadro
# na tomhle zarizeni takovy segment nenamapuje (SIGSEGV pred prvni instrukci).
# Resi to inicializovana globalka ve zdrojaku, viz lxnative.c.
gcc -O1 -static -nostdlib -nostartfiles -ffreestanding -fno-stack-protector \
    -o "$OUT/lxnative" "$SRC/lxnative.c"

gcc -O2 -fPIC -shared -o "$OUT/liblx-audit.so" "$SRC/liblx-audit.c"

install -m 0755 "$SRC/lxnative-run.sh" "$OUT/lxnative-run.sh"

echo "hotovo: $OUT/{lxnative,liblx-audit.so,lxnative-run.sh}"
readelf -lW "$OUT/lxnative" | grep -E '^  LOAD' || true
