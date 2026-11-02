# elf_loader v Termuxu — co a jak

Návod pro nastavení Termuxu tak, aby se choval jako referenční zařízení:
`proot` je `elf_loader`, glibc binárky z rootfs běží pod ním a starship /
zoxide / fzf se berou **z rootfs**, ne z Termuxu.

> **Rychlá cesta:** `bash termux-setup.sh` (viz [Instalace](#instalace)).
> Co dělá a proč: zbytek tohoto souboru.

## Obsah

- [Co se tím získá](#co-se-tím-získá)
- [Instalace](#instalace)
- [Co skript udělá](#co-skript-udělá)
- [Struktura souborů](#struktura-souborů)
- [Jak to funguje](#jak-to-funguje)
- [Závislosti v rootfs](#závislosti-v-rootfs)
- [Testované kombinace](#testované-kombinace)
- [Časté problémy](#časté-problémy)
- [Odinstalace](#odinstalace)

## Co se tím získá

| před | po |
|---|---|
| `proot -r rootfs cmd` | `proot -r rootfs cmd` — **stejné CLI**, ale bez ptrace |
| `proot-distro login ubuntu` | funguje dál (volá `proot`) |
| `python3`, `gcc`, `git` v Termuxu | nativní bionic, beze změny |
| `python3`, `gcc`, `git` **z rootfs** | `lx python3`, `lx gcc`, … pod loaderem |
| starship/zoxide z Termuxu | **z rootfs** (guest glibc pod loaderem) |

Klíčový rozdíl proti prootu: žádný `ptrace` překlad syscallů, takže `lx` je
~2–3× rychlejší na exec-heavy zátěži a nepotřebuje `--link2symlink` pro
běžné použití. Cena: `mount(2)` je v app doméně `ENOSYS`, takže bindy emuluje
tabulka v loaderu (viz README).

## Instalace

```sh
# 1) loader
pkg install proot-distro            # pokud ještě není
proot-distro install ubuntu         # nebo debian/kali/parrot

# 2) elf_loader — z GitHub Actions artefaktu, nebo si ho zbuildit
#    (viz README → Build). Musí to být NDK build s /system/bin/linker64.
cp elf_loader_ndk "$PREFIX/bin/elf_loader"
chmod 755 "$PREFIX/bin/elf_loader"
"$PREFIX/bin/elf_loader" --version

# 3) setup
bash termux-setup.sh
```

Skript se zeptá na rootfs jen tehdy, když ho sám nenajde. Bez otázek:

```sh
ROOTFS=$PREFIX/var/lib/proot-distro/containers/ubuntu/rootfs \
  bash termux-setup.sh --yes
```

Po doběhnutí:

```sh
exec zsh -l
```

## Co skript udělá

1. **Ověří, že běží v Termuxu** (`PREFIX`). V prootu ani na hostiteli nefunguje.
2. **Najde `elf_loader`** — postupně `$ELF_LOADER`, `~/eltest/elf_loader`,
   `$PREFIX/bin/elf_loader`, `~/usr/bin/elf_loader`, pak `command -v`.
3. **Najde rootfs** — `$ROOTFS`, jinak projde proot-distro layouty
   (`containers/<distro>/rootfs` i starší `installed-rootfs/<distro>`) pro
   ubuntu/debian/kali/parrot/archlinux/alpine a `~/nh/distro/<distro>`.
   Když nic nenajde, **zeptá se na cestu** a ověří, že obsahuje `usr/bin`
   a `etc`. Výsledek se **zapíše do `~/.elfrc`** ve spravovaném bloku.
4. **Doplní `zsh`** (`pkg install zsh`), pokud chybí.
5. **Přejmenuje `proot` → `proot.orig`** a udělá `proot` symlinkem na
   `elf_loader`. Idempotentní: když už symlink je, jen zkontroluje cíl;
   když `proot.orig` existuje, nic nepřepisuje.
6. **Napíše `~/.elfrc`** — **jediná uživatelská vrstva**: aliasy, `mkcd`,
   `fnd`, zoxide, fzf, starship (vše přes `lxwhich`/`lx`, tj. z rootfs),
   přepočet `R`/`LX_PATH` a spravovaný blok
   `ROOTFS`/`LX_DISTRO`/`ELF_LOADER` na začátku.
7. **Napíše `~/.zshrc`** — **jen zsh vrstva**: `setopt`, `bindkey`,
   `compinit`/`zstyle`, pluginy, tři helpery (`reload`/`zshrc`/`elfrc`)
   a jeden `init zsh`, který na konci svého výstupu sám načte `~/.elfrc`.
8. **Napíše `~/.bashrc`** — interaktivní bash `exec zsh -l`; neinteraktivní
   zůstává bash, ale dostane stejnou vrstvu (`init bash` / `. ~/.elfrc`).
9. **Napíše `~/.config/starship.toml`** (`command_timeout = 1000`).
10. **Volitelně nakopíruje rc do guesta** — `$ROOTFS/root/.zshrc`
    a `$ROOTFS/root/.elfrc` (stejné soubory; `.elfrc` pozná guest podle
    chybějícího `$PREFIX` a postaví si vlastní `lx`/`lxwhich`).
11. **Ověří** — `zsh -i`, `lxwhich starship`, `lx /bin/true`,
    `proot -r rootfs /usr/bin/true`.

Všechny existující soubory se zálohují jako `<soubor>.bak.<timestamp>`.

## Struktura souborů

```
$PREFIX/bin/elf_loader      NDK build (bionic, /system/bin/linker64)
$PREFIX/bin/proot           → symlink na elf_loader
$PREFIX/bin/proot.orig      původní proot (kdyby bylo potřeba se vrátit)

~/.elfrc                    JEDINÁ uživatelská vrstva — aliasy, mkcd/fnd,
                            zoxide, fzf, starship, přepočet R/LX_PATH
                            + spravovaný blok ROOTFS/LX_DISTRO/ELF_LOADER
~/.zshrc                    jen zsh vrstva (setopt, bindkey, compinit,
                            pluginy, helpery) + init zsh
~/.bashrc                   interaktivní → exec zsh, jinak bash + init bash
~/.config/starship.toml     command_timeout = 1000
~/.cache/{starship,fzf,zoxide}-init.zsh   generované cache

$ROOTFS/root/.zshrc         guest rc (kopie ~/.zshrc)
$ROOTFS/root/.elfrc         guest rc (kopie ~/.elfrc)
```

## Jak to funguje

### Vrstvy: `.zshrc` je jen zsh, všechno ostatní je v `.elfrc`

Dělení je striktní, aby se konfigurace nezdvojovala:

| soubor | obsah |
|---|---|
| `~/.zshrc` | `setopt`, `bindkey`, `compinit`/`zstyle`, pluginy, helpery, **jeden** `init zsh` |
| `~/.elfrc` | aliasy, `mkcd`, `fnd`, zoxide, fzf, starship, přepočet `R`/`LX_PATH` |
| `~/.bashrc` | interaktivní → `exec zsh -l`, jinak `init bash` |

`.zshrc` **nesourcuje `.elfrc` ručně** — dělá to `init zsh` na konci svého
výstupu. Kdyby to dělal i `.zshrc`, `.elfrc` by se načetl dvakrát a starship
by se inicializoval 2×.

### ⚠️ `.elfrc` NESMÍ volat `init`

`init zsh|bash` **sám na konci svého výstupu sourcuje `~/.elfrc`**. Když by
`.elfrc` zavolal `init`, vznikne rekurze:

```
.elfrc → init → .elfrc → init → … → SIGSEGV
```

Naměřeno: `zsh -i` skončil `rc=139`, v trace je poslední frame `eval $'…'`
z initu. Proto je `init` **jen v `.zshrc`/`.bashrc`**, nikdy v `.elfrc`.

Druhý důsledek: `init` autodetekuje rootfs podle `LX_DISTRO` (default
`parrot`) a spočítá z něj `R`/`LX_PATH`. Teprve **potom** sourcuje `.elfrc`,
kde je správný `ROOTFS`. `.elfrc` proto po přepsání `ROOTFS` musí `R`
a `LX_PATH` přepočítat — jinak `R` ukazuje na neexistující rootfs a `lx`
hlásí `not found in guest rootfs`.

### Dva světy, jeden rc

`.zshrc` i `.elfrc` se načítají **dvakrát** — jednou v Termuxu (bionic zsh),
jednou v guestu (glibc zsh pod loaderem). Proto v nich nesmí být nic, co
platí jen v jednom z nich:

- žádné `/root/...` natvrdo (host `/root` neexistuje, guest `$HOME` je `/root`)
- všechno volitelné za `command -v` / `lxwhich`
- `$PREFIX` je jen v Termuxu, `$ROOTFS` jen v guestu → obojí podmíněné

### `lx` a `lxwhich`

Dodává je `eval "$(elf_loader init zsh)"`:

```sh
lxwhich starship     # → $ROOTFS/usr/bin/starship  (nebo rc=1)
lx starship prompt   # → spustí ji pod loaderem (--ownall)
lx -c 'echo a; id'   # → celá řádka guest shellu (pipy, &&, redirecty)
```

`lx` resolvuje jméno **v rootfs** (`$ROOTFS/usr/bin`, …), nikdy v Termuxu.
Díky tomu `lx python3` spustí guest python, i když Termux má svůj.

### Proč starship/zoxide z rootfs

Termux má vlastní bionic `starship` i `zoxide`. Kdyby je `.elfrc` bral přes
`command -v`, prompt by pocházel z Termuxu a neprošel by loaderem — což je
přesně to, co nechceme (chceme guest glibc toolchain, stejný jako na
zařízení). Proto:

```sh
if lxwhich starship >/dev/null 2>&1; then   # jen rootfs
  starship() { lx starship "$@" ...; }
```

Když v rootfs nejsou, blok se **přeskočí** a prompt jede bez nich. Není to
chyba — je to záměrné pravidlo „z rootfs, ne z Termuxu".

### Prompt cache

`starship prompt` pod loaderem znamená exekuci guest binárky — na každé
překreslení je to drahé. `.elfrc` proto cachuje výsledek na 2 s
(`_STARSHIP_PROMPT_CACHE`, `_STARSHIP_PROMPT_TIME`).

### Generované cache a `sed`

`starship init zsh` i `fzf --zsh` vypisují **absolutní cestu k sobě**
(`$ROOTFS/usr/bin/starship`), ne `$L`. Cache se proto čistí dvěma pravidly:

```sh
sed -e "s|$L|starship|g" -e "s|$R/usr/bin/starship|starship|g"
```

Druhé pravidlo je důležité: bez něj prompt volal binárku **přímo**, mimo
loader, a padal na `no such file or directory` (host cestu v guestu nikdo
nepřeloží).

### Kanonizace cest

`elf_loader init zsh` skládá `ROOTFS` z kandidátů typu
`$HOME/../usr/var/lib/proot-distro/containers/ubuntu`. Kernel to rozřeší, ale
loader porovnává cesty **textově** (prefix `ROOTFS`), takže
`$HOME/../usr/.../rootfs/usr/bin/echo` mu nesedí. `.zshrc` proto po `eval`
dělá:

```sh
ROOTFS=$(cd "$ROOTFS" && pwd -P)
```

A pro proot-distro ≥ 4 navíc `$ROOTFS/rootfs` → `$ROOTFS`, protože `init`
hledá jen `containers/<distro>` (ten adresář existuje, ale `usr/bin` má až
jeho podadresář `rootfs`).

`LX_PATH` se po kanonizaci **přepočítá** — jinak `lxwhich` hledá
v neexistujících cestách.

### `proot` jako symlink

`proot-distro login` i ruční `proot -r ...` volají `proot`. Symlink na
`elf_loader` znamená, že obojí projde loaderem. Loader pozná proot CLI podle
`argv[0]` (basename == `proot`) a přepne se do `--proot-compat`.

Původní proot zůstává jako `proot.orig` — kdykoli se dá vrátit:

```sh
mv -f "$PREFIX/bin/proot.orig" "$PREFIX/bin/proot"
```

## Závislosti v rootfs

`lx` umí spustit jen to, co v rootfs existuje. Pro plný zážitek:

```sh
# nainstaluj uvnitř rootfs (přes loader, s fakerootem kvůli dpkg)
elf_loader --proot-compat -r "$ROOTFS" -0 -- \
  /usr/bin/apt-get install -y zsh starship zoxide fzf ripgrep

# nebo jednodušeji
proot -r "$ROOTFS" -0 /usr/bin/apt-get install -y zsh starship zoxide
```

| nástroj | v Ubuntu 26.04 base | poznámka |
|---|---|---|
| `zsh` | ❌ | `apt install zsh` |
| `starship` | ✅ | `/usr/bin/starship` |
| `zoxide` | ✅ | `/usr/bin/zoxide` |
| `fzf` | ❌ | `apt install fzf` |
| `ripgrep` | ❌ | `apt install ripgrep` (pro `fnd`, `FZF_DEFAULT_COMMAND`) |

Bez nich se nic nerozbije — příslušné bloky se přeskočí.

## Testované kombinace

Naměřeno na **jednom fyzickém zařízení** (Termux + app-files rootfs),
Android 11, kernel:

```
4.14.190-perf-g7da93debc0ee      (aarch64, 4 KB pages)
```

| rootfs | glibc | loader | stav |
|---|---|---|---|
| Parrot Security 7.4 (echo) | 2.41 (`Debian GLIBC 2.41-12+deb13u4`) | 1.0.0 | ✅ plná regresní sada 220 PASS / 2 FAIL / 31 SKIP |
| Kali GNU/Linux Rolling 2026.2 | 2.43 (`Debian GLIBC 2.43-6`) | 1.0.0 | ✅ destruktivní testy (`test-all.sh l2s`, `fakeroot`, `binds`) |
| Ubuntu 26.04.1 LTS (Resolute Raccoon) | 2.43 (`Ubuntu GLIBC 2.43-2ubuntu2.4`) | 1.0.0 | ✅ `uutils coreutils`, `fakeroot -0`, `--proot-compat`, zsh |

Poznámky k měření:

- **Regresní sada** `test-all.sh all` na parrotu: 220 PASS, 2 FAIL
  (`staticexec` — chybí `gcc` v daném běhu; `binds` GC tabulek — časování),
  31 SKIP (nedostupné binárky). Všechny FAIL jsou nesouvisející s loaderem.
- **Ubuntu 26.04** je jediná kombinace, kde se projevil `uutils coreutils`
  (`AT_EXECFN` identita) — opraveno v `1.0.0`, viz AGENTS.md past 33.
- **Kali** se používá na destruktivní testy (`--ownall`/`--shim` nad
  adresářem, `IMGCACHE`), protože ho nikdo nepoužívá. Parrot je živý
  a v testech jen read-only.
- **glibc 2.42** je odvozená (layout `_rtld_global` se bere z 2.43),
  neověřená. Při další verzi glibc znovu změřit.

## Časté problémy

### `lx: 'X' not found in guest rootfs`

Buď v rootfs není (`apt install X`), nebo je `ROOTFS` nekanonizovaný.
Zkontroluj:

```sh
echo "$ROOTFS"; ls "$ROOTFS/usr/bin/X"
zsh -i -c 'echo $LX_PATH' | tr ' ' '\n' | while read d; do ls -d "$d" 2>/dev/null; done
```

### Prompt je prázdný nebo `%`

Cache se nevygenerovala. Smaž ji a nech znovu vzniknout:

```sh
rm -f ~/.cache/starship-init.zsh ~/.cache/fzf-init.zsh ~/.cache/zoxide-init.zsh
exec zsh -l
```

Když ani potom nic, `lxwhich starship` v rootfs nic nenajde → nainstaluj
starship v rootfs (viz [Závislosti](#závislosti-v-rootfs)).

### Prompt je prázdný a v `~/.cache` je soubor `starship-init.`

Chybí `$_LX_SHELL`. Cache se pak uloží jako `starship-init.` (prázdná
přípona) místo `starship-init.zsh`, načte se 0 bajtů a `PROMPT` zůstane
`%# `. Kontrola:

```sh
ls ~/.cache/*-init.*        # nesmí být soubor končící tečkou
```

`.elfrc` detekuje shell na začátku (`ZSH_VERSION` → `zsh`, jinak `bash`).

### `no such file or directory: .../rootfs/usr/bin/starship`

Cache obsahuje absolutní cestu, kterou guest neumí přeložit. Znamená to, že
`sed` pravidlo v `.elfrc` chybí nebo je stará cache. Řešení: smazat cache
(viz výše) a zkontrolovat, že `.elfrc` má **obě** `sed -e` pravidla.

### `proot: command not found`

Symlink chybí:

```sh
ls -la "$PREFIX/bin/proot" "$PREFIX/bin/proot.orig"
ln -sf "$PREFIX/bin/elf_loader" "$PREFIX/bin/proot"
```

### `lx /bin/echo` hlásí „not found", ale soubor existuje

`ROOTFS` není kanonizovaný (obsahuje `..`) nebo ukazuje na
`containers/<distro>` místo `containers/<distro>/rootfs`. Ověř
`echo "$ROOTFS"` — musí to být reálná cesta bez `..`.

### `unset LD_PRELOAD` v testech

Termux má `LD_PRELOAD=libtermux-exec-ld-preload.so`. Při ručním testování
loaderu ho vypni, jinak se exec chová jinak:

```sh
( unset LD_PRELOAD; lx /bin/true )
```

`.zshrc` to nedělá — v interaktivním shellu je `LD_PRELOAD` potřeba pro
Termux exec.

### `su`/`sudo` pod loaderem

Nefunguje a fungovat nebude — `NO_NEW_PRIVS` je nevratný a loader own-loaduje
(jádro nevidí setuid bit). `su_wrapper` navíc potřebuje Root Bridge. Není to
bug loaderu; viz README → Known issues.

## Odinstalace

```sh
# vrátit proot
mv -f "$PREFIX/bin/proot.orig" "$PREFIX/bin/proot"

# odstranit rc soubory (zálohy zůstanou jako .bak.*)
rm -f ~/.zshrc ~/.bashrc ~/.elfrc ~/.config/starship.toml
rm -f ~/.cache/starship-init.zsh ~/.cache/fzf-init.zsh ~/.cache/zoxide-init.zsh

# volitelně guest rc
rm -f "$ROOTFS/root/.zshrc" "$ROOTFS/root/.config/starship.toml"

# loader
rm -f "$PREFIX/bin/elf_loader"
```

Rootfs ani Termux balíčky se nemažou — skript do nich nezasahuje kromě
volitelného guest rc a `zsh`.
