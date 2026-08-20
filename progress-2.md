# Progress 2 — Cíle další fáze (elf_loader)

Tento soubor shrnuje výsledky kompletního review projektu a rozdeluje doporučené kroky
do fází podle dopadu a nutného úsilí. Cílem je dostat projekt z „fungujícího prototypu
na hranici produkce“ do stabilního základu, který lze dál udržovat a rozšiřovat.

---

## Fáze 0 — Bezpečnostní / kritické fixy (nutné před dalším vývojem)

### 0.1 Hard fail při chybějící libc.so.6 v `--ownall`
- **Problém:** pokud `libc.so.6` není v scope, loader pokračuje a padá až později
  (např. při čtení `mp_` nebo jiné neinicializované struktury).
- **Úkol:** v `run_ownall()` po načtení scopu ověřit přítomnost `libc.so.6` a
  ukončit s exit kódem 1 + chybové hlášení.
- **Dopad:** eliminuje nejčastější segfault v produkčním použití.

### 0.2 Opravit `expand_dirs()` pro prázdný vstup
- **Problém:** `if (o > 0) out[o - 1] = '\0';` → pokud je vstup prázdný, zapisuje
  `out[-1]` (undefined behavior).
- **Úkol:** přidat `if (!*list) { out[0] = '\0'; return out; }` na začátek funkce
  nebo použít bezpečnější zapisovač s kontrolou `o > 0`.
- **Dopad:** bezpečnost + korektní chování při `LD_LIBRARY_PATH=""`.

### 0.3 Opravit `write_heap_veneer()` — nepoužívat 2-page `mprotect`
- **Problém:** mění se práva na 2 stránky místo 1, navíc `0x2000` není
  page-size agnostic.
- **Úkol:** nahradit `0x2000` za `PAGE_SIZE` a použít `mprotect(page, PAGE_SIZE, ...)`.
- **Dopad:** zmenší attack surface a vyřeší edge-case na hraně stránek.

### 0.4 Odstranit `MAP_FIXED` z privátního heapu
- **Problém:** `mmap((void *)0x7f00000000UL, PARROT_HEAP_SIZE, ..., MAP_FIXED, ...)`
  může kolidovat s existující mapou (QEMU, Android, opakované spuštění).
- **Úkol:** použít `mmap(NULL, PARROT_HEAP_SIZE, ..., MAP_PRIVATE | MAP_ANONYMOUS, ...)`
  a uložit base. Fallback na alternativní adresu pouze pokud první `mmap` selže.
- **Dopad:** robustnější start na různých zařízeních / proot verzích.

---

## Fáze 1 — Vyčištění a technický dluh ( zvýšení maintainability )

### 1.1 Odstranit debug instrumentaci z produkčního kódu
- `[dbg]` printy v `map_elf_segments()`, `elf_load()`, `elf_load_shared()`, `run_ownall()`
- `environ-patch bad nm` blok v `elf_load_shared()` (řádky ~1376–1408)
- `maps-dump` v `run_ownall()` a `fault_handler()`
- `ELF_LOADER_DUMP_AUXV`, `ELF_LOADER_DUMP_PHDR` bloky
- `rlimit_data` dump v `main.c`

Poznámka: `fault_handler` by měl zůstat co nejjednodušší, aby nedošlo k deadlocku
u `fprintf`/`malloc` po SIGSEGV. Pokud je potřeba ladění, použít env-gated
printy do `/proc/self/fd/2` (stderr) nebo do ring bufferu.

### 1.2 Opravit sign-compare warning v `main.c`
- Řádek 149: `int ai` vs `size_t` → převod na správný typ.

### 1.3 Centralizovat magic numbers glibc do `glibc_offsets.h`
- **Problém:** hardcoded offsety jako `libc+0x1be009` (strerror flag), `libc+0x1b6760`
  (mp_), `libc+0x1b0a40` (mp_ v run_ownall) jsou závislé na verzi glibc (2.39).
- **Úkol:** vytvořit `include/glibc_offsets.h` s pojmenovanými konstantami a
  commentem `/* glibc 2.39 */`. Při upgrade glibc stačí upravit tento soubor.
- **Dopad:** usnadní portování na novější glibc / jiné distribuce.

### 1.4 Opravit `AT_EXECFN`
- **Problém:** `AT_EXECFN` obsahuje `argv[0]` (může být relativní cesta, `./prog`).
- **Úkol:** uložit absolutní cestu při `elf_load()` (`origin_dir` + basename) a
  použít ji v `elf_run()` pro `AT_EXECFN`.
- **Dopad:** `readlink("/proc/self/exe")` a programy analyzující vlastní cestu
  budou fungovat správně.

### 1.5 Přidovat kontrolu návratových hodnot `malloc` v `dlopen_search()` a `find_in_paths()`
- **Problém:** `malloc()` bez kontroly → `NULL` → `memcpy`/`strcpy` segfault v OOM.
- **Úkol:** po každém `malloc` přidat `if (!ptr) { free(...); return NULL; }`.

---

## Fáze 2 — Výkon a škálovatelnost

### 2.1 Nahradit lineární `elf_scope_find()` hashmapou
- **Problém:** O(n * m) při resolution importů. U 64+ modulů a tisících symbolech
  se začíná cítit při startu `--ownall`.
