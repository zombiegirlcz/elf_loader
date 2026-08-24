# PROOT_FASTPATH — zrychlení spouštění distro příkazů přes elf_loader

Analyza (2026-08-24): jak využít elf_loader k obejití/zrychlení proot.

## Východiska (měřená data)

| Cesta | Cena per syscall s cestou | Start one-shot příkazu |
|---|---|---|
| nativní / chroot | ~1–2 µs | ~25 ms (20× echo = 500 ms) |
| **elf_loader --ownall** | **~1–2 µs (žádný tracer!)** | ~66 ms (20× echo = 1319 ms) |
| proot (ptrace) | 104 µs fresh / 345–449 µs live | ~250–500 ms+ |

**Klíčový poznatek:** proot nelze zrychlit — cena je inherentní designu
(ptrace-stop na každý path syscall, kontext-switch do traceru, čtení/zápis
vzdálené paměti). Seccomp akcelerace už je zapnutá (`ptrace acceleration
(seccomp mode 2) enabled`). Jediná cesta = **obejít ptrace úplně**.

elf_loader to už dělá: binárka běží nativně, žádný tracer, syscalls plnou
rychlostí. Chybí jen dvě věci, které proot má: **path confinement**
(/ = rootfs) a **fork/exec guest binárek**.

## Fáze 1 — Path confinement bez ptrace (největší výnos)

**Mechanismus: stacked seccomp `RET_TRAP` na path-syscalls + SIGSYS handler.**

- Infrastruktura existuje: `install_legacy_syscall_filter_impl()` už instaluje
  stacked filtr (ENOSYS pro clone3/close_range/openat2/faccessat2) a
  `PR_SET_NO_NEW_PRIVS` je nastaveno. Filtry se dají jen přidávat.
- Přidat do stejného filtru: `SECCOMP_RET_TRAP` pro path syscalls
  (aarch64 čísla): openat(56), statx(291), faccessat(48), readlinkat(78),
  unlinkat(35), mkdirat(34), renameat(38/276), execve(221), chdir(51),
  fchmodat(53), fchownat(54), symlinkat(36), linkat(37), mknodat(33),
  utimensat(88), truncate(45), newfstatat(79)…
- SIGSYS handler (async-signal-safe, jen raw svc — pattern `sys_write`
  už je v kódu):
  1. `si_syscall` + `ucontext->uc_mcontext.regs` → najde pointerové argy
  2. přeloží cestu: bind tabulka (`--bind src:dst`, stejná jako nh distro login)
     → jinak `/X` → `$ROOTFS/X`; relativní → přeložit přes virtualizovaný cwd
  3. udělá raw `svc #0` s přepsanými pointery
  4. výsledek do `regs[0]`, `pc += 4` (přeskočit trapped svc)

**Proč to pokryje všechno:** na rozdíl od veneerů na exportovaných symbolech
(tedy jen PLT volání z exe) zachytí seccomp i **interní inline syscally libc**
(fopen → interní openat bez symbolu). To je plná ekvivalence proot translace.

**Cena:** synchronní signál v-process ≈ 5–20 µs/syscall vs proot 104–449 µs
→ **5–20× rychlejší než proot**, stále ~10× pomalejší než native (přijatelné).

### Detaily, na které si dát pozor
- `getcwd()` musí vracet **guest view**; `chdir` překládá a ukládá guest cwd
  do loader globálu (handler ho potřebuje pro relativní cesty).
- `readlink`/`realpath` vracejí **guest** cesty → handler musí dělat i reverzní
  translaci (`$ROOTFS/X` → `/X`) na výstupních bufferech.
- `/proc`, `/sys`, `/dev` → passthrough (proot je taky binduje); bind tabulka
  se vyhodnocuje PŘED prefixem rootfs.
- Handler nesmí volat printf/malloc (async-signal-safe!) — jen statické buffery
  a raw syscalls.
- Scratch buffery pro přeložené cesty: per-thread pole (nebo jedno — exe je
  single-threaded v této fázi).

## Fáze 2 — execve bridge (fork/exec parrot binárek)

Dnes: fork+exec guest dynamické binárky selže (PT_INTERP `/lib/ld-linux-aarch64.so.1`
ENOENT pod bionickým hostem) → pipeline s parrot binárkami nejedou.

**Fix v SIGSYS handleru pro execve:** přeložit cestu → pokud cíl je ELF s
guest interp → `execve("/proc/self/exe", {"elf_loader", "--ownall",
<přeložená_cesta>, originální_args...}, envp)`. Child se stane dalším loaderem,
binárka poběží nativně. Seccomp filtr se dědí — child má confinement taky.

Výsledek: `bash -c 'ls | wc -l'` s parrot binárkami funguje nativně, každý
člen pipeline nezávisle confined.

## Fáze 3 — startup latency (66 ms → cíl <30 ms)

1. **Přeskočit `.symtab` parsing při ownall loadu** (`elf_load` ř. ~1051 čte
   SHT_SYMTAB — ownall potřebuje jen dynsym; symtab je pro introspect).
2. Lazy PLT default pro device build (`--lazy` už existuje, jen default off).
3. Batchovat mprotect (apply_segment_prots dělá mprotect per segment — OK,
   ale kontroluj počet volání během relokací RW stránky textu).
4. Měřit! `clock_gettime(CLOCK_MONOTONIC)` markery kolem load/reloc/TLS/init.

## Integrace do kali_core_emulator

| Bod | Změna |
|---|---|
| `nh distro exec <distro> [--bind ...] -- cmd args` | fast-path one-shot (login už je) |
| gbsh | už routuje parrot přes `--ownall` — dostane confinement automaticky |
| launcher.sh `NH_FAST=1` | volitelný režim session bez prootu |
| Root Bridge UI | později: přepínač „Fast exec (no proot)" |
| proot | zůstává pro interaktivní TUI (htop full-screen, ncurses) a apt/dpkg |

## Pořadí implementace

1. Fáze 1 (trap+translate) — jádro hodnoty, ~300–400 řádků C
2. Test baterie: compat_tests.sh rozšířit o variantu WRAPPER=elf+confinement
   (dnes porovnává proot vs chroot; cílem je elf ≡ proot output)
3. Fáze 2 (execve bridge)
4. Fáze 3 (startup tuning) + integrace do nh/gbsh

## Rizika
- Jádro 4.14: RET_TRAP je starší funkce, OK. Syscall User Dispatch (5.9+) NE —
  použít klasický seccomp-trap.
- SIGSYS handler + parrot TP: handler může být vyvolán POD parrot TPIDR_EL0 —
  nesmí sahát na host TLS (stejná past jako fprintf ve fault handleru —
  proto raw syscalls only).
- Statické binárky (bez libc): trapují taky — handler funguje stejně (path
  translace je ortogonální vůči tomu, kdo syscall volá).
- Výkon signálů na 4.14: změřit hned na začátku mikrobenchmarkem
  (1000× openat v cyklu, s filtrem a bez).
