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
# POZOR: label NENI prvni sloupec. Toybox `ls -ldZ` dava
#   drwx------ 7 u0_a323 u0_a323 u:object_r:app_data_file:s0:c67,… 3488 … /cesta
# takze `awk '{print $1}'` vratilo prava (`drwx------`), `chcon` to odmitl
# a `2>/dev/null || true` chybu spolklo -> skript hlasil uspech a nedelal nic.
# Hledame proto sloupec podle tvaru, ne podle pozice.
LABEL=$(ls -ldZ "$DIR" 2>/dev/null | tr -s ' ' '\n' | grep -m1 '^u:object_r:')

[ -n "$OWNER" ] || { echo "nepodarilo se zjistit vlastnika $DIR" >&2; exit 1; }
[ -n "$LABEL" ] || { echo "nepodarilo se zjistit SELinux label $DIR" >&2; exit 1; }

echo "fix-app-ownership: $DIR"
echo "  cilovy vlastnik: $OWNER"
echo "  cilovy label:    $LABEL"

n_wrong_owner_before=$(find "$DIR" -xdev -user 0 2>/dev/null | wc -l)
echo "  souboru root:root pred:  $n_wrong_owner_before"

# Pocet spatne olabelovanych. `find -printf '%Z'` pouzit NELZE - sleduje
# symlinky a hlasi label CILE (past c. 13). `ls -ldZ` v davkach ale dela
# lstat, takze u symlinku vidi label odkazu samotneho.
count_wrong_label() {
    find "$DIR" -xdev -exec ls -ldZ {} + 2>/dev/null |
        tr -s ' ' ' ' | cut -d' ' -f5 | grep -c -v -x -F "$LABEL" || true
}
n_wrong_label_before=$(count_wrong_label)
echo "  spatny label pred:       $n_wrong_label_before"

if [ "$APPLY" != 1 ]; then
    echo "(--dry-run, nic se nemeni; --apply pro skutecnou opravu)"
    exit 0
fi

# chown vcetne symlinku (-h), zvlast kvuli past c. 7/11 (absolutni/l2s
# symlinky) - chown bez -h by prepsal vlastnika CILE, ne odkazu
find "$DIR" -xdev -user 0 -exec chown "$OWNER" {} + 2>/dev/null || true
find "$DIR" -xdev -user 0 -exec chown -h "$OWNER" {} + 2>/dev/null || true

# SELinux relabel. `chcon -h` i pro symlinky - bez -h by se olabeloval CIL,
# ne odkaz, a prave na odkazech to vadi nejvic: l2s symlink s labelem
# `app_data_file:s0` BEZ kategorii nejde z app domeny ani lstat-nout, takze
# `dpkg` na nem skonci na "error removing old backup file ...: Permission
# denied" (namereno na kali 2026-10-29).
#
# `restorecon -R` se tu ZAMERNE nepousti: file_contexts na /data/data nema
# per-app kategorie, takze by prave nastavene `c67,…` zase srazil na holé
# `app_data_file:s0` - tedy presne do toho stavu, ktery opravujeme.
chcon_err=0
find "$DIR" -xdev -exec chcon -h "$LABEL" {} + 2>/dev/null || chcon_err=1
[ "$chcon_err" = 0 ] || echo "  POZOR: chcon nekde selhal (nekompletni relabel)"

n_wrong_owner_after=$(find "$DIR" -xdev -user 0 2>/dev/null | wc -l)
n_wrong_label_after=$(count_wrong_label)
echo "  souboru root:root po:    $n_wrong_owner_after"
echo "  spatny label po:         $n_wrong_label_after"
echo "hotovo."
