#!/bin/sh
# gbsh-shim-gen.sh — vygeneruje symlink farmu, ktera kazdou externi binarku
# z rootfs spusti pres `elf_loader --ownall`. Cil: v shellu staci psat
# "ls -la /etc" misto "elf ls -la /etc"; builtiny (cd/export/echo) zustavaji
# nedotcene (jsou v shellu, ne na PATH).
#
# Pouziti:
#   ROOTFS=/cesta/k/rootfs sh tools/gbsh-shim-gen.sh
#   PATH pak nastav na $GBSH_SHIM_DIR (viz .gbshrc).
#
# Env:
#   ROOTFS         (povinne) cesta k rootfs
#   GBSH_SHIM_DIR  kam generovat (default $HOME/.gbsh-shim)
#   ELF_LOADER     cesta k loaderu (default: elf_loader z PATH / ROOTFS/../usr/bin)
set -e

: "${ROOTFS:?ROOTFS musi byt nastaven}"
SHIM_DIR="${GBSH_SHIM_DIR:-$HOME/.gbsh-shim}"

# Najdi elf_loader: $ELF_LOADER -> $ROOTFS/../usr/bin -> PATH
if [ -z "$ELF_LOADER" ]; then
    for c in "$ROOTFS/../usr/bin/elf_loader" "/data/user/0/com.linux_core/files/usr/bin/elf_loader"; do
        [ -x "$c" ] && { ELF_LOADER="$c"; break; }
    done
fi
[ -z "$ELF_LOADER" ] && ELF_LOADER="$(command -v elf_loader || true)"
[ -n "$ELF_LOADER" ] || { echo "gbsh-shim: elf_loader nenalezen" >&2; exit 1; }

mkdir -p "$SHIM_DIR"

# Dispatch skript: prevezme basename sveho jmena (pres $0 symlinku) a spusti
# odpovidajici binarku z rootfs pres elf_loader --ownall. Cesty jsou zapsane
# absolutne, aby dispatch fungoval i bez ROOTFS/ELF_LOADER v prostredi.
cat > "$SHIM_DIR/dispatch" <<EOF
#!/bin/sh
# vygenerovano gbsh-shim-gen.sh — needituj rucne
exec "$ELF_LOADER" --ownall "$ROOTFS/usr/bin/\$(basename "\$0")" "\$@"
EOF
chmod +x "$SHIM_DIR/dispatch"

# Sesbirej binarky z usr/bin i bin. POZOR: $ROOTFS/bin byva symlink na usr/bin
# (usrmerge) -> jinak by se kazda binarka pocitala 2x. Deduplikujeme podle
# jmena. Zaroven si pamatujeme seznam aktualnich jmen, abychom na konci
# uklidili zastarale symlinky (binarka smazana z rootfs).
count=0
seen=""
for dir in "$ROOTFS/usr/bin" "$ROOTFS/bin"; do
    [ -d "$dir" ] || continue
    for bin in "$dir"/*; do
        [ -f "$bin" ] || continue
        [ -x "$bin" ] || continue
        name=$(basename "$bin")
        # dedup: uz jsme tuhle binarku videli?
        case " $seen " in *" $name "*) continue ;; esac
        seen="$seen $name"
        ln -sf "$SHIM_DIR/dispatch" "$SHIM_DIR/$name"
        count=$((count + 1))
    done
done

# Uklid: symlinky na dispatch, ktere uz nemaji odpovidajici binarku v rootfs
# (uzivatel ji smazal), odstran. Jinak by v PATH zustavaly "ducha" prikazy.
removed=0
for link in "$SHIM_DIR"/*; do
    [ -L "$link" ] || continue
    lname=$(basename "$link")
    # zpetna kompatibilita s puvodnim jmenem promenne
    lcase=$lname
    case " $seen " in *" $lcase "*) continue ;; esac
    rm -f "$link"
    removed=$((removed + 1))
done

echo "gbsh-shim: vygenerovano $count symlinku do $SHIM_DIR (odstraneno $removed zastaralych, loader=$ELF_LOADER)"