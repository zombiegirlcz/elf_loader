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
- **Host `.zshrc` — `lx` toolkit (kompletní zápis)**: proměnné/cesty,
  prompt, `lx`/`lxq`/`lxdbg`/`lxhelper`/`lxlog`/`lxdiag`/`lxinfo`/`lxfault`,
  `lxtest` regresní sada, `command_not_found_handler`, `help`, starship/
  zoxide integrace (cache + `/proc/self/exe` rewrite) — soubor žije jen
  na zařízení, není v gitu — [postup.md#L2425](postup.md#L2425).

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

## 9. Ostatní binárky a poslední fixy

- **Bun (`claude.exe`) — špatný `l_addr` v link_map** —
  [postup.md#L2273](postup.md#L2273).
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

## 10. Zbývá / otevřené body

- Kosmetika: `src/main.c:149` sign-compare, `\]` escape.
- Test na reálném 16K Android 15+ zařízení (Task 3).
- Bionic dlerror/errno test (Task 4).
- Node 23+/v26.8.2 JSDispatchTable — otevřeno; viz pokračování 12–14
  [postup.md#L1883](postup.md#L1883)+.
- Síťové binárky (nmap, starship, fzf) — SIGSEGV pod bionic hostem
  [postup.md#L680](postup.md#L680).

Průběžný stav otevřených bugů — viz i auto-paměť
`[[elf-loader-open-bugs]]`.
