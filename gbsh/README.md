# gbsh — Ghost/Bionic Shell

Nativní shell pro Android host (bionic, NDK build). Běží přímo bez loaderu;
parrot rootfs příkazy automaticky routuje přes elf_loader own-loading.

## Koncept
| Typ příkazu | Mechanizmus |
|---|---|
| builtiny | inline v procesu (cd/pwd/echo/export/alias/source/history/...) |
| host (/system/bin toybox apod.) | fork + execvp |
| parrot rootfs ($ROOTFS/{usr/bin,bin,...}) | fork + execve **elf_loader --ownall** |

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

## Config — ~/.gbshrc
Provádí se při startu stejným parserem jako interaktivní vstup:
```sh
export GBSH_PROMPT='%u@%h:%~ $ '
alias ll='ls -la'
alias ..='cd ..'
```
Prompt proměnné: `%u` user, `%h` hostname, `%~` cwd (~ zkráceně), `%$` #/$.

## Build & deploy
```sh
modal run gbsh_build.py          # → /tmp/gbsh (bionic dynamic, ~28 KB)
# deploy: base64 chunky přes ashell do $FILES/usr/bin/gbsh + chmod 755
```

## Test (host i device)
```sh
printf 'echo A && echo B || echo C\nexit 0\n' | gbsh; echo rc=$?
```

## Ověřeno na device (2026-08-24)
- builtins + redirecty (včetně `2>`) ✓
- pipeline přes parrot binárky (elf_loader ownall) ✓
- && / || / ; chainy ✓, exit code propagation ✓
- $VAR/~/ expanze, aliasy, history, source ~/.gbshrc ✓
