# AGENTS.md — elf_loader

Krátký provozní manuál: co to je, jak se to buildí/deployuje, jaká pravidla
platí a co je otevřené. **Historie, root cause, diffy a naměřená čísla jsou
v [postup.md](postup.md)** (chronologický deník; odkazy `[#L…]` = řádek tam).

## Cíl

Vlastní načítání glibc (`libc.so.6` + deps) do privátního scope a běh glibc
binárek v procesu na AArch64 Androidu (bionic host). Preference non-root
(`--ownall`); chroot (`linuxsh`) je rychlá alternativa jen s rootem.
Nahrazuje `proot` — žádný ptrace překlad syscallů, takže uživatel může
pouštět distro nástroje přímo v appce bez NDK rebuildu.

## Build a prostředí

gcc 14.2 v prootu, `as` symlink fix [#L8]. Testy: `make test`, `--ownall`
[#L15]. Ověřené runtime hodnoty (GOT/TLS/arena) [#L76].
Nasazený loader je NDK clang build **s `-O0`** (Makefile i `build.yml`) —
bajtové smyčky v shimech nejsou zadarmo. Host unit testy (bez zařízení):
`test/{cache,relsym,l1,l1_common,l1_logic}_test.c`, build v hlavičce souboru.

## Výkon — co má a nemá smysl řešit

Měřeno host-level (mimo DW, `LX_DW=0`): nativ 11 / proot 28 / loader ~26 ms
na `/bin/true`; `find /usr` ~25× rychlejší než proot. Loader se vyplatí na
dlouhé procesy, ne na krátký exec.

Hotové optimalizace (detaily [#L92] a postup.md 2026-10-20): cache svc-site
skenu (`cache_nr_t.scanned`, i negativní výsledek) a DT_GNU_HASH lookup
(`elf_scope_find` už není lineární `strcmp`). Dohromady `python3 -c 0`
208 → 102,8 ms, 1000× `/bin/true` 47,4 → 25,8 ms.

**Relokace je po hashi jen 2–12 % exec** → cache RELRO obrazů i lazy binding
jsou hotové, ale defaultně vypnuté a měřením nepotvrzené. Neotvírat znovu
bez nového měření.

Knoby: `ELF_LOADER_TIMING`, `ELF_LOADER_NO_CACHE`, `ELF_LOADER_NO_HASH`,
`ELF_LOADER_HASH_CHECK`, `ELF_LOADER_LAZY`, `ELF_LOADER_IMGCACHE`,
`ELF_LOADER_NO_RELSYM`, `ELF_LOADER_NO_BIONIC_BRIDGE`, `ELF_LOADER_EXEC_TRACE`,
`ELF_LOADER_L1` (`=1`/`=2`), `ELF_LOADER_L1_SCAN`, `F2_SKIP=a,b` (denylist
jmenných hooků), `F2_ONLY` (allowlist), `ELF_LOADER_L2` (`=1` záchytná síť),
`ELF_LOADER_L2_LAZY` (líný přepis, default vyp), `ELF_LOADER_GOMODE=0`
(vypne Go mód; L2 ho nahrazuje).

**Metodika:** pinování CPU mění čísla o desítky % → měřit při stejné CPU
politice a A/B dělat knobem na téže binárce, ne dvěma buildy.

## Git / GitHub

Ruleset `protect-master` (zákaz update/force-push/smazání, bypass jen admin),
`protect-dev` (zákaz force-push/smazání). `feedback-automerge.yml`: PR do
`dev` měnící **jen** soubory ve `feedback/` se sám squash-mergne; jinak ruční
review. Před synchronizací `git pull origin dev`, pak `master` ← `dev` (ff).

## Deploy na zařízení

- Magisk modul + `elf` wrapper (bionic NDK build), namespace workaround
  `/data/adb/` [#L328]. `LD_LIBRARY_PATH` poisoning fix — `/system/lib64`
  PRVNÍ [#L406].
- `linuxsh` (chroot + `unshare -m`) [#L558]. **KRITICKÉ:** hned po
  `unshare(CLONE_NEWNS)` → `mount --make-rprivate /` (jinak mounty leaknou
  do global NS) [#L605]. Architektura spouštění [#L598], [#L731].
- `bzsh` — bundled bionic zsh 5.9.2, instaluje se do `/data/adb/` stejně jako
  `linuxsh` (Magisk mount není vidět v app namespace).
- **`mount(2)` je v app doméně ENOSYS** (seccomp) — `mount --bind` v rootfs
  nejde, použít symlink.

## Shelly: `lx`, `init`, double world

- **`init zsh|bash`** — univerzální ROOTFS, žádný hardcode:
  `elf_print_init_env` odvozuje D/ROOTFS/R/L/LX_LOG z `$HOME`. Host
  `.zshrc`/`.bashrc` = `eval "$(… init …)"` [#L2395].
- **`.elfrc`** — jediný uživatelský rc pro host shell (starship/zoxide/aliasy),
  `ELF_RC` override, `LX_NO_RC=1` skip; `init example` / `init install`.
- **`lx` toolkit** (žije jen na zařízení, není v gitu) [#L2425].
  `lx -c '<řádka>'` posílá celou řádku guest shellu → pipy/`&&`/redirecty.
  `lxwhich`/`lx` zkouší první `$VIRTUAL_ENV/bin`.
- **Double world** (`init zsh|bash -dw`, `lxdw on|off|status`): vstup cwd do
  `$ROOTFS` spustí guest shell pod loaderem; `cd ..` z `/` ukončí guest přes
  marker `$LX_LOG/dw/mark.<pid>`. Guest rc = `$LX_LOG/dw/rc`.
- **`lxfb [-m pozn.] <cmd>`** — běh s debug env, `tee` do logu, report do
  `$LX_LOG/feedback/` → PR do `dev`/`feedback/`.

## Seccomp / exec

App seccomp (kernel 4.14): `clone3` SIGSYS, `close_range`… →
`install_legacy_syscall_filter()` (RET_ERRNO ENOSYS pro 435/436/437/439),
fork veneer (vfork-style clone) [#L685].

## Překlad cest — vrstvy

- **L3 jmenné hooky** (`g_f2_hooks`, GOT override + inline/BTI hooky) —
  default, plné pokrytí sémantiky (exec/own-load, symlinky, getcwd, Bun).
- **L1** (prompt 04, `src/l1.h`, `src/l1_emit.h`, `src/l1_common.S`):
  cestová `svc #0` v načtených modulech → `b` do per-site stubu, překlad na
  hranici syscallu. **Default vypnuto.** `ELF_LOADER_L1=1` = L1 vedle hooků,
  `=2` = navíc vypne 52 hooků „jen překlad" (`g_l1_translate_only`).
  Patchuje jen cestová čísla (libc 48 + 10 dynamických `x8` na 17 stránkách,
  ld.so 9), seznam míst v cache (v3, slot `CACHE_NR_L1`). Cena ~1 % na
  `find`, 0,1 % na python importu, RSS +85 KB. Mimo L1: exec, mount,
  bind/connect. `L1=2 all` = 194/0/33 [2026-10-21 (2)].
- **L2** (prompt 05, `src/l2.h`): seccomp `RET_TRAP` záchytná síť pro kód,
  který L1 nepřepíše — statické binárky, Go, Bun (raw syscally), JIT. Cestové
  `svc` z **textu guest exe** TRAPnou, `sigsys_handler`→`go_handle` přeloží.
  Zobecněný Go mód (`l2_install_range_filter` sdílí Go mód i L2, nastavuje
  `g_go_lo/hi`). PIE exe se přesune do pevného okna `0x11_0000_0000`
  (`MAP_FIXED`), aby text-range nekolidoval s ASLR host binárek; ET_EXEC
  (gh/Bun/static) mají pevnou nízkou adresu. `go_handle` + `getcwd`/`readlinkat`
  strip → Bun bez `F2_NO_CWD_STRIP`. **Default vypnuto** (`ELF_LOADER_L2=1`;
  pod ním se místo Go módu instaluje L2). Lazy přepis `svc`→L1 stub
  (`ELF_LOADER_L2_LAZY=1`, default vyp — není ARM-safe souběžně). `L2=1`
  dělá statickou glibc i Go viditelnou do rootfs. `all` default 195/0/33
  [2026-10-21 (3)].

## Pravidla a pasti (než sáhneš na kód)

1. **Guest TP vs bionický TLS.** Kód, který běží pod guest (parrot) TP, nesmí
   volat bionické TLS-závislé funkce (`snprintf`, `getpid`, `getenv`,
   `fprintf`, `sigaction`, `sigaltstack`). Buď raw syscall, nebo obalit
   `dl_enter_host()`/`dl_leave_host()`. `dl_enter_host` **musí** blokovat
   async signály. Tato past se v projektu opakovala 3× (`4602b18`, `80c3ffd`,
   `c69eeac`).
2. **glibc 2.41 funkce začínají `BTI c`** → `hook_install` je nepatchuje a
   glibc-interní `bl` obejde shim. Řešit wrapperem na vnější funkci nebo
   `hook_inline_prologue`.
3. **GOT override je per-jméno.** Při přidání shimu zkontrolovat aliasy
   (`nm -D libc.so.6`) a importy binárek (`nm -D --undefined-only`).
4. **Jeden rc pro oba světy — nikdy `/root/…` natvrdo.** `/root/.zshrc`
   a `$ROOTFS/root/.zshrc` jsou tentýž inode, liší se jen `$HOME`; host
   bionic shell `/root` nemá. Vždy `$HOME/…`. Po instalaci
   `grep -n '/root/' ~/.zshrc ~/.bashrc`.
5. **`su -c` běží přes proot a maskuje path bugy** — platný test je jen
   `ashell -c`.
6. **`test-all.sh`:** bare `grep` = host toybox (nezná BRE `\|`) → `grep -E`.
   `ashell_out` má `timeout 2` → chladný zápis cache se nevejde, použít
   `$R/usr/bin/timeout 25`. Sadu `all` nespouštět v `nohup … &` uvnitř
   background volání (osiřelý proces).
7. **Rootfs absolutní symlinky** jádro vyhodnocuje proti HOST kořeni →
   v rootfs držet kanonicky relativní tvar (`src/relsym.h`). Test `relsym`
   musí měřit HOST nástroji, jinak ho shim zachrání a A/B nic neprokáže.
8. **Hardlinky (`link`) zakazuje SELinux app domény** i nativně — platformní
   limit.
9. **Kód volaný z L1 stubu** (`l1_before/after`, `shim_translate`, relsym)
   běží pod guest TP **na cizím zásobníku** (Go goroutine 8 KB) → žádné
   velké buffery na stacku (použít slot). Syscall ABI zachovává **všechny**
   registry kromě `x0` — i `x1..x7` se guestu vrací původní, ne přeložené.
   `stp/ldp` X párů má dosah jen 504 B. Host test asm musí mít mock, který
   caller-saved registry a NZCV úmyslně ničí (jinak negativní kontrola projde).

## Otevřené bugy

- ~~**Statický glibc ELF v rootfs končí SIGSYS** (RC=159)~~ — **vyřešeno pod
  L2** (prompt 05, 2026-10-21 (3)): `ELF_LOADER_L2=1` dá statické binárce
  filtr na její text-range, cestové syscally se přeloží a doběhne (RC=0, vidí
  rootfs). Bez L2 (default) pořád padá — na default přepnout až s L2.
- **Vlastní `argv[0]` se při exec zahazuje** (`exec -a`, zsh `ARGV0=`, perlí
  `exec {} LIST`) → rozbíjí Claude Code `grep` (re-exec jako `ugrep`).
  Workaround `command grep` [#L3939].
- **`/proc/self/cmdline` není shimnutý** a leakuje cestu k loaderu. Fix
  analogický `shim_readlink` — pozor na TLS past [#L3973].
- **Binárku mimo `$ROOTFS` nelze spustit** — `shim_execve` nalepí `$ROOTFS`
  i na absolutní host cestu. Nejspíš táž vrstva jako vnořený `make`
  (postup.md 2026-10-07 (2)) — řešit najednou [#L4029].
- **rustc/cargo odvozuje `sysroot` z `cwd`, ne z binárky** → `E0463: can't
  find crate for std`. Root cause neizolován. Workaround
  `--sysroot $R/root/.rustup/toolchains/stable-aarch64-unknown-linux-gnu`
  [#L3988].
- **L1: `pthread_cancel` vlákna blokovaného v cestovém syscallu** se projeví
  až po návratu (PC je v `l1_common`, ne v okně `__syscall_cancel_arch`).
  Bezpečné, jen odložené. Default L1 zatím vypnutý.
- **Celorootfs konverze absolutních symlinků čeká na potvrzení** (1093 linků,
  1081 převoditelných) — `tools/relsymlinks.sh --dry-run`.
- Test na reálném 16K Android 15+ (Task 3). `nmap` není v rootfs [#L680];
  ostatní síťové binárky ověřeny OK.
- Kosmetika: `src/main.c:149` sign-compare, `\]` escape.

## Mapa postup.md

| téma | řádky |
|---|---|
| build, testy, runtime hodnoty, stav | L8–L92 |
| IFUNC resolver | L49 |
| static TLS pro exe, tcbhead | L101 |
| `strerror`, tunable env override | L121, L139 |
| bionic static-PIE rebase | L154 |
| PAGE_SIZE 16K | L189 |
| NDK cross-compile (Modal), 16K/namespaces | L210, L246, L254 |
| brk desync / private arena | L279 |
| TLS blok pod TP | L303 |
| Magisk modul, `elf` wrapper, `LD_LIBRARY_PATH` | L328, L406 |
| `linuxsh`, mount propagace, architektura | L558, L598, L605, L731 |
| fixy #1/#2 (stack_chk, rseq, emutls) | L493, L509 |
| C++ inity pod bionic TP, apt, nano, TUI | L628, L651, L662, L795, L870 |
| seccomp filtr, fork veneer | L685, L704, L710 |
| shell/integrace, `init`, `.elfrc`, `lx`, dw | L2376, L2395, L2425, L2795 |
| uv/venv, shebang, fstat, test runner, NSS | L2529, L2570, L2606, L2715 |
| Node saga | L1004–L2272 |
| `pi`/`pi-starship`, dl_enter_host | L2742 + 2026-10-06 (2)–(6) |
| Bun, long argv, bionic bridge | L2273 + 2026-10-06 (7), 2026-10-12 |
| `/proc/self/exe`, `eaccess`, pathops, posix_spawn | 2026-10-07 (2)–(7) |
| relsym + rootfd | 2026-10-21 |
| L1 — překlad na hranici syscallu | 2026-10-21 (2) |
| L2 — seccomp záchytná síť (okno, go_handle) | 2026-10-21 (3) |
| výkon: timing, cache, hash, imgcache, lazy | 2026-10-16, 2026-10-20 |
| zátěžový test multi-runtime | L3885, L4047 |
| otevřené nálezy exec/argv | L3939, L3973, L3988, L4029 |

Otevřené bugy též v auto-paměti `[[elf-loader-open-bugs]]`.
