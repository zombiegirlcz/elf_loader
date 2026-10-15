# Prompt 06 — uživatelská tabulka mountů + `mount --bind` v guestu

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(sekce 5 — past se sdíleným rc a `mount(2)` = ENOSYS) a
`docs/nextgen/proot-nahrada.md` kapitolu 3.1.

## Motivace

Uživatel zkoušel `mount --bind /root /home/deamon` — v app doméně je `mount(2)`
ENOSYS (seccomp), složka zůstala prázdná. `shim_excluded()` (`src/main.c:343`)
je ve skutečnosti natvrdo zapsaná tabulka bindů.

## Úkoly

1. Nahradit `shim_excluded` longest-prefix tabulkou `guest prefix → host cesta
   [ro]`. Výchozí obsah = dnešní seznam (identita host→host). Konfigurace:
   `$ROOTFS/etc/lx-binds` (formát jako `proot -b host:guest`) + env `LX_BINDS`.
2. Překlad: najdi nejdelší shodný prefix (na hranici komponenty), jinak
   `$ROOTFS` prefix. Před překladem lexikálně normalizovat `.`/`..` (`..` přes
   hranici mountu se vrací do guest rodiče).
3. Zpětné mapování host→guest pro `getcwd`, `readlink(at)`, `/proc/self/cwd`.
4. `ro` binds: zapisující operace → `EROFS`.
5. **Shim `mount`:** `MS_BIND` (i `MS_BIND|MS_REC`) přidá záznam do tabulky,
   `umount`/`umount2` ho odebere. Sdílení s potomky: tabulka v souboru pod
   `$LX_LOG` (nebo memfd, fd v env) + generační čítač; každý proces při změně
   čítače tabulku znovu načte. Ostatní typy mountů → dál ENOSYS.
6. Bez mallocu v shim cestě (guest TP): pevná tabulka, např. 64 položek.

## Hotovo, když

- V guestu: `mount --bind /root /home/deamon && ls /home/deamon` ukáže obsah
  `/root`; `umount /home/deamon` vrátí prázdnou složku; nový proces (potomek
  i sourozenec ze stejné session) bind vidí.
- `cat /proc/mounts` (pokud prompt 07 hotový) bind ukazuje.
- A/B test `test-all.sh binds`; `all` bez regrese (dnešní excluded chování
  identické).
