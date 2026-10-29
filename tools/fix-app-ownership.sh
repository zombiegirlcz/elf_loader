#!/system/bin/sh
# Oprava vlastnictvi/SELinux labelu po operacich jako skutecny root uvnitr
# appkina rootfs (prompt 13, T4).
#
# K cemu: po apt/dpkg operacich 23.-25.10.2026 pod skutecnym rootem zustalo
# 22 087 souboru root:root a 30 291 souboru s labelem app_data_file:s0 BEZ
# kategorii (chybejici c54...c768 sufix). Appka na ne nema ani stat pravo,
# takze je normalni `du` pod appkou nevidi (rozdil 46,06 vs 52,71 GiB).
# Je to presne povinnost z korenoveho CLAUDE.md po root operaci, ktera se
# po poslednim zasahu neudelala.
#
# DULEZITE: vlastnik a SELinux kategorie se NEBEROU natvrdo - zapsana pamet
# projektu drive uvadela u0_a310 / c54,c257,c512,c768, ale tahle appka ma
# u0_a323 / c67,c257,c512,c768 - kategorie plynou z UID a lisi se appku od
# appky. Skript si spravny vlastnik+label VZDY odvodi ze SAMOTNE cilove
# slozky (ne z nejakeho jineho souboru v rootfs, ktery muze byt sam spatne).
#
# Pouziti:
#   tools/fix-app-ownership.sh --dir DIR [--dry-run|--apply]
#
# --dry-run je DEFAULT (jen report pred/po pocty). --apply skutecne
# chown/chcon. Bezet jako root (su 0), jinak chown/chcon stejne neprojdou.

set -eu

DIR=""
APPLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dir)     DIR="$2"; shift 2 ;;
        --apply)   APPLY=1; shift ;;
        --dry-run) APPLY=0; shift ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "neznamy argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$DIR" ] || { echo "chybi --dir" >&2; exit 2; }
[ -d "$DIR" ] || { echo "neni adresar: $DIR" >&2; exit 2; }

if [ "$(id -u 2>/dev/null || echo 1)" != "0" ]; then
    echo "chybi root (su 0) - chown/chcon jinak nic nezmeni" >&2
    exit 1
fi

# vlastnik a label odvozene ze SAMOTNE cilove slozky, ne hardcode
OWNER=$(stat -c '%U:%G' "$DIR" 2>/dev/null || stat -c '%u:%g' "$DIR")
LABEL=$(ls -ldZ "$DIR" 2>/dev/null | awk '{print $1}')

[ -n "$OWNER" ] || { echo "nepodarilo se zjistit vlastnika $DIR" >&2; exit 1; }
[ -n "$LABEL" ] || { echo "nepodarilo se zjistit SELinux label $DIR" >&2; exit 1; }

echo "fix-app-ownership: $DIR"
echo "  cilovy vlastnik: $OWNER"
echo "  cilovy label:    $LABEL"

n_wrong_owner_before=$(find "$DIR" -xdev -user 0 2>/dev/null | wc -l)
# past c. 13: find -printf '%Z' sleduje symlinky a hlasi label CILE, ne
# odkazu samotneho - na pocty "spatnych" labelu tedy nespoleh, jen report
# vlastnictvi je spolehlivy pres -user 0. Label kontroluj rucne ls -ldZ.
echo "  souboru root:root pred:  $n_wrong_owner_before"

if [ "$APPLY" != 1 ]; then
    echo "(--dry-run, nic se nemeni; --apply pro skutecnou opravu)"
    exit 0
fi

# chown vcetne symlinku (-h), zvlast kvuli past c. 7/11 (absolutni/l2s
# symlinky) - chown bez -h by prepsal vlastnika CILE, ne odkazu
find "$DIR" -xdev -user 0 -exec chown "$OWNER" {} + 2>/dev/null || true
find "$DIR" -xdev -user 0 -exec chown -h "$OWNER" {} + 2>/dev/null || true

# SELinux relabel - chcon -h pro symlinky, pak restorecon jako pojistka
find "$DIR" -xdev -exec chcon -h "$LABEL" {} + 2>/dev/null || true
restorecon -R "$DIR" 2>/dev/null || true

n_wrong_owner_after=$(find "$DIR" -xdev -user 0 2>/dev/null | wc -l)
echo "  souboru root:root po:    $n_wrong_owner_after"
echo "hotovo."
