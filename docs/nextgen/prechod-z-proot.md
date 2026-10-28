# Přechod z proot na elf_loader (prompt 09)

Krátký návod pro appku (`ProotManager.kt`): jak vyměnit proot za loader
beze změny volajícího kódu, a kdy to (ještě) nejde.

## 1. Záměna binárky

Appka dnes spouští proot nějak takto (zjištěno z `/proc/*/cmdline`
běžících instancí a env):

```
proot -r <ROOTFS> -b <host>:<guest> [-b ...] -w <cwd> [-0] [-l] <cmd> [args...]
```

Nahraď binárku za `elf_loader` a buď:

- přidej `--proot-compat` před zbytek argumentů, **nebo**
- udělej symlink/kopii `elf_loader` pojmenovanou `proot` — loader detekuje
  `argv[0] == "proot"` a chová se stejně, jako by `--proot-compat` dostal.

Obě varianty parsují stejnou syntaxi. Žádná jiná změna appky není potřeba.

## 2. Co je podporováno

| proot volba | efekt pod loaderem |
|---|---|
| `-r/--rootfs <dir>` | `ROOTFS=<dir>` (povinné — bez něj se vždy spustí reálný proot) |
| `-b/--bind <host[:guest]>` | `LX_BINDS` (viz `docs/nextgen/06-mount-tabulka.md`) |
| `-w/--cwd/--pwd <dir>` | `chdir($ROOTFS/<dir>)` před spuštěním |
| `-0/--root-id` | `LX_FAKEROOT=1` (viz `08-fakeroot-l2s.md`) |
| `-l/--link2symlink` | `LX_L2S=1` |
| `-k/--kernel-release <ver>` | spolknuto, žádná emulace (loader kernel-release nehlásí jinak) |
| `--kill-on-exit` | no-op (životnost dítěte == loader proces) |
| bez příkazu | `$SHELL` (nebo `/bin/bash`) v rootu, interaktivní shell |

Neznámá volba (`-S`, `-v`, `-V`, `--sysvipc`, …) **nikdy** neskončí chybou
appky — celé původní `argv` se beze změny předá skutečnému proot
(`LX_PROOT_BIN`, default hledá v běžných instalačních cestách a `$PATH`).

## 3. Hybridní dispatch (binárky, které loader neumí)

Uvnitř běžící session (`exec`/`execve`) loader sám pozná, co neumí
own-loadovat, a přesměruje to na skutečný proot se **stejnými bindy**
(sebranými ze sdílené tabulky, `06-mount-tabulka.md`):

- binárka s cizí `e_machine` (ne `aarch64`) — pokud je v
  `$ROOTFS/usr/bin/qemu-<arch>-static`, proot ho dostane přes `-q`;
- basename uvedený v `$ROOTFS/etc/lx-force-proot` (jeden název na řádek);
- statická nativní (aarch64) binárka **jen** s `ELF_LOADER_STATIC_PROOT=1`
  (default vypnuto — bionické testovací nástroje v rootfs, např. `ashell`,
  `su_wrapper`, mají zůstat na přímém real execve).

Glibc dynamické binárky jdou pod loader (own-loading) jako dřív.

## 4. Co (ještě) nejde

- `posix_spawn`/`posix_spawnp` cesta (uv/python detekce) hybridní dispatch
  na proot nemá — `proot_compat_exec_real` dělá `execve` (process replace),
  ne fork+exec, který `posix_spawn` potřebuje. V praxi vzácné (jen glibc
  detekce přes `ld-linux --version`), binárky bez interpretru tam dál jdou
  na přímý real exec jako dřív.
- `-k/--kernel-release` se jen spolkne, ne emuluje.
- Appka musí mít `LX_PROOT_BIN` nastavené (nebo proot musí ležet v jedné
  z výchozích cest), jinak fallback na neznámé volby selže na `execve`.

## 5. Ověření

```
test-all.sh prootcompat   # CLI parse, -0/id, argv[0]==proot, fallback bez pádu
test-all.sh all           # regrese zbytku
```

Destruktivní/`lx-force-proot` testy dělej na kali (`AGENTS.md` sekce
"Bezpečnost"), ne na živém parrotu.
