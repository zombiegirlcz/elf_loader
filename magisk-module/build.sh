#!/bin/bash
# Build the ELF Loader Magisk module zip.
#
# Produces: elf_loader.zip (flash in Magisk Manager / recovery)
#
# Obsah modulu:
#   /system/bin/elf_loader   own-loading glibc loader (bionic NDK build)
#   /system/bin/zsh          bionic-native zsh 5.9.2 (host shell)
#   /system/bin/bzsh         launcher pro ten zsh (ZDOTDIR/TERMINFO)
#   /system/etc/zsh-bionic/  terminfo + zshrc (eval "$(elf_loader init zsh)")
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/out"
ZIP="$HERE/elf_loader.zip"

echo "[1/3] staging dirs..."
rm -rf "$OUT"
mkdir -p "$OUT/system/bin" "$OUT/system/etc" "$OUT/META-INF/com/google/android"

echo "[2/3] staging binaries + support files..."
cp "$HERE/system/bin/elf_loader" "$OUT/system/bin/elf_loader"
cp "$HERE/system/bin/zsh"        "$OUT/system/bin/zsh"
cp "$HERE/system/bin/bzsh"       "$OUT/system/bin/bzsh"
cp -r "$HERE/system/etc/zsh-bionic" "$OUT/system/etc/zsh-bionic"

echo "[3/3] staging module scripts + zip..."
cp "$HERE/module.prop" "$HERE/customize.sh" "$HERE/service.sh" \
   "$HERE/post-fs-data.sh" "$HERE/uninstall.sh" "$OUT/"
chmod +x "$OUT/customize.sh" "$OUT/service.sh" "$OUT/post-fs-data.sh" "$OUT/uninstall.sh"
chmod +x "$OUT/system/bin/elf_loader" "$OUT/system/bin/zsh" "$OUT/system/bin/bzsh"

cp "$HERE/META-INF/com/google/android/update-binary" "$OUT/META-INF/com/google/android/"
cp "$HERE/META-INF/com/google/android/updater-script" "$OUT/META-INF/com/google/android/"

rm -f "$ZIP"
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

echo "done: $ZIP"
ls -la "$ZIP"