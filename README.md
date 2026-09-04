# elf_loader + gbsh + elroot

Spouštěč glibc/parrot binárek na Androidu (aarch64) **bez prootu a bez qemu** —
vlastní „own-loading" loader + nativní bionic shell (`gbsh`) + PRoot-like
launcher (`elroot`). Kompatibilní s jakoukoli aplikací a jakýmkoli rootfs:
**vše se bere z proměnných prostředí, žádné hardcoded cesty.**

> Podrobné vývojové poznámky (certifikace, seccomp emulace, historie) jsou v
> [`postup.md`](postup.md) — ten soubor záměrně zůstává jako deník.

## Rychlý start (Quick Start)

```bash
# 1. Nastavení rootfs (Parrot GNU/Linux)
export ROOTFS=/data/user/0/com.linux_core/files/nh/distro/parrot

# 2. Spuštění binárky z guest rootfs
elf_loader --ownall "$ROOTFS/bin/ls" -la

# 3. Nebo interaktivní shell
gbsh
```

> **Tip:** Všechny příkazy mají `--help` pro nápovědu:
> `elf_loader --help` · `gbsh --help` · `elroot --help`

## Co je v repo
| soubor | účel |
|---|---|
| `src/elf_loader.c`, `src/main.c`, `src/entry.S`, `include/elf_loader.h` | vlastní loader (načte/relokuje/spustí ELF64) |
| `gbsh/gbsh.c` | nativní interaktivní shell (bionic) s vlastním line editorem |
| `tools/elroot.sh` | PRoot-like launcher nad elf_loader + gbsh |
| `elf_loader`, `gbsh` | předpřipravené binárky |
| `finale_loader_build.py`, `gbsh_build.py`, `Makefile` | build přes Modal (NDK) |
| `magisk-module/` | Magisk modul (univerzální detekce rootfs) |

## elf_loader — přímé použití

### Základní příkazy

```bash
# Introspekce ELF souboru (ukáže base, entry point, symboly)
elf_loader /bin/ls

# Spuštění programu s argumenty
elf_loader --run /bin/ls -la /data

# Spuštění s vlastními knihovnami
elf_loader --own /bin/myprog /path/to/libfoo.so

# Spuštění se všemi závislostmi z guest rootfs (doporučeno)
elf_loader --ownall "$ROOTFS/bin/bash"

# Spuštění s F2 path-translation shimem (non-root, bez namespaces)
elf_loader --shim "$ROOTFS/bin/ls"

# Lazy PLT binding (rychlejší start, pozdější resoluce)
elf_loader --lazy --run /bin/grep -r .

# Nápověda
elf_loader --help
elf_loader -h
```

### Volby a přepínače

| Přepínač | Popis |
|---|---|
| `<elf_binary>` | Introspekce ELF souboru (bez spuštění) |
| `--run <elf> [args...]` | Spustí ELF binárku s argumenty |
| `--own <elf> <shared.so> [args...]` | Spustí s konkrétní sdílenou knihovnou |
| `--ownall <elf> [args...]` | Spustí se všemi závislostmi z guest rootfs |
| `--shim <elf> [args...]` | Spustí s F2 path-translation shimem (non-root) |
| `--lazy` | Povolí lazy PLT binding (před `--run`) |
| `--help, -h` | Zobrazí tuto nápovědu |

### Proměnné prostředí (elf_loader)

| Proměnná | Význam | Výchozí |
|---|---|---|
| `ROOTFS` | Cesta k guest rootfs (Parrot) | — |
| `ELF_LOADER` | Cesta k elf_loader binárce | `$ROOTFS/../usr/bin/elf_loader` |
| `ELF_DEBUG` | Úroveň debugu: 0=NONE, 1=ERROR, 2=WARN, 3=INFO, 4=DEBUG, 5=VERBOSE | 0 |
| `ELF_LOADER_DUMP_MAPS` | Vypíše memory mapy po spuštění | — |
| `ELF_LOADER_SKIP_RELOC` | Přeskočí relokace (ladění) | — |
| `F2_FILTER=1` | Zapne F2 seccomp path filter | — |
| `F2_ONLY=seznam` | Čárkami oddělený seznam F2 hooků | — |
| `F2_DISABLE` | Vypne F2 path-translation shim | — |

### Příklady použití

```bash
# Základní introspekce
elf_loader /bin/ls

# Spuštění s argumenty
elf_loader --run /bin/ls -la /data

# Spuštění bash se všemi závislostmi
elf_loader --ownall "$ROOTFS/bin/bash"

# F2 shim pro non-root běh
elf_loader --shim "$ROOTFS/bin/ls"

# Lazy binding pro rychlejší start
elf_loader --lazy --run /bin/grep -r "pattern" /etc

# Debug výstup
ELF_DEBUG=4 elf_loader --run /bin/cat /etc/issue

# S vlastním rootfs
ROOTFS=/data/parrot elf_loader --ownall /bin/zsh

# Dump memory map
ELF_LOADER_DUMP_MAPS=1 elf_loader --run /bin/ls
```

## gbsh (interaktivní shell)

Nativní bionic shell s vlastním line editorem — **nevyžaduje root**.

```bash
# Spuštění interaktivního shellu
export ROOTFS=/cesta/k/distro
gbsh
```

**Vlastnosti:**
- Zalamování dlouhých řádků, detekce šířky terminálu (`TIOCGWINSZ`)
- Bracketed paste, Ctrl-A/E/K/W, barevný výstup, syntax highlighting
- Dual-world navigace (`cd ..` z `/` překlopí na host), `--chroot` režim

Pro **plný zsh zážitek** (ZLE/completion/barvy) stačí `elroot zsh` (parrot zsh 5.9 běží pod loaderem).

## elroot — PRoot-like launcher

Automatický výběr režimu podle dostupnosti root:

```bash
export ROOTFS=/cesta/k/distro
export ELF_LOADER="$ROOTFS/../usr/bin/elf_loader"
export GBSH="$ROOTFS/../usr/bin/gbsh"

# Auto: ROOT -> chroot, NON-ROOT -> elf_loader --ownall
elroot <prikaz> [args]

# Vynutit root chroot (plný fs, žádný seccomp)
elroot --chroot <prikaz>

# Vynutit non-root (elf_loader --ownall)
elroot --ownall <prikaz>

# Interaktivní shelly
elroot zsh                    # ROOT -> plný zsh 5.9 (ZLE/completion/barvy)
elroot --chroot zsh           # ROOT -> zsh v chrootu
elroot --ownall zsh           # NON-ROOT -> zsh bez modulů (pro non-root použij gbsh)

# Spuštění gbsh přímo (bionic non-root shell)
ROOTFS=/cesta/k/distro $ROOTFS/../usr/bin/gbsh
```

## Build

Přes Modal (NDK r28):
```bash
modal run finale_loader_build.py     # -> /tmp/elf_loader
modal run gbsh_build.py             # -> /tmp/gbsh (+ /tmp/gbsh_static)

# Nasazení na zařízení:
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