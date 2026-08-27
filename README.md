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
                                     #         pro non-root shell pouzij gbsh
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
