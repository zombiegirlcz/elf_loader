# PROMPT — elf_loader jako fast path místo prootu

> Úkol pro AI agenta (nebo poznámka pro sebe): implementovat path confinement
> a exec bridge v elf_loaderu, tak aby nahradil proot u one-shot příkazů.
> Čtení povinné předem: `README.md`, `postup.md`, `PROOT_FASTPATH.md`.

## Kontext projektu

`~/elf_loader` je vlastní ELF64 loader pro AArch64, který načítá glibc binárky
(Parrot/Kali rootfs) nativně do bionického procesu na Androidu — bez prootu,
bez ptrace. Režim `--ownall` own-loaduje celý glibc cluster (libc.so.6, libm,
libselinux…) do privátního scope, řeší relokace včetně IRELATIVE/TLSDESC,
staví vlastní TLS (TP switch v trampolině) a private heap @ `0x7f00000000`.

**Stav:** 10/10 PASS na zařízení (echo/true/false/uname/date/wc/cat/ls/grep/sed),
btop/htop/btm --version OK, seccomp compat filtr (clone3/close_range/openat2/
faccessat2 → ENOSYS) nasazený.

## Proč

| Cesta | Path syscall | Start one-shot |
|---|---|---|
| nativní / chroot | ~1–2 µs | ~25 ms |
| **elf_loader --ownall** | **~1–2 µs** | ~66 ms |
| proot (ptrace) | 104 µs fresh / 345–449 µs live | ~250–500 ms |

proot nelze zrychlit (ptrace design). elf_loader už běží 5–8× rychleji —
chybí jen **path confinement** (/ = rootfs) a **fork/exec guest binárek**.

## Co implementovat (v pořadí)

### Fáze 1 — path confinement přes seccomp RET_TRAP + SIGSYS handler

- Do `install_legacy_syscall_filter_impl()` (src/elf_loader.c) přidat
  `SECCOMP_RET_TRAP` pro path syscalls (aarch64 nr):
  openat(56), newfstatat(79), statx(291), faccessat(48), readlinkat(78),
  unlinkat(35), mkdirat(34), renameat(38), renameat2(276), execve(221),
  chdir(51), fchmodat(53), fchownat(54), symlinkat(36), linkat(37),
  mknodat(33), utimensat(88), truncate(45).
- SIGSYS handler (async-signal-safe, JEN raw svc — žádné printf/malloc,
  pattern `sys_write` v kódu):
  1. z `si_syscall` + `ucontext->uc_mcontext.regs` najít pointerové argy
  2. translace: bind tabulka (`--bind src:dst`, stejná syntaxe jako
     `nh distro login`) → `/X` → `$ROOTFS/X` → relativní přes virtualizované cwd
  3. raw `svc #0` s přepsanými pointery → výsledek do `regs[0]`, `pc += 4`
- Virtualizace cwd: `getcwd()` vrací guest view, `chdir` překládá a ukládá
  guest cwd do loader globálu (handler ho potřebuje).
- Reverzní translace na výstupu (`readlink`, `realpath`): `$ROOTFS/X` → `/X`.
- `/proc`, `/sys`, `/dev` passthrough; bind tabulka se vyhodnocuje PŘED rootfs.

### Fáze 2 — execve bridge

Trapped `execve`: přeložit cestu → pokud cíl je dynamický ELF s guest interpu
→ `execve("/proc/self/exe", {"elf_loader", "--ownall", <translated>, args…},
envp)`. Seccomp filtr se dědí → child je confined taky. Tímto začnou fungovat
pipeline (`ls | wc -l`) s parrot binárkami — dnes PT_INTERP ENOENT.

### Fáze 3 — startup tuning (66 ms → cíl <30 ms)

1. Skip `.symtab` parsing při ownall (`elf_load`, jen dynsym stačí)
2. Lazy PLT default pro device build (`--lazy` existuje)
3. Batch mprotect, měřit markery `clock_gettime(CLOCK_MONOTONIC)` kolem
   load/reloc/TLS/init_array

## Kritická pravidla (z postup.md — NIKDY neopakovat staré bugy)

1. **Handler pod parrot TP nesmí šáhnout na host TLS** — fprintf ve fault
   handleru padal; jen raw syscalls a statické buffery.
2. **`__NR_seccomp = 277`** na aarch64 (278 je getrandom — EINVAL past).
3. **Filtry se dají jen přidávat**, ne ubírat — stacked RET_TRAP je legální.
4. **Privátní heap**: sbrk/brk/fork veneery (`write_heap_veneer`,
   `patch_module_heap_syms`) nesmím rozbít — nový kód jde vedle nich.
5. **LD_LIBRARY_PATH pořadí**: `/system/lib64:/system/lib` PRVNÍ (wrapper `elf`),
   parrot cesty až za — jinak bionic linker natáhne parrot libc ld-script.
6. **Emutls zakázáno** v loaderu (NDK clang dělá `__thread` jako emutls →
   crash pod parrot TP) — žádné `__thread` v novém kódu.
7. **Bionic phdr fix** (`maybe_fixup_bionic_phdr`) nedotýkat se glibc binárek.
8. Rootfs absolutní symlinky nefungují mimo chroot — bind tabulka musí umět
   i alternativy (`/etc/alternatives/...`).

## Ověření

- Rozšířit `test/compat_tests.sh` o variantu WRAPPER=elf (dnes: proot vs chroot);
  cíl: elf output ≡ proot output (22/23 identických jako chroot).
- Mikrobenchmark hned na startu: 1000× openat v cyklu, filtr ON vs OFF
  (ověřit cenu SIGSYS na jádře 4.14 — očekávání 5–20 µs/syscall).
- Regrese: `make test` rc=0, 10/10 device baterie, setarch -R ×10.
- Na zařízení: `ROOTFS=... elf ls /etc` musí číst ROOTFS/etc (ne host /etc!).

## Integrace do kali_core_emulator (po Fázi 1+2)

- `nh distro exec <distro> [--bind …] -- cmd` → fast path (`login` už existuje)
- gbsh routuje parrot přes `--ownall` už teď → dostane confinement automaticky
- `launcher.sh`: volitelný režim `NH_FAST=1`
- proot zůstává pro interaktivní TUI (htop fullscreen, ncurses) a apt/dpkg
