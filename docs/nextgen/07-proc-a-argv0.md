# Prompt 07 — virtualizace `/proc` (`cmdline`, `maps`, `mounts`, `fd`, `cwd`) + `argv[0]`

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(sekce 10: `/proc/self/exe` fix a jeho TLS past; otevřené nálezy `argv[0]`
a `/proc/self/cmdline`) a `docs/nextgen/proot-nahrada.md` kapitoly 3.4, 3.5.

## Úkoly

1. **`argv[0]`** (otevřený nález, postup.md:3939): exec shim zahazuje vlastní
   `argv[0]` (`exec -a`, zsh `ARGV0=`, perl `exec {} LIST`). Předat ho loaderu
   zvlášť (nový přepínač, např. `--argv0 <s>`) a loader ho dá guestu jako
   `argv[0]`. Ověř Claude Code `grep` → `ugrep` (`error: unknown option '-G'`)
   a `find` (`error: unknown option '-S'`) — po opravě musí fungovat bez
   `command grep`.
2. **`/proc/self/cmdline` a `/proc/<pid>/cmdline`** vlastního procesu: při
   `open(at)` vrátit memfd se syntetickým obsahem (guest argv oddělené `\0`).
   `memfd_create` (279) — ověř, že ho app profil pouští; jinak fallback
   na soubor v `$LX_LOG/tmp` otevřený a hned unlinknutý.
3. **`/proc/self/maps`**: přepsat jméno loaderu na guest exe a cesty
   `$ROOTFS/...` na guest cesty (sanitizers, JVM, Go runtime maps čtou).
4. **`/proc/self/mounts`, `/proc/self/mountinfo`, `/proc/mounts`**: syntetický
   rootfs `/` + binds z tabulky (prompt 06, pokud existuje).
5. **`readlink /proc/self/fd/N`, `/proc/self/cwd`**: strip `$ROOTFS`.

## Pozor

Celé to běží pod guest TP: žádný `snprintf`/`getpid` z bioniku — první verze
`/proc/self/exe` fixu tím rozbila python. Čísla formátuj ručně, pid přes raw
syscall.

## Hotovo, když

- `cat /proc/self/cmdline | tr '\0' ' '` ukazuje guest argv; `ps`-style nástroje
  (`ps -o args`) pro guest procesy taky (pokud čtou cizí pid — zdokumentuj limit).
- `exec -a foo sleep 1 & ps`/`/proc/<pid>/cmdline` ukazuje `foo`.
- A/B testy `test-all.sh argv0`, `test-all.sh procvirt`; `all` bez regrese.
