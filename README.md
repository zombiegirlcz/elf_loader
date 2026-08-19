# ELF Loader (glibc-binary runner)

Custom ELF64 loader, built on AArch64, that loads glibc binaries into
memory and executes them natively inside an already-running process
(a Bionic/glibc host). It is the core of the ABI-bridging approach for
running Linux/glibc binaries on Android without proot.

## Build & Run

    make
    make test                       # introspect /bin/ls + execute test/hello
    ./elf_loader /path/to/binary    # introspect (base/entry/symbols)
    ./elf_loader --run /path/to/binary [args...]   # execute

## Struktura

    include/elf_loader.h   - API
    src/elf_loader.c       - loader: load, resolve, relocate, run
    src/entry.S            - AArch64 trampoline (set sp, zero regs, br entry)
    src/main.c             - CLI (introspect / --run)
    test/hello.c           - test binary (prints argc/argv)
    test/env.c             - test binary (reads env vars)
    Makefile

## Co umí (hotovo)
- Načte ELF64 PT_LOAD segmenty do paměti, přeloží virtuální adresy
- Načte `.symtab` a `.dynsym` + jejich string tables
- Vyhledá symboly a rozliší defined / import / not found
- Načte DT_NEEDED závislosti přes `dlopen` s hledáním v RPATH/RUNPATH
  (`$ORIGIN`/`$LIB`), `LD_LIBRARY_PATH` a adresáři binárky
- Importy řeší přes `dlsym` z hostitelských knihoven
- Aplikuje relokace (R_AARCH64_RELATIVE / GLOB_DAT / JUMP_SLOT / ABS64)
  a **R_AARCH64_IRELATIVE (ifunc)** — resolver se volá až po dokončení
  všech ostatních relokací a nastavení práv stránek (může volat PLT/importy)
- Sestaví stack (argc/argv/envp) + auxiliary vector (AT_PHDR/ENTRY/BASE...)
- Skok na entry point přes trampolínu, 16B zarovnaný stack
- Reálně spustí glibc binárky: `--run /bin/ls -l`, TLS přes sdílené knihovny
  (dlopen + hostí glibc) funguje
- **Interpozice symbolů** (`--shim`): `elf_register_override()` zaregistruje
  loaderem dodané funkce, které mají přednost před `dlsym` při řešení importů —
  základ pro ABI shim (např. `puts` zachycen a přeposlán na real)
- **Lazy PLT binding** (`--lazy`): GOT sloty JUMP_SLOT relokací ukazují na
  `lazy_plt_stub` (entry.S); po prvním zavolání přes aarch64 PLT protokol
  (x16 = &GOT[n]) resolver `elf_lazy_resolve()` doplní adresu přímo do GOT.
  Eager binding zůstává výchozí (`elf_set_lazy(0/1)`); resolver najde správný
  objekt přes registry (funguje i pro vlastní moduly)
- **Vlastní ELF module loader** (`--own <elf> <shared.so>`): `elf_load_shared()`
  mapuje ET_DYN z filesystému (žádný `dlopen`), rekurzivně načte DT_NEEDED,
  relokuje a přidá do **privátního scopu** (`elf_scope_*`). Importy se řeší
  v pořadí: override → scope → host `dlsym` (přechodný fallback). Základ pro
  „reálný linker" — glibc cluster vedle Bionic bez závislosti na host linkeru.
  Ověřeno na skutečné glibc `libm.so.6` (sin/cos/pow/floor).
- **TLS pro vlastní moduly**: `R_AARCH64_TLSDESC` + `R_AARCH64_TLS_TPREL`
  relokace (včetně TLSDESC v `.rela.plt`). Každý modul s `PT_TLS` dostane
  alokovaný TLS blok; resolver `tlsdesc_return` (entry.S) vrací offset
  relativně k TP → `__thread` v načtených .so funguje (`--own test/uselib
  test/libtls.so` → lib_get=7, after bump=8)

## Co zbývá (další hard parts)
- **Statický TLS vlastní binárky** (TLS bridging). Binárka s vlastním
  `PT_TLS` má offsets napařené do kódu (`TP+0x10` na aarch64/glibc). V tomto
  procesu už hostí glibc zabírá slot `TP+0x10` (loader nemá vlastní TLS, takže
  libc leží hned po TCB). Nativní exe by očekávala svůj blok přesně tam.
  Řešení: alokovat nový TLS region, zkopírovat blok libc na stejnou relativní
  pozici, postavit nový TCB/DTV a přepnout `tpidr_el0`. Vyžaduje i re-relokaci
  TPREL odkazů v libc.so.6 — invazivní, viz poznámka níže.
- Statický TLS **vlastní exe** (TLS bridging — viz výše); TLS modulů je hotový
- Struct shims (pthread_mutex_t 40B vs 4B, sigset_t 128B vs 8B)
- Cross-compile přes Android NDK (aarch64-linux-android31-clang)
- Načítat glibc libs (libc.so.6, libm, ld-linux) **vlastním loaderem** ve
  scopu — dnes deps modulů řeší host `dlopen` (přechodné), exe stále taky

## Poznámka k TLS
TLS přes `dlopen`ované knihovny funguje (hostí glibc to zařídí). Vlastní
statické TLS načtené binárky je tvrdá část ABI bridgingu. V dev prostředí
(proot, hostí glibc) vyžaduje re-relokaci libc. Na cílové platformě (loader
běží vedle Bionic jako "americký spotřebič") budeme od startu řídit TP my, takže
TLS bloky půjdou sestavit jako v pořádném dynamic linkeru.

## Cross-compile (na hostiteli, NE na telefonu)
Build pro Android se dělá na počítači s NDK:
    # stažení NDK + aarch64-linux-android31-clang
    make clean
    make CC=aarch64-linux-android31-clang \
         CFLAGS="-Wall -Wextra -O2 -std=c11 -fPIE -fPIC" \
         LDFLAGS="-ldl -pie"
    adb push elf_loader /data/local/tmp/
    adb shell /data/local/tmp/elf_loader --run /data/local/tmp/hello_glibc

## Inspirace
- libhybris: https://github.com/libhybris/libhybris
  - hybris/src/linker.c (custom loader)
  - hybris/src/hooks_pthread.c (struct bridging)
- System V ABI (AArch64 psABI)
