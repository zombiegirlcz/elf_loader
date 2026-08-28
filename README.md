# elf_loader + gbsh + elroot

Spouštěč glibc/parrot binárek na Androidu (aarch64) **bez prootu a bez qemu** —
vlastní „own-loading" loader + nativní bionic shell (`gbsh`) + PRoot-like
launcher (`elroot`). Kompatibilní s jakoukoli aplikací a jakýmkoli rootfs:
**vše se bere z proměnných prostředí, žádné hardcoded cesty.**

> Podrobné vývojové poznámky (certifikace, seccomp emulace, historie) jsou v
> [`postup.md`](postup.md) — ten soubor záměrně zůstává jako deník.

## Co je v repo
| soubor | účel |
|---|---|
| `src/elf_loader.c`, `src/main.c`, `src/entry.S`, `include/elf_loader.h` | vlastní loader (načte/relokuje/spustí ELF64) |
| `gbsh/gbsh.c` | nativní interaktivní shell (bionic) s vlastním line editorem |
| `tools/elroot.sh` | PRoot-like launcher nad elf_loader + gbsh |
| `elf_loader`, `gbsh` | předpřipravené binárky |
| `finale_loader_build.py`, `gbsh_build.py`, `Makefile` | build přes Modal (NDK) |
| `magisk-module/` | Magisk modul (univerzální detekce rootfs) |

## Spuštění (univerzální — přes ENV)

