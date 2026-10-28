# Prompt 08 — fake root + link2symlink (apt/dpkg bez proot)

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(setuid shimy v sekci 5 dw; hardlinky zakázané SELinuxem v sekci 10;
`/dev/shm` link → `renameat2`) a `docs/nextgen/proot-nahrada.md` kapitoly
3.2, 3.3.

## Úkoly

1. **Fake root** (za `LX_FAKEROOT=1`, ekvivalent `proot -0`):
   - `getuid/geteuid/getgid/getegid/getresuid/getresgid/getgroups` → 0;
   - `stat`/`statx`/`fstat`: vlastník = app uid → 0 (a podle DB níže);
   - `chown/lchown/fchown/fchownat`, `chmod` setuid bitů → úspěch + zápis do DB;
   - `mknod/mknodat` zařízení → obyčejný prázdný soubor + záznam typu a
     major/minor; `stat` pak hlásí `S_IFCHR`/`S_IFBLK`;
   - DB: nejdřív zjisti, zda f2fs na `/data` povoluje `user.*` xattry pro app
     soubory (`setfattr`). Pokud ano, ukládat do xattru; jinak sidecar soubor
     klíčovaný `(dev, ino)` (formát jako fakeroot `faked` save file), mmap
     `MAP_SHARED`, zámek přes `flock` (raw).
   - Při `unlink`/`rename` DB udržovat konzistentní (ino reuse!).
2. **link2symlink** (ekvivalent `proot -l`): `link()`/`linkat()` → originál se
   přesune do `$ROOTFS/.l2s/<id>`, obě jména se stanou symlinky na něj,
   počítadlo odkazů v `.l2s/<id>.nlink`; `unlink` počítadlo snižuje a při 0
   smaže; `stat` hlásí `st_nlink` z počítadla. Zachovat dnešní `/dev/shm`
   speciální případ. Zjisti, jakou konvenci používá `PROOT_L2S_DIR` u proot,
   a pokud jde, buď kompatibilní (aby rootfs fungoval pod oběma).

## Hotovo, když

- Pod loaderem s `LX_FAKEROOT=1`: `apt-get install --reinstall` malého balíku
  projde, `dpkg -V` je čistý, `ls -l` ukazuje `root root`, `ln a b && ls -li`
  ukazuje 2 odkazy, `rm a` nechá `b` čitelné.
- A/B testy `test-all.sh fakeroot`, `test-all.sh l2s`; `all` bez regrese.
- Výsledek apt benchmarku (prompt 01) proti proot.

## Stav (ověřeno 2026-10-28)

**Hotovo a funkční**, default vypnuto (`LX_FAKEROOT=1 LX_L2S=1`). Ověřeno
znovu živě:

- `ls -l`/`stat` hlásí `root root` na app-uid souborech; `ln a.txt b.txt &&
  ls -li` ukazuje stejný inode + `nlink=2` (l2s), `rm a.txt` nechá `b.txt`
  čitelné (test proveden ve scratch adresáři na parrot rootfs, bezpečné).
- `apt-get install --reinstall` na **kali** (ne na živém parrot — viz
  bezpečnostní pravidlo v `AGENTS.md`): s čistým `apt-get` padá na
  `setegid/seteuid … Operation not permitted` — apt chce reálně přepnout na
  sandbox uživatele `_apt`, což app-uid proces nemůže (real kernel limit,
  netýká se fakeroot/l2s emulace). Workaround `-o
  APT::Sandbox::User=root` obejde sandbox a instalace **projde až k
  rozbalení** (binárka nahrazena, `root root`, obsah správný) — ale narazí
  na samostatný problém: `dpkg: error: error removing old backup file
  '/var/lib/dpkg/status-old': Permission denied` (ten soubor/symlink
  zůstal z dřívější operace jako skutečný root, uid 0 ≠ app uid — reálný
  permission wall v rootfs, ne bug loaderu).
- `dpkg -V nano` čisté (žádný výstup) i po částečném běhu.

**Shrnutí:** mechanismus (fake ownership, hardlinky přes `.l2s`) je hotový
a funguje přesně, jak má. `apt-get install --reinstall` jako celá
end-to-end operace potřebuje navíc `APT::Sandbox::User=root` a čisté
`var/lib/dpkg/` (bez reziduí reálného rootu) — to je stav konkrétního
rootfs, ne feature k dopsání.
