# Prompt 04 — L1: přepis všech cestových `svc #0` při načtení

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(sekce 10 — historie jmenných hooků, BTI, aliasy) a
`docs/nextgen/proot-nahrada.md` kapitoly 0 a 2.1.

## Cíl

Přesunout překlad cest z ~150 jmenných hooků na hranici syscallů, v procesu.
Výchozí data: glibc 2.41 má 411 `svc #0` na 62 stránkách, u 401 je `x8`
konstantní (`mov x8,#nr` do 8 instrukcí před `svc`), u 10 dynamické.

## Návrh (závazné body)

- Zobecnit `elf_patch_syscall_sites` (`src/elf_loader.c:1942`): `svc #0` →
  **`b <per-site stub>`** (ne `bl` — přepsal by `x30` v listových funkcích).
  Stub končí `b <místo+4>`.
- „Ostrov stubů" do ±128 MiB od každého modulu (loader volí adresy). Stub uloží
  registry, zavolá překladač, provede `svc #0` sám, opraví výsledek, obnoví
  registry kromě `x0`.
- Tabulka cestových syscallů (arm64 čísla) s pozicemi cestových argumentů:
  `openat`, `newfstatat`, `faccessat`, `faccessat2`, `execve`, `execveat`,
  `mkdirat`, `mknodat`, `unlinkat`, `renameat`, `renameat2`, `linkat`,
  `symlinkat` (jen linkpath, ne target), `readlinkat`, `statx`, `chdir`,
  `chroot`, `truncate`, `fchmodat`, `fchownat`, `utimensat`, `statfs`,
  `setxattr`/`getxattr`/`listxattr`/`removexattr` (+ `l*`), `inotify_add_watch`,
  `bind`/`connect` (AF_UNIX `sun_path`), `mount`. Výsledky: `getcwd`,
  `readlinkat` (strip `$ROOTFS`, `/proc/self/exe`).
- Dynamické `x8` (10 míst — `syscall()`, cancelace): stub s dispatchem za běhu.
  Disassemblací potvrď, kde ta místa jsou.
- **Překladový buffer ne na zásobníku** (Go goroutines mají 8 KB): per-vláknová
  oblast indexovaná podle `gettid`, bez TLS a bez malloc.
- Idempotence: cesta, která už začíná `$ROOTFS`, se nepřekládá (`shim_translate`
  to umí) → L1 může běžet souběžně s jmennými hooky.
- Zapnout nejdřív za `ELF_LOADER_L1=1`; postupně, po ověření, jmenné hooky
  vypínat (začni těmi, které jsou jen „přelož cestu a zavolej originál").
- Scan cachovat podle BuildID (pokud prompt 02 už cache zavedl, použij ji).

## Pozor

- Stránky s patchem se COWnou (~62 stránek glibc) — ověř dopad na RSS.
- Vše ve stubu a překladači = guest TP → jen raw syscally.
- Signály uvnitř stubu: handler guestu může během stubu běžet — stub musí být
  reentrantní (žádný globální stav bez per-thread oddělení).
- `elf_patch_syscall_sites` dnes patchuje i KILLované syscally (rseq, clone3) —
  ty zůstávají NOP/`movn`, ne stub.

## Hotovo, když

- S `ELF_LOADER_L1=1` a **vypnutými** jmennými hooky typu „jen překlad" projde
  `test-all.sh all` (nebo je seznam rozdílů vysvětlený).
- A/B test `test-all.sh l1`: případy, které jmenné hooky historicky minuly —
  `eaccess` (make fast path), `mkdtemp`, `remove`, `glob`, `nftw`, `scandir`,
  `ftok`/`tempnam` — projdou i bez příslušného jmenného hooku.
- Změřená cena (prompt 01) na `find`/python importu není horší než dnes.
