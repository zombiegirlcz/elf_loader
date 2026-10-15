# Prompt 03 — absolutní symlinky → relativní, clamp `..`, rootfd

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(sekce 5 „Rootfs absolutní symlinky nefungují mimo chroot") a
`docs/nextgen/proot-nahrada.md` kapitolu 4.

## Cíl

Absolutní symlinky v rootfs (`/etc/alternatives/awk → /usr/bin/mawk`) jádro
vyhodnocuje vůči host kořeni. Dnes to řeší jen exec (`shim_resolve_symlinks`).

## Úkoly

1. **Konverzní nástroj** `tools/relsymlinks.sh` (nebo podpříkaz loaderu):
   projde `$ROOTFS`, absolutní cíle přepíše na relativní ve „kanonickém tvaru"
   (přesně hloubka × `../` + cesta od kořene). Režim `--dry-run`, log změn,
   možnost vrátit (uložit seznam původních cílů). Přeskočit `/proc`, `/sys`,
   `/dev` a mountpointy hostu uvnitř rootfs (`$R/data`, `$R/system`…).
2. **Shim `symlink`/`symlinkat`:** absolutní cíl zapíše rovnou relativně
   (dpkg, `update-alternatives`, `ln -s /x`). Cíl, který míří do excluded
   host cest (`/system`, `/data`, `/proc`…), nech absolutní.
3. **Shim `readlink`/`readlinkat`:** relativní cíl v kanonickém tvaru vrať jako
   absolutní guest cestu (`update-alternatives` porovnává cíle). Zachovej
   existující výjimky (`/proc/self/exe`).
4. **Clamp `..` nad kořenem** v `shim_translate`: lexikálně normalizovat tak,
   aby `/../etc` nevedlo do `…/distro/etc`.
5. **Volitelně rootfd:** změř (prompt 01), zda `*at(rootfd, rel)` místo prefixu
   `$ROOTFS` něco přinese. Implementuj jen při měřitelném zisku.

## Pozor

- Shimy běží pod guest TP → jen raw syscally a `shim_str*`.
- Nezapomeň na aliasy v GOT tabulce (`nm -D libc.so.6`, stejná adresa).

## Hotovo, když

- Po konverzi fungují bez exec shimu: `cat /etc/alternatives/awk`-style čtení
  přes symlink, `python3` import přes alternatives, `update-alternatives
  --display`, `dpkg -L` u balíku s alternatives.
- A/B test `test-all.sh relsym` (nový absolutní link přes `ln -s` se zapíše
  relativně, `readlink` vrací absolutní tvar, `stat` přes link funguje, `/..`
  zůstane v rootfs).
- Proot s konvertovaným rootfs dál funguje (relativní linky jsou pro něj
  neutrální) — ověř 2–3 příkazy.
