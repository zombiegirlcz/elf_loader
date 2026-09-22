#!/bin/sh
# Build elf_loaderu pres GitHub Actions + deploy na device.
# Nahrada za `modal run finale_loader_build.py`, dokud je Modal nedostupny.
#
# Pouziti:
#   tools/gh_build_deploy.sh           # cekej na run pro HEAD, stahni, nasad
#   tools/gh_build_deploy.sh --push    # nejdriv pushni aktualni vetev
#   tools/gh_build_deploy.sh <run-id>  # pouzij konkretni run
#
# Deploy cesta: /mnt/app = bind na /data/user/0/com.linux_core (app data dir),
# takze /mnt/app/files/... je primo device. Zadny ashell, zadne base64.
set -e

APP=/mnt/app
DEST=$APP/files/usr/bin/elf_loader
OUT=/tmp/ndkart
BR=$(git rev-parse --abbrev-ref HEAD)

[ -d "$APP/files" ] || { echo "[-] $APP/files neexistuje - bind na app data dir chybi"; exit 1; }

RUN=""
case "$1" in
    --push) git push origin "$BR"; sleep 10 ;;
    [0-9]*) RUN="$1" ;;
esac

SHA=$(git rev-parse HEAD)
if [ -z "$RUN" ]; then
    echo "[*] hledam run pro $SHA ($BR)"
    i=0
    while [ $i -lt 30 ]; do
        RUN=$(gh run list -b "$BR" -L 5 --json databaseId,headSha \
              --jq "[.[] | select(.headSha==\"$SHA\")][0].databaseId")
        [ -n "$RUN" ] && [ "$RUN" != "null" ] && break
        sleep 5; i=$((i + 1))
    done
fi
[ -n "$RUN" ] && [ "$RUN" != "null" ] || { echo "[-] run pro $SHA nenalezen"; exit 1; }

echo "[*] run $RUN"
gh run watch "$RUN" --exit-status --interval 10 >/dev/null 2>&1 || true
CONC=$(gh run view "$RUN" --json conclusion --jq .conclusion)
[ "$CONC" = "success" ] || { echo "[-] build skoncil: $CONC"; gh run view "$RUN" --log-failed | tail -30; exit 1; }

rm -rf "$OUT"
gh run download "$RUN" -n elf_loader_ndk -D "$OUT"

# POVINNA kontrola: musi to byt bionic binarka, ne glibc build (viz SKILL.md)
readelf -h "$OUT/elf_loader_ndk" | grep -q AArch64 || { echo "[-] neni AArch64"; exit 1; }
readelf -l "$OUT/elf_loader_ndk" | grep -q '/system/bin/linker64' \
    || { echo "[-] spatny interpreter (glibc build?)"; exit 1; }

# Prepis bezici binarky pada na "Text file busy" -> nejdriv vedle, pak mv.
cp -f "$OUT/elf_loader_ndk" "$DEST.new"
chmod 755 "$DEST.new"
mv -f "$DEST.new" "$DEST"
echo "[+] nasazeno: $DEST ($(stat -c %s "$DEST") B, run $RUN)"
