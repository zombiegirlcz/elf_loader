#!/system/bin/sh
# Vypise soubory v DIR, jejichz SELinux label se lisi od labelu DIR samotneho.
# `ls -ldZ` (lstat) misto `find -printf '%Z'` (ten sleduje symlinky, past 13).
DIR=${1:?chybi dir}
LABEL=$(ls -ldZ "$DIR" 2>/dev/null | tr -s ' ' '\n' | grep -m1 '^u:object_r:')
echo "cilovy label: $LABEL"
find "$DIR" -xdev -exec ls -ldZ {} + 2>/dev/null |
    tr -s ' ' ' ' | grep -v -F " $LABEL " | head -40