- **Úkol:** přidat `elf_scope_hash` (např. 256 bucketů, djb2/xxhash) vedle
  `elf_scope_t`. Při `elf_scope_add()` indexovat; `elf_scope_find()` použít hash.
- **Dopad:** start `--ownall` zrychlí o řád.

### 2.2 Rozdělit `src/elf_loader.c` na moduly
- **Návrh:**
  - `loader/load.c` — mmap segmentů, dynamic parsing, `elf_load()`, `elf_load_shared()`
  - `loader/reloc.c` — `elf_relocate()`, RELR, IRELATIVE, apply_segment_prots
  - `loader/symbol.c` — `elf_resolve_symbol()`, `elf_resolve_import()`, overrides,
    scope, `resolve_jmp_symbol()`, lazy resolve
  - `loader/tls.c` — `elf_setup_own_tls()`, `elf_teardown_own_tls()`,
    `relocate_tls()`, `tlsdesc_return`
  - `loader/run.c` — `elf_run()`, stack setup, auxv, `jump_to_entry` wrapper
  - `ldso/emul.c` — `ldso_setup()`, link_map, `_dl_find_dso_for_object`,
    `_dl_find_object`, `_dl_catch_exception`
  - `host/host.c` — `ldso_private_heap_init()`, `ldso_sbrk/brk`, `write_heap_veneer()`,
    `patch_module_heap_syms()`, `tunable_*`
- **Dopad:** lepší navigace, paralelní vývoj, snazší code review.

### 2.3 Přidat CI (continuous integration)
- **Úkol:** nastavit GitHub Actions / self-hosted runner s `make test` + `--ownall`
  baterií. Testy by měly běžet na:
  - nativním hostu (proot) — `make test` + seznam `--ownall` binárek
  - pokud možno na Androidu přes ADB (emulátor nebo fyzické zařízení)
- **Dopad:** eliminuje regrese při každém commitu.

---

## Fáze 3 — Kompatibilita a hard parts

### 3.1 Struct shims (pthread, signals, …)
- **Problém:** Bionic a glibc mají různé velikosti struktur (`pthread_mutex_t` 4 B
  vs 40 B, `sigset_t` 8 B vs 128 B, `pthread_cond_t`, `pthread_rwlock_t`, atd.).
- **Úkol:** implementovat wrapper struktury, které mapují glibc očekávání na
  bionic API. Inspirace: libhybris `hooks_pthread.c`.
- **Dopad:** umožní běh vícevláknových programů s pthread.

### 3.2 Fork / exec cizích binárek na Androidu
- **Problém:** `fork` + `exec` selhává na Androidu, protože `/lib/ld-linux-aarch64.so.1`
  neexistuje (bionic).
- **Úkol:** buď implementovat `posix_spawn` fallback, nebo přidat wrapper,
  který předá správný `PT_INTERP` / napodobí execve.
- **Dopad:** umožní běh shell skriptů a pipeline na Androidu.

### 3.3 Načíst glibc libs vlastním loaderem (bez host `dlopen`)
- **Problém:** dnes `--ownall` používá host `dlopen` pro vyřešení `DT_NEEDED` pokud
  vlastní scope selže. Cílem je kompletní vlastní linking.
- **Úkol:** rozšířit `elf_load_shared()` aby načítal `libc.so.6`, `libm.so.6`,
  `ld-linux-aarch64.so.1` do privátního scopu.
- **Dopad:** zmenší závislost na hostiteli, zvýší stabilitu.

---

## Fáze 4 — Produkce a uživatelská zkušenost

### 4.1 Dokumentace „Co umí / Co neumí“
- Vytvořit `COMPATIBILITY.md` nebo rozšířit `README.md` o:
  - seznam podporovaných binárek a jejich verze
  - známé omezení (fork/exec, struct shims, GNU extensions)
  - požadavky na glibc verzi / Android API level

### 4.2 Magisk modul — opravit `customize.sh` při upgrade
- **Problém:** `customize.sh` se nespustí při upgradu modulu, jen při čisté instalaci.
- **Úkol:** přidat `post-fs-data.sh` nebo `service.sh` logic, která zajistí
  `/data/adb/parrot_root` i při upgrade.
- **Dopad:** lepší UX pro uživatele aktualizující modul.

### 4.3 Namespace workaround — robustnější řešení
- **Problém:** Magisk mount nevidí app namespace → nutné kopírovat do `/data/adb/`.
- **Úkol:** investigate Zygisk nebo su spawn pro automatické nasazení `elf` wrapperu
  do `/data/local/tmp` / app namespace.

---

## Shrnutí fází

| Fáze | Název | Očekávaný dopad | Úsilí |
|------|-------|-----------------|-------|
| 0 | Kritické fixy | Bezpečnost, stabilita | nízké |
| 1 | Vyčištění + technický dluh | Maintainability | střední |
| 2 | Výkon + škálovatelnost | Rychlejší start, CI | střední |
| 3 | Kompatibilita | Hlavní barrier pro produkci | vysoké |
| 4 | Produkce + UX | Uživatelská zkušenost | střední |

Doporučené pořadí: **Fáze 0 → Fáze 1 → Fáze 2** (v tomto pořadí,
protoči zvyšuje stabilitu a snižuje entropy pro další práci).
Fáze 3 a 4 lze rozběhat paralelně, ale 3.1 (struct shims) je kritické
pro širokou kompatibilitu.
