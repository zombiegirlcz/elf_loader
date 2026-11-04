#!/bin/bash
# deploy_bin.sh <lokální_soubor> <device_cesta> [perms]
# Appka a rootfs koexistují v jedné appce - device cesta je tady vidět
# přímo přes bind mount, žádná síť/ashell není potřeba (nahrazuje push_bin.sh).
# chmod PŘED mv je nutné (viz historická poznámka k push_bin.sh v postup.md):
# okno mezi truncate a chmod by appku dočasně zneschopnilo (ETXTBSY-safe rename).
set -e
SRC="$1"; DST="$2"; PERMS="${3:-755}"
[ -f "$SRC" ] || { echo "není: $SRC"; exit 1; }

cp "$SRC" "$DST.new"
chmod "$PERMS" "$DST.new"
mv -f "$DST.new" "$DST"

GOT=$(stat -c%s "$DST")
ORIG=$(stat -c%s "$SRC")
if [ "$GOT" = "$ORIG" ]; then
    echo "OK: $DST ($GOT B)"
else
    echo "FAIL: $DST má $GOT B, očekáváno $ORIG B"; exit 1
fi
