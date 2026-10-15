# Prompt 11 — výzkum: soft exec a zygote (exec bez `execve`)

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(fork veneer v sekci 6, signály v sekci 8) a `docs/nextgen/proot-nahrada.md`
kapitoly 5.7, 5.8. **Předpoklad:** prompty 01 a 10 — tohle má smysl jen
pokud exec po nich pořád výrazně zaostává za nativem.

## Charakter úkolu

Výzkum + prototyp za proměnnou, **ne** produkční přepnutí. Výstupem je
rozhodnutí podložené měřením.

## Varianta A — soft exec

Když guest proces spouští glibc program: v potomkovi po `fork`/`vfork` místo
`execve`:
- zavřít `O_CLOEXEC` fd (raw `fcntl` přes `/proc/self/fd`), resetovat dispozice
  signálů (kromě ignorovaných), zrušit sigaltstack, robust list, timery;
- odmapovat vše kromě loaderu a sdílených glibc modulů (podle `/proc/self/maps`);
- obnovit **pristine RW snapshot** glibc + deps pořízený jednou po initu;
- načíst nový exe + chybějící deps, postavit stack/auxv, `PR_SET_NAME`,
  aktualizovat `g_guest_exe_path`/cmdline virtualizaci, skočit na entry.
Prozkoumej a zdokumentuj, co přesně se tím rozbije (debuggery, `/proc/<pid>/exe`
pro cizí procesy, setuid sémantika, vlákna rodiče po `vfork`, atfork handlery).

## Varianta B — zygote / fork server

Trvalý `lxd` s předinicializovanou glibc; exec = požadavek přes unix socket,
zygota forkne. Proxy stub u volajícího: předá fd 0/1/2 + tty přes `SCM_RIGHTS`,
přeposílá signály, vrací exit kód. Navrhni řešení job controlu (`setsid`,
`TIOCSCTTY`, `TIOCSPGRP`, `setpgid`), nebo zdokumentuj, že pro interaktivní
procesy zygota nepůjde a bude jen pro neinteraktivní (configure/make).

## Výstup

- Prototyp vybrané varianty za `ELF_LOADER_SOFTEXEC=1` / `LX_ZYGOTE=1`.
- Měření 1000× `/bin/true` a `./configure` proti promptům 02/10.
- Kapitola „Soft exec / zygote — výsledek" do `docs/nextgen/proot-nahrada.md`:
  doporučení jít/nejít dál, seznam známých nekompatibilit.
