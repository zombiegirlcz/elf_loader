# gbsh — Ghost/Bionic Shell

Nativní shell pro Android host (bionic, statický combined build). Jeden binárka
obsahuje jak samotný shell, tak `elf_loader`; dispatcher režim automaticky
přepíná mezi shellem a loaderem podle argumentů.

## Koncept
| Typ příkazu | Mechanizmus |
|---|---|
| builtiny | inline v procesu (cd/pwd/echo/export/alias/source/history/...) |
| host (/system/bin toybox apod.) | fork + execvp |
| parrot rootfs ($ROOTFS/{usr/bin,bin,...}) | fork + execve **elf_loader --ownall** |

Shell spouští příkazy z parrot rootfsu přímo, bez nutnosti spouštět
samostatný `elf_loader` binárku. Pro host příkazy používá nativní exec.

## Syntaxe
```
cmd args...                    jednoduchý příkaz
cmd1 | cmd2                    pipeline
cmd > file   cmd >> file       output redirect (podporuje i N> fd formu)
cmd < file                     input redirect
cmd1 && cmd2                   spusť cmd2 jen při úspěchu
cmd1 || cmd2                   spusť cmd2 jen při selhání
cmd1 ; cmd2                    sekvenčně
"quotes" 'quotes'              citování
$VAR ~/                        expanze proměnných a home
```

## Build artefakt
Výsledkem je **jeden statický binárka** `gbsh` (ET_EXEC, ~2.3 MB, zero NEEDED).

| Režim | Kdy se použije |
|---|---|
| **gbsh / shell** | interaktivní režim, `-c`, builtiny, aliasy, history |
| **elf_loader** | `--ownall/--shim/--run/--own/--check/--lazy/--help/--version` |

Build je prováděn na Modal (NDK r28, `aarch64-linux-android24-clang`):
```sh
modal run gbsh_combined_static_build.py
# výstup: /root/elf_loader/files/usr/bin/gbsh
```

> Pozn.: static-pie (`-fPIE -static-pie`) **nefunguje na tomto zařízení**
> (kernel 4.14, RC=139 i u triviálních testů). Používá se proto `-static`
> (non-PIE), což na tomto zařízení běží stabilně.

## Deploy
```sh
# po buildu je binárka už v files/usr/bin/gbsh
chmod 755 /root/elf_loader/files/usr/bin/gbsh

# na device spustit přes ashell (app uid 10310)
ashell -c "$D/usr/bin/gbsh -c 'echo OK'"
```

## Config — ~/.gbshrc
Provádí se při startu stejným parserem jako interaktivní vstup:
```sh
export GBSH_PROMPT='%u@%h:%~ $ '
alias ll='ls -la'
alias ..='cd ..'
```
Prompt proměnné: `%u` user, `%h` hostname, `%~` cwd (~ zkráceně), `%$` #/$.

gbsh načítá config z:
1. `$GBSHRC` (pokud je nastaveno)
2. `$HOME/.gbshrc`
3. `$ROOTFS/root/.gbshrc`
4. `$ROOTFS/etc/gbshrc`

> Pozn.: systémový `/etc/zsh/zshrc` z rootfsu se **ne-načítá** — gbsh není
> zsh a zsh-specifické konstrukce by způsobovaly hluk při startu.

## Test (host i device)
```sh
# lokálně (v prootu jako smoke test)
printf 'echo A && echo B || echo C\nexit 0\n' | gbsh; echo rc=$?

# na device přes ashell
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
ashell -c "ROOTFS=$R $D/usr/bin/gbsh -c 'echo OK'; echo RC=\$?"
```

## Ověřeno na device (2026-09-08)
- builtins + redirecty ✓
- pipeline přes parrot binárky (elf_loader ownall) ✓
- && / || / ; chainy ✓, exit code propagation ✓
- $VAR/~/ expanze, aliasy, history, source ~/.gbshrc ✓
- loader flagy (--help/--version/--check/--ownall) fungují ✓
- statický build ET_EXEC, ~2.3 MB, zero NEEDED ✓
- static-pie varianta **nefunguje** na kernelu 4.14 (RC=139) ✗
