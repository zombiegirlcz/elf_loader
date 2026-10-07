# AGENTS.md — elf_loader (komprimovaný rozcestník)

Toto je hutný index k [postup.md](postup.md). Sekce jsou seřazeny tematicky
(původní `postup.md` je chronologický deník). Odkazy vedou na řádky v
`postup.md`, kde je plný kontext, čísla, diffy a ověření.

**Cíl loaderu:** vlastní načítání glibc (`libc.so.6` + deps) do privátního
scope a běh glibc binárek v procesu na AArch64 Androidu (bionic host).
Preference — non-root vlastní loading (`--ownall`); chroot (`linuxsh`) je
rychlejší alternativa jen s rootem.

---

## 1. Základ

- Sestavení, gcc 14.2 v prootu, `as` symlink fix — [postup.md#L8](postup.md#L8).
- Baterie testů (`make test`, `--ownall`) — [postup.md#L15](postup.md#L15).
- Ověřené runtime hodnoty (GOT/TLS/arena) — [postup.md#L76](postup.md#L76).
- Aktuální stav (linka po opravách) — [postup.md#L92](postup.md#L92).
- **Git / GitHub ochrana (2026-10-06):** ruleset `protect-master` (zákaz
  update/force-push/smazání, bypass jen admin = vlastník → cizí PR do `master`
  nejde mergnout), `protect-dev` (zákaz force-push/smazání). Workflow
  `.github/workflows/feedback-automerge.yml`: PR do `dev`, který mění **jen**
  běžné soubory ve `feedback/`, se sám squash-mergne (`pull_request_target`,
  kód PR se nespouští); jinak čeká na ruční review. Důsledek: `dev` může mít
  commity navíc → před synchronizací `git pull origin dev`, pak teprve
  `master` ← `dev` (ff), ne naslepo `dev` ← `master`.

## 2. Klíčové opravy jádra loaderu

- **IFUNC resolver (`memcpy` crash /bin/ls, /bin/dir)** — root cause + fix
  v `resolve_jmp_symbol()` — [postup.md#L49](postup.md#L49).
- **Static TLS pro načítaný exe (Step 2)** — mmap + kopie tcbhead, msr
  `tpidr_el0` až v trampolině — [postup.md#L101](postup.md#L101).
- **`strerror` `Unknown error` bez čísla** — nastavení
  `*(char*)va(m, 0x1be009) = 1` v `run_module_init` — [postup.md#L121](postup.md#L121).
- **Brk desync (dva alokátoři, jeden brk)** — private mmap arena
  `0x7f00000000`, `write_heap_veneer`/`patch_module_heap_syms` — [postup.md#L279](postup.md#L279).
- **TLS blok pod TP (negativní tls_offset)** — `min_off`/`span` v
  `elf_setup_own_tls` — [postup.md#L303](postup.md#L303).
- **Runtime PAGE_SIZE (`sysconf`) — Android 15+ 16K stránky** — [postup.md#L189](postup.md#L189).
- **Tunable env override (`getenv` before default)** — [postup.md#L139](postup.md#L139).

## 3. Bionic / NDK (Step 3)

- **Bionic static-PIE `p_vaddr` rebase** (`maybe_fixup_bionic_phdr`) —
  [postup.md#L154](postup.md#L154).
- **NDK cross-compile loaderu přes Modal, `dlfcn_stubs.c`** —
  [postup.md#L210](postup.md#L210).
- Empirický 16K test / bionic namespaces — [postup.md#L246](postup.md#L246),
  [postup.md#L254](postup.md#L254).

## 4. Device deploy — Magisk modul, `elf` wrapper, `linuxsh`

- **Magisk modul + `elf` wrapper (bionic NDK build), namespace workaround
  `/data/adb/`** — [postup.md#L328](postup.md#L328).
- **Opravený `elf` wrapper (výmysl `--library-path`), LD_LIBRARY_PATH
  poisoning fix — pořadí `/system/lib64` PRVNÍ** — [postup.md#L406](postup.md#L406).
- **Fixy #1 (`__stack_chk_guard`, rseq, `__libc_stack_end`)** + úklid
  debug — [postup.md#L493](postup.md#L493).
- **Fix #2 — emulated TLS `__emutls_get_address` z `__thread` guard v
  `tunable_get_val`; nahrazeno `static int`** — [postup.md#L509](postup.md#L509).
- **`linuxsh` (nativní chroot + `unshare -m` + `make-rprivate /`)** —
  [postup.md#L558](postup.md#L558).
- **KRITICKÝ FIX: mount propagace leak (342 mountů v global NS)** —
  vždy `mount --make-rprivate /` hned po `unshare(CLONE_NEWNS)` —
  [postup.md#L605](postup.md#L605).
- Finální architektura spouštění (tabulka) — [postup.md#L598](postup.md#L598),
  [postup.md#L731](postup.md#L731).
- **`bzsh` — bundled bionic-native zsh (host shell)**: zsh 5.9.2 (NDK build,
  linkuje jen `libc/libm/libdl`), trimnuté terminfo (~80K), generický `zshrc`
  bez `com.linux_core` hardcode; instaluje se do `/data/adb/` stejně jako
  `linuxsh`/`gbsh` (Magisk mount není vidět v app namespace). `zshrc` evaluje
  `elf_loader init zsh`, pokud je rootfs nastaven — jeden shell pro bionic
  host i glibc guest.
- Kompat parity `linuxsh` vs proot (22/23 identických) —
  [postup.md#L716](postup.md#L716).

## 5. C++ / Rust / TUI a další binárky

- **Inity pod bionic TP → SIGSEGV libstdc++**: fronta initů, spuštění v
  `elf_final_jump` pod parrot TP; `uselocale(NULL)+__ctype_init()` —
  [postup.md#L628](postup.md#L628).
- `nh fix apt` — merged-usr marker, systemd 241→257 no-op preinst —
  [postup.md#L651](postup.md#L651).
- **Wrapper `elf` finální — resolve-by-name, mksh pasti** —
  [postup.md#L662](postup.md#L662).
- **Rootfs absolutní symlinky nefungují mimo chroot** (např.
  `libblas.so.3 -> /etc/alternatives/…`) — [postup.md#L674](postup.md#L674).
- **LOCPATH/LC_ALL/SHELL defaulty natvrdo v `run_ownall`** (dřív ruční env
  před tmuxem/shellem) — [postup.md#L2376](postup.md#L2376).
- **`elf_loader init zsh`** (shell-integrace vzor jako starship/zoxide,
  `eval`-able LOCPATH/LC_ALL pro host zsh) + build/deploy na zařízení —
  [postup.md#L2395](postup.md#L2395).
- **`init zsh|bash` produkčně — univerzální ROOTFS, žádný hardcode**
  (`elf_print_init_env` odvozuje D/ROOTFS/R/L/LX_LOG z `$HOME`; LOCPATH
  jen když locale existuje; `LX_HELPERS_BASH`; `lx` předává guest PATH →
  `lx gcc` najde `ld`). Host `.zshrc`/`.bashrc` = `eval "$(… init …)"`.
  Demo pro prezentaci `tools/demo.zsh` (5 kroků, „stejné jádro, dva
  userspace") — [postup.md#L2795](postup.md#L2795).
- **Host `.zshrc` — `lx` toolkit (kompletní zápis)**: proměnné/cesty,
  prompt, `lx`/`lxq`/`lxdbg`/`lxhelper`/`lxlog`/`lxdiag`/`lxinfo`/`lxfault`,
  `lxtest` regresní sada, `command_not_found_handler`, `help`, starship/
  zoxide integrace (cache + `/proc/self/exe` rewrite) — soubor žije jen
  na zařízení, není v gitu — [postup.md#L2425](postup.md#L2425).
- **`lxfb [-m pozn.] <cmd>`** (v `init zsh|bash`, jedno tělo pro oba shelly):
  běh s `ELF_DEBUG`/`ELF_LOADER_DIAG`/`ELF_LOADER_SIGTRACE`, `tee` do logu,
  report `$LX_LOG/feedback/<model>-<datum>-<cmd>-<čas>.md` (datum, zařízení/SoC,
  Android+SDK, kernel, page size, loader verze+md5, rootfs OS + glibc z
  `libc.so.6`, exit/signál, crash řádky, log ≤ `LXFB_MAX`=4000 ř., nové
  `diag.<pid>.txt`) → PR do `dev` / `feedback/` (auto-merge). Ověřeno na zařízení
  pod bzsh i guest bash.

## 6. Seccomp compat filtr + fork veneer

- **App seccomp (kernel 4.14) — `clone3` SIGSYS, `close_range` atd.**;
  `install_legacy_syscall_filter()` (RET_ERRNO ENOSYS pro 435/436/437/439),
  fork veneer (vfork-style clone) — [postup.md#L685](postup.md#L685).
- Ověření na zařízení + externí exec limit — [postup.md#L704](postup.md#L704),
  [postup.md#L710](postup.md#L710).

## 7. Konkrétní opravy — dep libc.so.6, `--own` flow, gbsh, nano

- #3 libc dep hard-exit; #5 `--own` pc=0x0 — [postup.md#L743](postup.md#L743).
- #2 `ft6` syscall probe diagnostika — [postup.md#L758](postup.md#L758).
- **gbsh v0.4/v0.5 (dual-world flag, cd navigace, dvojitý starship
  prompt)** — [postup.md#L772](postup.md#L772), [postup.md#L969](postup.md#L969).
- **nano 8.4 — `derive_distro_libdirs`** — [postup.md#L795](postup.md#L795).
- Termux proot-distro rootfs (glibc 2.28) — [postup.md#L846](postup.md#L846).
- TUI testy (top/htop/btop/btm) — [postup.md#L870](postup.md#L870).
- Docker-kopie test byl neplatný, reálný bug FLAKY —
  [postup.md#L910](postup.md#L910).
- gbsh static build (combined static OK, static-pie ne) —
  [postup.md#L930](postup.md#L930).

## 8. Node saga (2026-09-22, pokračování 1–15)

Kompletní deník: [postup.md#L1004](postup.md#L1004) až [postup.md#L2272](postup.md#L2272).
Milníky:

- Testovací smyčka bez Modalu (GH Actions) — [postup.md#L1006](postup.md#L1006).
- Call-site/entry tracer + dva vedlejší bugy (alloc_near underflow,
  fprintf pod guest TP) — [postup.md#L1094](postup.md#L1094).
- `ELF_LOADER_TRACE_RING`, deterministický cíl `0x199d560` —
  [postup.md#L1181](postup.md#L1181).
- Vyloučené hypotézy (IC/feedback/CompileLazy, verze node) —
  [postup.md#L1257](postup.md#L1257).
- V8 zdroj — potvrzení SharedFunctionInfo layoutu; getOffsetNanosecondsFor
  vestigiální — [postup.md#L1314](postup.md#L1314), [postup.md#L1397](postup.md#L1397).
- `--inspect-brk` blokován; PAUSE_ENTRY/PAUSE_CALL infra —
  [postup.md#L1441](postup.md#L1441).
- **GDB přes „chroot-jen-pro-gdb“ + attach podle PID** —
  [postup.md#L1538](postup.md#L1538).
- Řetěz bytecode→handler zmapován, JSDispatchTable objevena —
  [postup.md#L1598](postup.md#L1598).
- `--always-sparkplug` odhaluje pravdepodobný root cause —
  [postup.md#L1744](postup.md#L1744).
- **PRŮLOM #2: entry #4096 v JSDispatchTable má jinou identitu
  nativně vs. pod loaderem; ~3352-položková init smyčka v
  `Isolate::Init`** — [postup.md#L1883](postup.md#L1883).
- Rozluštěno `Builtins::code()` — [postup.md#L2135](postup.md#L2135).
- **SKUTEČNÝ FIX (Node 18 tiskne „42“) — SIGABRT handler bug v
  `guest_sigaction`** (3 commity `fix-guest-sigaction`) —
  [postup.md#L2164](postup.md#L2164).
- v26.8.2 stále padá — dokumentovaný JSDispatchTable bug —
  [postup.md#L2256](postup.md#L2256).
- **pi + `@narumitw/pi-starship` v TTY SIGSEGV (RC=139) — VYŘEŠENO**
  (commit `4602b18`): `elf_run_pending_inits` instaloval fault handler
  `elf_install_fault_handlers()` pod **guest (parrot) TP**, ale
  `sigaltstack`/`sigaction` jsou bionické → handler se nenainstaloval a
  SIGSEGV šel na default action. Fix: instalace pod bionickým TP
  (`mrs/msr tpidr_el0` dance). A/B (DEBUG_PC, 6 běhů): starý 0/6 render +
  6× SIGSEGV, nový 6/6 render. test-all 160/0 —
  [postup.md#L2742](postup.md#L2742).
- **pi + pi-starship pod `script` (Node 26) SIGSEGV — VYŘEŠENO** (commit
  `80c3ffd`): async signál (SIGWINCH/SIGCHLD/libuv) dorazil v okně, kdy
  `dl_enter_host` přepnul TP na bionic kvůli `_rtld_global_ro` shimu
  (`ldso_lookup_symbol_x` → `elf_scope_find`); kernel spustil guest handler
  pod bionickým TP → glibc `THREAD_SELF->cancelhandling` (TP-0x618) → pád.
  Fix: `dl_enter_host` blokuje nesynchronní signály (raw `rt_sigprocmask`),
  `dl_leave_host` vrátí TP a pak masku. A/B 5/5 pád → 0/5. Nalezeno přes
  core dump (gdb/strace pád maskovaly) — [postup.md](postup.md) 2026-10-06 (2)–(6).

## 9. Ostatní binárky a poslední fixy

- **Bun (`claude.exe`) — špatný `l_addr` v link_map** —
  [postup.md#L2273](postup.md#L2273).
- **claude (Bun standalone) `SyntaxError: Invalid character '\0'` při
  interaktivním startu — VYŘEŠENO** (commit `c3c2d68`): Bun drží embedovaný JS
  v sekci `.bun` a volá na ni `madvise(MADV_DONTNEED)`; `map_elf_segments`
  mapoval segmenty anonymně + `memcpy` → stránky se vrátily jako nuly. Fix:
  celé stránky uvnitř `p_filesz` mapovat ze souboru (`MAP_PRIVATE|MAP_FIXED`).
  A/B 3/3 SyntaxError → 0/3; regresní test `test-all.sh madv`; test-all
  161/0 — [postup.md](postup.md) 2026-10-06 (7).
- **tmux pod loaderem** — [postup.md#L2306](postup.md#L2306).
- **Helper knihovny (`ELF_LOADER_HELPER`), náhodný SIGSEGV ~5 %
  (heap fix), Node ≤22 teardown `free(): invalid pointer` (EXIT=134)** —
  [postup.md#L2337](postup.md#L2337).
- **uv/venv — glibc ELF a absolutní symlink mimo ROOTFS** (managed Python
  pod `/data/.../uv/python/…`; `is_glibc_elf` + `force_redirect` v shimu,
  `resolve_symlinks_under_root` host-cesta fallback; regresní test
  `test-all.sh uv`) — [postup.md#L2529](postup.md#L2529).
- **Entry shebang detekce + symlink chain** (`which -> alternatives ->
  skript` hlásil „Not an ELF file“; `shim_resolve_symlinks` před `open(path)`
  v `main()`; regresní test `test-all.sh symlink`) —
  [postup.md#L2570](postup.md#L2570).
- **`__fxstatat64`/`__fxstatat` 5-arg starý ABI** `(ver, fd, path, buf, flags)`
  — EFAULT („Bad address“) u `stat` s `dir_fd` (Python `os.stat(dir_fd=)`,
  `shutil.rmtree`, PEP517 build wheel); regresní test `test-all.sh fstat` —
  [postup.md#L2606](postup.md#L2606).
- **Test runner přes guest `bash -s` + `renameat`/`unlinkat` path-translace** —
  `run_test` posílal `&&`/`|`/`>` jako argv loaderu (~27 falešných FAILů);
  `mv`/`rm`/`rmdir` ENOENT kvůli chybějícím `renameat2`/`unlinkat`/`rmdirat`
  shimům. PASS 131→157, FAIL 33→7.
- **NSS `/etc/protocols` chyběl v `open64_nocancel` whitelistu** — `ping`
  `unknown protocol icmp` (NSS files backend čte `/etc/protocols` interním
  nocancel voláním; doplněno + `/etc/services`). Opraveny i chybné test-casy
  (timeout/pslog/ping6) a ping/uptime/shred do `should_skip` (host limit).
  **Regresní test** `test-all.sh nss` (`getprotobyname("icmp") == 1` přes
  Python, bez setuid/raw-socket závislosti; v `all`).
  Finálně PASS **160 / FAIL 0**, [postup.md#L2715](postup.md#L2715).
- **`lx` nerespektoval aktivní Python venv** — po `source .venv/bin/activate`
  `lx <konzolový_skript_z_venv>` hlásil „není v Parrot rootfs" (`lxwhich`
  hledalo jen v pevném `$LX_PATH`, `lx()` navíc natvrdo přepisovalo PATH).
  **Vyřešeno** (commit `c5b13e7`): `lxwhich`/`lx` zkouší nejdřív
  `$VIRTUAL_ENV/bin` (s normalizací guest-relativní cesty na `$R`-prefixed).
- **Bionic ELF uvnitř `$ROOTFS` se omylem own-loadoval jako glibc**
  (`ashell` testovací nástroj v `usr/local/bin`) — `shim_execve`/
  `shim_posix_spawnp`'s „uz pod ROOTFS" větev nekontrolovala
  `is_glibc_elf()`. **Vyřešeno** (commity `b840530`, `1737add`): obě větve
  teď ověří ELF magic + `is_glibc_elf()` před zabalením do `--ownall`.
  **Vyřešeno i navazující testovací bug:** `test-all.sh`'s `ashell_rc`/
  `ashell_out` používaly bare `timeout`/`bash`, které se v own-loadovaném
  guestu resolvovaly na HOST `/system/bin/timeout` (PATH dává `/system/bin`
  před rootfs) → RAW exec cíle bez hooků → "Permission denied". Oprava:
  plné cesty `$R/usr/bin/timeout`/`$R/bin/bash`. Zároveň `test-all.sh` teď
  běží přes jediné `ashell -c` (ne jeden na každý test) —
  [postup.md](postup.md) 2026-10-06 (8)/(9).
- **Audit SKIP vs. skutečný FAIL v `test-all.sh` + test heavy binárek a
  pip balíčků z `/usr/bin`** — přehnaně široké substring vzory `*vi*`/`*nc*`
  v `should_skip()` omylem skipovaly funkční `service`/`truncate`/
  `gencat`/`loginctl`; `category_madv` hlásila `no gcc` jen kvůli
  chybějícímu `$R/usr/bin` v PATH top-level bashe. Opraveno (commit
  `9694076`). Dále reálně (přes `ashell -c`, ne `--version`) ověřeny OK:
  `rg`, `eza`, `zoxide`, `openssl`, `bison`, `gpg`, `pandoc`, `batcat`,
  `sq`, `gh`, `git-lfs`; Python `pandas`/`lxml`/`Pillow`/`scipy`/
  `cryptography`/`numpy`. **Nový bug nalezen** (neopraveno): `/proc/self/exe`
  v own-loadovaném procesu vrací cestu k loaderu, ne ke guest binárce —
  rozbíjí self-introspekující nástroje (`cmake` generuje nefunkční
  Makefile s `$(CMAKE_COMMAND)` ukazujícím na neexistující host cestu) —
  [postup.md](postup.md) 2026-10-06 (10).

## 10. Zbývá / otevřené body

- Kosmetika: `src/main.c:149` sign-compare, `\]` escape.
- ~~`/proc/self/exe` v own-loadovaném procesu vrací cestu k loaderu~~ —
  **VYŘEŠENO** (2026-10-07): `run_ownall` ukládá resolvovanou host cestu
  ke guest binárce do `g_guest_exe_path`; `shim_readlink`/`shim_readlinkat`
  rozpoznají `/proc/self/exe` i `/proc/<pid>/exe` a vrátí tuto cestu místo
  volání do skutečného readlink (ten by vrátil loader). Cesta pro `<pid>`
  je sestavena ručně (žádný `snprintf`/`getpid()`) — první verze fixu
  použila oba a rozbila python3 (`import os` SIGSEGV): tyto shimy běží
  patchnuté přímo v glibc kódu pod GUEST TP, takže bionic `snprintf`/`getpid`
  (TLS-dependent) korumpovaly guest TLS. Ověřeno `readlink /proc/self/exe`
  pod `--ownall` → vrací `$ROOTFS/.../binárka` (dřív cestu k loaderu);
  `test-all.sh all` PASS 171/0 (dřív 160, bez regrese). cmake `-S/-B`
  configure fázi už nekrashuje a `CMAKE_COMMAND` v cache je korektní
  guest-relativní cesta (dřív loader). Regresní test `test-all.sh selfexe`.
- ~~GNU Make "fast path" `make: /usr/bin/X: No such file or directory`~~ —
  **VYŘEŠENO** (2026-10-07, (3)): GNU Make 4.4 (gnulib `find_in_given_path`)
  ověřuje program přes **`eaccess()`** PŘED `posix_spawn`; `eaccess` je
  glibc alias `euidaccess` (stejná adresa), ale GOT override jde podle
  jména a `eaccess` v tabulce chyběl (inline hook `euidaccess` se kvůli BTI
  prologu přeskočí) → nepřeložená host cesta → ENOENT → `posix_spawn` se
  vůbec nezavolal. Fix: `elf_register_override("eaccess", shim_euidaccess)`
  v `shim_register_overrides`. Ověřeno: plný `cmake -S/-B` + `cmake --build`
  triviálního projektu pod `--ownall` projde (s guest PATH), A/B regresní
  test `test-all.sh selfexe` (stará binárka FAIL, nová PASS), `all` PASS
  171/0. **Pozor (ladicí past):** `su -c` na tomhle zařízení běží přes
  `proot` (`PROOT_L2S_DIR`), který překládá cesty na úrovni syscallů — bug
  se tam neprojeví; jediný platný test je `ashell -c`. Diagnostika
  `ELF_LOADER_EXEC_TRACE=1` (raw-syscall trace v exec/access shimech) —
  [postup.md](postup.md) 2026-10-07 (2)/(3).
- ~~Funkce s cestou mimo override tabulku~~ — **VYŘEŠENO** (2026-10-07, (4)):
  audit importů 1927 ELF v rootfs; doplněny shimy `chmod/chown/utime*/
  truncate/mknod/mkfifo/linkat/statfs/xattr/creat/pathconf/
  inotify_add_watch` + AF_UNIX `bind`/`connect` (překlad `sun_path`).
  Regresní test `test-all.sh pathops` (A/B). Navíc `mkdtemp` (glibc interní
  `__mkdir` mimo GOT; rozbíjela cmake `try_compile` na guest cestách, apt/
  dpkg, `strip`) — ověřeno reálným projektem cJSON (clone → cmake → build →
  ctest 19/19 s guest cestami) — [postup.md](postup.md) 2026-10-07 (5).
  Dále `remove` (interní `unlink`/`rmdir`), `nftw`/`ftw` + `glob` (kořen/vzor
  přeložen, z callback cest a `gl_pathv` se odřízne prefix `$R`) a
  **inline hook `posix_spawn` (BTI prolog)** → `system()`/`popen()` spouští
  guest `/bin/sh` přes loader místo host `/system/bin/sh` (vypnout
  `F2_NO_SPAWN_HOOK=1`) — [postup.md](postup.md) 2026-10-07 (6).
  **Obecné pravidlo:** glibc 2.41 funkce začínají `BTI c` → `hook_install`
  je nepatchuje → glibc-interní `bl` volání obchází shimy; řešit wrapperem
  na vnější funkci nebo `hook_inline_prologue`.
  Hardlinky (`link`) zakazuje
  SELinux app domény i nativně — platformní limit. **Pravidlo:** GOT override
  je per-jméno; při přidání shimu zkontrolovat aliasy v `libc.so.6`
  (`nm -D`, stejná adresa) a importy binárek (`nm -D --undefined-only`;
  toybox `readelf` relokace nevypisuje).
- Test na reálném 16K Android 15+ zařízení (Task 3).
- ~~Bionic dlerror/errno test (Task 4).~~ **Vyřešeno** — `ldso_dlerror()` +
  guest `dlopen/dlsym/dlerror/dlclose/dladdr` nad `_rtld_global` ověřeno
  (`src/elf_loader.c:1295`, `src/main.c:3185`).
- Node 23+/v26.8.2 JSDispatchTable — plain `node -e` na v26.10.0 už
  nepadá (5/5, 2026-10-06). `pi`+`pi-starship` TTY pád **vyřešen** (viz
  sekce 8, commit `80c3ffd`).
- Síťové binárky — starship, fzf, curl, wget, python `getaddrinfo`+HTTPS,
  getent, ssh **ověřeny OK** (2026-10-06, nativně přes `ashell`, 3×).
  Zbývá jen `nmap` (není v rootfs nainstalovaný) — [postup.md#L680](postup.md#L680).
- ~~`env`/`timeout` spouštějící `ashell`~~ — **vyřešeno**, byl to PATH
  ordering (bare jméno → host binárka), ne hook bypass; viz sekce 9.

Průběžný stav otevřených bugů — viz i auto-paměť
`[[elf-loader-open-bugs]]`.
