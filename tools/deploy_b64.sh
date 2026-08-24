#!/bin/bash
# deploy_b64.sh <local_file> <device_path> [chunk]
# Robustní upload přes ashell s verifikací velikosti.
LOCAL="$1"
DEST="$2"
F=/data/user/0/com.linux_core/files

[ -f "$LOCAL" ] || { echo "no $LOCAL"; exit 1; }

B64=$(base64 -w0 "$LOCAL")
LEN=${#B64}
EXPECT_B64=$(( LEN + (LEN + 899) / 900 ))   # + newline per chunk

for attempt in 1 2 3 4 5; do
    i=0; first=1
    FAIL=0
    while [ $i -lt $LEN ]; do
        C=${B64:$i:900}
        if [ $first = 1 ]; then
            ashell -c "echo '$C' > $F/deploy.b64" >/dev/null 2>&1 || FAIL=1
            first=0
        else
            ashell -c "echo '$C' >> $F/deploy.b64" >/dev/null 2>&1 || FAIL=1
        fi
        i=$((i+900))
        # krátká pauza každou desítku chunků (session stabilita)
        if [ $(( (i / 900) % 10 )) = 0 ]; then sleep 0.3; fi
    done

    # verifikace délky b64 na device
    GOT=$(ashell -c "/system/bin/wc -c $F/deploy.b64" 2>/dev/null | awk '{print $1}')
    if [ "$GOT" != "$EXPECT_B64" ]; then
        echo "attempt $attempt: b64 size mismatch (got=$GOT want=$EXPECT_B64), retrying..."
        sleep 1
        continue
    fi

    # dekód + verifikace velikosti
    OUT=$(ashell -c "/system/bin/base64 -d $F/deploy.b64 > $DEST.tmp && /system/bin/wc -c $DEST.tmp" 2>/dev/null | awk '{print $1}')
    if [ "$OUT" = "$(wc -c < "$LOCAL")" ]; then
        ashell -c "/system/bin/rm -f $DEST.old" >/dev/null 2>&1
        ashell -c "/system/bin/cp $DEST $DEST.old" >/dev/null 2>&1
        ashell -c "/system/bin/cat $DEST.tmp > $DEST && /system/bin/chmod 755 $DEST && /system/bin/rm $F/deploy.b64 $DEST.tmp $DEST.old" >/dev/null 2>&1
        echo "OK: $DEST ($OUT bytes, attempt $attempt)"
        exit 0
    fi
    echo "attempt $attempt: decoded size mismatch (got=$OUT want=$(wc -c < $LOCAL))"
    sleep 1
done
echo "FAILED after 5 attempts"
exit 1
