#!/bin/bash
# Build the Magisk module zip for the parrot ELF loader.
# Requires: aarch64-linux-gnu-gcc (cross glibc), zip, and the elf_loader sources.
#
# Produces: parrot_elf_loader-vX.zip  (flash in Magisk Manager / recovery)
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT="$HERE/out"
ZIP="$HERE/parrot_elf_loader.zip"

echo "[0/4] staging dirs..."
rm -rf "$OUT"
mkdir -p "$OUT/system/bin" "$OUT/system/lib" "$OUT/META-INF/com/google/android"

echo "[1/4] cross-building elf_loader (glibc PIE)..."
aarch64-linux-gnu-gcc -Wall -Wextra -g -O0 -std=c11 -ldl \
    -o "$OUT/system/bin/elf_loader" \
    "$ROOT/src/main.c" "$ROOT/src/elf_loader.c" "$ROOT/src/entry.S"

echo "[2/4] cross-building static interpreter bridge..."
aarch64-linux-gnu-gcc -nostdlib -static -O2 -s -fno-builtin \
    -o "$OUT/system/lib/ld-linux-aarch64.so.1" \
    "$HERE/src/interp_wrapper.c" "$HERE/src/interp_wrapper.S"

echo "[2b/4] cross-building PT_INTERP patcher..."
aarch64-linux-gnu-gcc -static -O2 -s \
    -o "$OUT/system/bin/patchelf_interp" \
    "$HERE/src/patchelf_interp.c"

echo "[3/4] staging module tree..."
cp "$HERE/module.prop" "$OUT/"
cp "$HERE/customize.sh" "$HERE/service.sh" "$HERE/uninstall.sh" "$OUT/"
cp "$HERE/system/bin/parrot" "$HERE/system/bin/parrot-sh" "$HERE/system/bin/parrot-fix-exec" "$OUT/system/bin/"
chmod +x "$OUT/customize.sh" "$OUT/service.sh" "$OUT/uninstall.sh"
chmod +x "$OUT/system/bin/parrot" "$OUT/system/bin/parrot-sh" "$OUT/system/bin/parrot-fix-exec"

# Magisk module zip layout: META-INF + module root
rm -f "$ZIP"
cp "$HERE/META-INF/com/google/android/update-binary" "$OUT/META-INF/com/google/android/"
cp "$HERE/META-INF/com/google/android/updater-script" "$OUT/META-INF/com/google/android/"
if command -v zip >/dev/null 2>&1; then
    ( cd "$OUT" && zip -qr "$ZIP" . )
else
    python3 - "$OUT" "$ZIP" <<'EOF'
import os, sys, zipfile
out, zpath = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _dirs, files in os.walk(out):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, out))
EOF
fi

echo "[4/4] done: $ZIP"
ls -la "$ZIP"