Všechny cesty se předávají přes prostředí, takže to může použít kdokoliv,
kdekoliv. Minimální nastavení pro launcher (tvoje „custom launcher" / .rc):

```sh
export ROOTFS=/cesta/k/distro        # např. /data/.../nh/distro/parrot
export ELF_LOADER="$ROOTFS/../usr/bin/elf_loader"   # volitelné, má výchozí
export GBSH="$ROOTFS/../usr/bin/gbsh"               # volitelné, má výchozí
export SU="${SU:-/product/bin/su}"                  # volitelné
```

Pak:
```sh
elroot <prikaz> [args]               # AUTO: je-li su dostupné (ROOT), spustí přes chroot (ROOT);
                                     #        jinak elf_loader --ownall (NON-ROOT)
elroot --chroot <prikaz>             # vynutit root chroot (plný fs, žádný seccomp)
elroot --ownall <prikaz>             # vynutit non-root (elf_loader --ownall)
elroot zsh                            # ROOT (auto) -> plny interaktivni zsh 5.9 (ZLE/completion/barvy)
elroot --chroot zsh                  # ROOT -> zsh v chrootu (plny zsh)
elroot --ownall zsh                   # POZOR: non-root -> zsh nenajde moduly (ZLE nefunguje),
                                     #         pro non-root shell spust gbsh PRIMO (je nativni)
# gbsh se spousti PRIMO (bionicky non-root shell, deploynuty vedle rootfs):
#   ROOTFS=/cesta/k/distro $ROOTFS/../usr/bin/gbsh      # interaktivni non-root shell
```

Přímo přes loader (bez elrootu):
```sh
export ROOTFS=/cesta/k/distro
elf_loader --ownall "$ROOTFS/bin/ls" -l     # spustí parrot ls
gbsh                                     # interaktivní shell (vyžaduje ROOTFS)
```

### Proměnné prostředí
| proměnná | význam | výchozí |
|---|---|---|
| `ROOTFS` | cesta k distro rootfs (povinná pro gbsh) | — (gbsh skončí s chybou, pokud není) |
| `ELF_LOADER` | cesta k elf_loader binárce | `$ROOTFS/../usr/bin/elf_loader`, pak `/system/bin/elf_loader` |
| `GBSH` | cesta k gbsh | `$ROOTFS/../usr/bin/gbsh`, pak `/system/bin/gbsh` |
| `SU` | cesta k `su` (root detekce) | `/product/bin/su` |
| `TERMINFO` | terminfo DB (pro barvy v terminálu) | elroot nastaví na `$ROOTFS/usr/share/terminfo` |
| `GBSHRC` | gbsh config (místo `~/.gbshrc`) | — |
| `GBSH_PROMPT` / `GBSH_PROMPT_MODE` | vzhled/prompt gbsh | výchozí |

## gbsh (interaktivní shell)
Vlastní line editor (nezávislý na zsh/bash):
- zalamování dlouhých řádků bez duplicit (save/restore kurzoru + smazání oblasti),
- detekce šířky terminálu přes `TIOCGWINSZ` (fallback `$COLUMNS`/80),
- bracketed paste (`\x1b[200~`/`201~`) — víceřádkový vklad,
- Ctrl-A/E (začátek/konec), Ctrl-K (smazat do konce), Ctrl-W (slovo),
- barvy: `ls`/`grep`/`diff` `--color=auto`, syntax highlighting vstupu,
- dual-world navigace (`cd ..` z `/` překlopí na host), `--chroot` režim.

Pro **plný zsh zážitek** stačí `elroot zsh` (parrot zsh 5.9 běží pod loaderem).

## Build
Přes Modal (NDK r28):
```sh
modal run finale_loader_build.py     # -> /tmp/elf_loader
modal run gbsh_build.py             # -> /tmp/gbsh (+ /tmp/gbsh_static)
# nasazení na zařízení:
./tools/push_bin.sh /tmp/elf_loader /cesta/usr/bin/elf_loader 755
./tools/push_bin.sh /tmp/gbsh       /cesta/usr/bin/gbsh       755
```
Lokálně (vyžaduje NDK): `make`.

## Magisk modul
`magisk-module/` se instaluje do `/data/adb/modules/…`. Rootfs detekuje
**univerzálně** (skenuje `/data/user/0/*/files`, `/data/data/*/files`,
`/data/adb/*/files` po `nh/distro/parrot`) — žádné jméno aplikace není
natvrdo. Cestu uloží do `/data/adb/parrot_root`.

## Co umí loader (stručně)
Načte ELF64 PT_LOAD, vyřeší DT_NEEDED přes vlastní scope, aplikuje relokace
(vč. `R_AARCH64_IRELATIVE`/ifunc), sestaví stack + auxv, skočí na entry.
Vlastní module loader (`--own`) pro glibc .so bez `dlopen`. Signály blokované
seccompem (non-root) se emulují v SIGSYS handleru (setfsuid/setpriority/NUMA/
keyring/syslog/IPC/futex_waitv výjimkou). Viz `postup.md`.

## F2 — path-translation shim (`--shim`)
Alternativa k chrootu pro běh glibc binárek **non-root** bez namespaců:
loader načte host binárku (bionic) i guest glibc rootfs, při JUMP_SLOT /
import resolvenutí dává F2 override (vlastní `open`/`open64`/`openat`/
`openat64`/`statx`/`fstatat`/`symlink`/`rename`/`unlink`/`mkdir`/`rmdir` shimy)
přednost před host symbolem. Shimy překládají absolutní `/…` cesty na
`$ROOTFS/…` a volají původní glibc funkci přes `g_orig_*`.

**Seccomp proot-lite (syscall-level path translation):** kromě PLT/GOT override
instaluje F2 před `elf_load` seccomp filtr, který pro path-syscally
(`openat=56`, `statx=291`, `newfstatat=79`, `readlinkat=78`, `faccessat=48`)
vrací `SECCOMP_RET_TRAP`. SIGSYS handler přeloží cestu (x1) na `$ROOTFS/…` a
**vyřeší symlinkové řetězy** (proot-lite `f2_realpath`: `newfstatat`+
`readlinkat` smyčka, absolutní cíle přeložené na `$ROOTFS`), pak zemuluje
syscall raw `svc #0` s `F2_SENTINEL` v `x5` (filtr jej pustí → zabrání
zacyklení). To chytí i glibc IFUNC-resolved `open64`/`__openat64`, jejichž
GOT je předplněný při loadu a PLT-override je nechytí (viz historie níže).
Handler běží v parrot TLS → používá jen ruční `sys_write`/`raw_syscall6`.

**Status (testováno na device, 23+ příkazů, `maps-begin=0` = žádný pád):**
- ✅ Funguje: `cat head wc ls stat find realpath dirname basename
  sed sort awk mawk grep cut tr uniq python3 --version apt apt-get ldconfig`.
- ✅ Symlinkové řetězy se řeší: `awk` je `/usr/bin/awk → /etc/alternatives/
  awk → /usr/bin/gawk`; `ls -l` správně ukazuje `→` šipky.
- ✅ `ldconfig`/`dpkg` symlink/rename hooky (žádné `Can't link`).
- ⚠️ `statx` má `AT_SYMLINK_NOFOLLOW` v `x2` (ne `x3`) → handler čte flagy dle
  syscallu (openat/statx=`a2`, newfstatat/faccessat=`a3`), jinak by `lstat`
  nechtěně vyřešil symlink.
- ⚠️ Exclude list (host fs, který se NEPŘEKLÁDÁ) je `/proc /sys /dev /system
  /apex /vendor /product /odm /mnt /metadata` — **`/data` tam není**, protože
  `$ROOTFS` žije pod `/data/…`; kdyby v exclude byl, ROOTFS cesta by se
  shodovala s `/data`+`/` a `f2_realpath` by ji vyloučil → symlinky pod
  ROOTFS by se nerozvinuly (ENOENT, např. právě `awk`).
- Build: `modal run finale_loader_build.py` (flagy `-O0 -g`, bez `-Werror`).

Spuštění přes `elroot --shim <cmd>` (viz `tools/elroot.sh`).
