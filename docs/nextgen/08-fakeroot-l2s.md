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
