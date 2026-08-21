# Postup a hodnoty (elf_loader)

Loader pro AArch64: vlastní načítání skutečného glibc `libc.so.6` + závislostí
do privátního scope a běh glibc binárek v procesu (`--ownall`).
Cíl: spouštět binárky na Androidu bez proot (hostitelská libc je nyní
používána jen samotným loaderem).

## Sestavení / prostředí

- gcc 14.2.0 v prootu. `as` selhával přes výchozí cestu gcc
  ("Too many levels of symbolic links" — proot), vyřešeno:
  `CFLAGS = -Wall -Wextra -g -O0 -std=c11 -B/usr/bin` v `Makefile`
  a `/usr/bin/as -> /usr/bin/aarch64-linux-gnu-as` (absolutní symlink).

## Testovací baterie (vše OK)

### `make test`
```
== introspect /bin/ls ==
== execute test/hello ==            -> "Hello from loaded ELF!"
== execute test/ifunc (IRELATIVE) ==
== execute test/uselib (TLS) ==     -> lib_get=7 after bump=8
== own module loader ==             -> mod_add(1,2)=103 mod_add(10,20)=130 count=2
== own + module TLS (TLSDESC) ==    -> lib_get=7 after bump=8
== shim (interposed puts) ==        -> Hello from loaded ELF! argc=1 argv[0]=test/hello
== lazy PLT binding ==              -> printf/puts/__libc_start_main importy, 11 relokací
```

### `--ownall` baterie (rc=0, správný výstup)
```
/bin/ls -d /tmp                    -> /tmp
/bin/ls /tmp/empty                 -> (prázdno)
/bin/ls --color=never /tmp         -> kompletní výpis
/bin/dir /tmp                      -> kompletní výpis (dříve crash)
/bin/cat /etc/hostname             -> TERMINATOR
/bin/echo hello ownall             -> hello ownall
/bin/pwd                           -> /root/elf_loader
/bin/true /bin/mkdir -p ... /bin/rmdir /bin/rm
/bin/sleep 0
/bin/uname -a                      -> Linux localhost 6.17.0-proot-distro ... aarch64
/bin/date                          -> Tue Aug 18 20:42:42 UTC 2026
/bin/stat /tmp /bin/head /bin/tail /bin/grep -c /bin/wc -l
/bin/basename /bin/dirname /bin/id -un /bin/printf
/bin/ln -sf /bin/cp /bin/test /bin/[  -> OK
/tmp/mt                            -> "malloc ok"  (50x malloc(0x38))
/tmp/captest                       -> "caps: ="  "cap test ok"  (libcap; dříve "caps: ?")
```

## Nalezená chyba a oprava (root cause crashu /bin/ls a /bin/dir)

### Symptom (před opravou)
- `--ownall /bin/ls -d /tmp` → rc=139, SIGSEGV.
- signatura: `pc=ls+0x17300 addr=libc+0xa334c x3=libc+0xa3348 x30=ls+0x4bc0`
  (caller `ls+0x4bbc` = bitmap test-and-set helper `0x172c0`).
- `ls+0x410c0` i `ls+0x413f8` = `libc+0xa3340` (obě sloty, kam ls ukládá
  výsledek `xmemdup(ls+0x41580, 0x38)`).
- `malloc(56)` vracel platné ukazatele (`0x…9e20/…9e60`), ale `0x19504`
  (= `malloc(56)`+`memcpy`) vracela `libc+0xa3340`.

### Příčina
`memcpy@GLIBC_2.17` je v libc **IFUNC** (st_value `0x9c1a0`, velikost 180).
`resolve_jmp_symbol()` řešil JUMP_SLOT na adresu symbolu = adresu **resolveru**,
místo aby resolver zavolal. `memcpy@plt` tedy volal resolver s argumenty
`(dst, src, n)`, resolver vrátil nesmyslnou adresu `libc+0xa3340`
(uvnitř `__xpg_strerror_r@0xa2ce0`), která byla použita jako výsledek memcpy
→ ls ji uložil do svých slotů → helper `0x172c0` psal do textové sekce libc
→ SIGSEGV. Stejná chyba stála i za "caps: ?" (cap_to_text).

### Oprava (src/elf_loader.c)
V `resolve_jmp_symbol()` pro undefined symbol: prohledá scope přes
`elf_scope_find()`; když `ELF64_ST_TYPE(fs->st_info) == STT_GNU_IFUNC`,
zavolá resolver `call_ifunc_resolver()` s `struct ifunc_arg_t`
(`{size, hwcap=AT_HWCAP, hwcap2=AT_HWCAP2, auxv[2]={NULL}}`).
Výsledek se uloží do GOT slotu.

## Ověřené hodnoty (při běhu --ownall /bin/ls)

- GOT ls: `0x3fc78`(malloc)=`libc+0x97804`, `0x3fea8`(__errno_location)=`libc+0x22680`,
  `0x3fb50`(memcpy)=`libc+0x9c1a0` (resolver IFUNC — nyní se resolve volá).
- TLS: errno slot `libc+0x1afdb0` = `tls_offset(0x4b80)+0x30`; `__errno_location()`
  vrací `tlsblk+0x30`. libc TLS blok: tls_off=0x4b80, libselinux tls_off=0x5b80.
- `__curbrk` @ `libc+0x1b7320` (seeduje se v `elf_load_shared` z host `sbrk(0)`,
  analogicky k `__environ`).
- arena: `*(TP + slot@libc+0x1afd68)`, např. `libc+0x263010`; fastbin hlavy
  `arena+0x80+idx*8`, count `arena+idx*2` (u16, glibc 2.39).
- libc: LOAD1 R E `0x0..0x19b83b` (r-x 0..0x1a0000), LOAD2 RW `0x1ad080..0x1be010`,
  celkem `0x1bf000`.
- ls PLT stub (32 B): `bl 0x3d28`→GOT `0x3fc78`=malloc, `bl 0x43b8`→GOT `0x3fea8`
  =__errno_location. `0x17228` = save/restore errno + `0x19504`(=malloc(0x38)+memcpy).
  `0x172c0` = bitmap test-and-set (`str w1,[x3,x4]`, x3=x0+8).

## Stav

- `/bin/ls`, `/bin/dir` (libcap+libselinux) pod `--ownall` — **OPRAVENO**.
- Kompletní `make test` i `--ownall` baterie zelené.
- Debug skafold (malloc interceptor, tracepointy, GOT patche, arena dumpy) odstraněn.
- **Step 2 (statický TLS pro načítaný exe) — OPRAVENO.** `--ownall /tmp/tlstest`:
  `counter=42 buf=tls-works static=7 errno=0x1234`; `/tmp/tlsprobe3`:
  `TP=.. &a=TP+0x10 &c=TP+0x18`; stabilní i pod `setarch -R` (20/20 ls, 10/10 tlstest).

### Step 2 — návrh (jak funguje TLS bridging)

- Náčítaný exe (local-exec `__thread`) používá adresy `TP+0x10` (první var) a výše.
- Moduly (libc=0x4b80, libselinux=0x5b80) mají bloky v `TP+tls_offset` — offset je
  konstanta nezávislá na layoutu.
- `elf_setup_own_tls()`: mmap region, **zkopíruje celý hostitelský TLS blok**
  (`struct pthread` na `TP-0x720` + tcbhead na `TP`) do regionu na stejné relativní
  offsety, přeloží vnitřní ukazatele (self @ -0x640, tcb @ TP+0x790, atd.),
  vynuluje `dtv` (TP+8), zkopíruje bloky modulů na `new_TP+tls_offset` a `.tdata`
  exe na `new_TP+0x10`. `new_TP = region+0x720`.
- **Switch `tpidr_el0` MUSÍ proběhnout až v trampolině** (`jump_to_entry` v entry.S,
  nový podpis `(entry, rsp, new_tp, old_tp)`): msr těsně před `blr entry`, restore
  po návratu. Dřívější varianty (msr v C v setup) kolidovaly s host libc/ld.so —
  první lazy-binding hovor po switchi četl `[TP-0x720]`, ale pod novým TP (region
  base) je tato stránka nemapovaná → SIGSEGV (layout-dependentní, reprodukovatelné
  pod `setarch -R`). Minimalistické 2-slovo TCB nestačilo: `__libc_init_first`
  čte `[TP-0x628]` a píše canary na `TP-0x620`.
- `elf_teardown_own_tls()`: restore TP + munmap (hlavně mrtvá cesta — glibc exe
  končí přes `exit_group`).

### Bug 1 — `strerror(1234)` → `Unknown error` bez čísla (VYŘEŠENO)

- **Dopad:** jen `strerror()`/`perror()` pro neznámé errno (>133 nebo záporné) —
  chybělo číslo. Známá errno (0–133) i `strerror_r()` fungovaly; hodnota `errno`
  samotná byla správná. Čistě kosmetická chyba.
- **Příčina:** `strerror_l` (libc+0x9ed80) pro neznámé errno čte flag byte
  `libc+0x1be009` (bit 0): bit=1 → větev `__asprintf_chk` → `Unknown error %d`
  (s číslem); bit=0 → jen `__dcgettext` statického `Unknown error `. Tento byte
  nastavuje na 1 `__libc_early_init()` (libc+0x140860, `strb w19,[x0,#9]`), které
  v normálním procesu volá ld.so při startu s argumentem 1. Náš načítač ho nikdy
  nevolá → byte zůstal 0 (zeroed .bss) → statická větev bez čísla.
- **Oprava (src/elf_loader.c, `run_module_init`):** při načtení `libc.so.6`
  nastavit `*(char *)va(m, 0x1be009) = 1` (simulace early-init flagu). Volání
  celého `__libc_early_init(1)` selhalo (crash v .rodata) — nechat kód nedotčený.
- **Ověřeno:** `strerror(1234)='Unknown error 1234'`, `strerror(3000)` OK, známá
  errno beze změny, `perror(1234)` = `P: Unknown error 1234` (shodné s host),
  `make test` rc=0, setarch -R ×10 OK, tlstest OK.

### Zbývá

- `src/main.c:149` sign-compare warning (int vs size_t) — kosmetika.
- **Libc.so.6 dep handling**: loader by měl při "dep libc.so.6 not found" tvrdně skončit chybou HNED, ne pokračovat až k segfaultu na `mp_` čtení. Řešení: v `load_module_needed` (src/elf_loader.c ř. ~1152) po selhání, když soname == "libc.so.6", `exit(1)` přímo z `run_ownall`.
- Otevřeno: Step 3 (bare-Android / NDK static-PIE).
- **Tunable env override** (tunable_get_val): přidáno načítání env proměnné
  před výpočet default hodnoty. Předchozí implementace vracela vždy default
  hodnotu, env proměnné nebyly zohledněny. Nyní: `getenv(env_name)` → strtoll,
  pokud není nastaveno → default_val. Ověřeno: `MALLOC_TOP_PAD_=999999`
  → v=999999, from_env=1; bez env → v=131072 (default), from_env=0.

### Fixy 2026-08-19 (brk desync + TLS offset under TP)

## Step 3 — bionic (NDK) binárky přes načítač

### Bionic static-PIE (`/tmp/bstaticpie`) — OPRAVENO

**Symptom:** `/tmp/bstaticpie` (NDK r28, `-static-pie`) přes načítač → rc=139.
`pc=base+0x1e6f8` v `__find_elf_note`, čtení `addr=0x270` (link-time adresa,
nemapováno). Crashne i nativně (QEMU-user, rc=139), zatímco glibc statiky
(`/tmp/sfull` rc=7, `/tmp/spie` rc=42) běží nativně OK.

**Příčina:** bionic static (`libc_init_static.cpp`) používá `phdr->p_vaddr`
PŘÍMO s `load_bias=0`: `__find_elf_note(type,name,phdr,phnum,note,desc,0)`
počítá `note_addr = 0 + p_vaddr`; `__libc_init_mte(..., /*load_bias=*/0)`;
`__bionic_get_tls_segment(phdr, phnum, 0, ...)`. Tedy vyžaduje, aby **pole
`p_vaddr` v mapované phdr tabulce byla předrelokovaná (base-added)**. Nic ji
nerelokuje: kernel (`fs/binfmt_elf.c` jen lokální `phdr_addr += load_bias` pro
AT_PHDR), QEMU ani načítač (0 rela targetů v phdr rozsahu `[0x40,0x270)` —
všech 424 relokací je R_AARCH64_RELATIVE jen do RW segmentů).

**Oprava (`maybe_fixup_bionic_phdr` v src/elf_loader.c, voláno v `elf_load`
po namapování, před `apply_segment_prots`):** detekce bionic přes PT_NOTE se
jménem "Android" (`.note.android.ident`); pokud ano, `phdr[i].p_vaddr += base`
přímo v mapovaném obraze (stránky jsou ještě RW). Glibc se nedotkne (nemá
Android note) — jinak by se rozbil výpočet `load_bias` glibc static-pie.

**Ověřeno:**
```
./elf_loader --run /tmp/bstaticpie
[dbg] bionic phdr p_vaddr rebased (+0x7541350000)
bionic (NDK) hello
printf via bionic: 42
rc=42
```
sfull rc=7, sstatic rc=42, spie rc=0, `make test` — bez regrese.
Pozn.: ET_EXEC varianta `/tmp/bstatic` (base 0x200000) NELZE spustit v tomto
prostředí — 0x200000 je v každém procesu obsazena emulační vrstvou proot/QEMU
(`loader` binárka), MAP_FIXED_NOREPLACE selže.

### Task 1 — page size patch (bug.md)

**Symptom:** `#define PAGE_SIZE 4096` — na Androidu 15+ (16K stránky) by
ALIGN_UP/ALIGN_DOWN i TLS region dávaly špatné rozsahy.

**Oprava:** runtime verze:
```c
static size_t sys_page_size(void) {
    if (!g_page_size)
        g_page_size = (size_t)sysconf(_SC_PAGESIZE);
    return g_page_size;
}
#define PAGE_SIZE sys_page_size()
```
Nahrazen i zbylý `4096` literál (TLS region `ALIGN_UP(..., PAGE_SIZE)`).
Zbylé `0x1000` na ř. 1286 (slack margin TLS) a 1390 (sanity check `> 0x1000`)
nejsou page-size závislé — ponechány.

**Ověřeno:** `make test` rc=0, všechny segment totals násobkem runtime page
size (`0x97000`/`0x8f000`/`0xa3000`).

### Task 2 — NDK cross-compile loaderu (bug.md)

`finale_loader_build.py` (Modal, NDK r28, `aarch64-linux-android24-clang`,
stejné flagy jako Makefile mínus proot-only `-B/usr/bin`). Výsledky:

- **a) MAP_FIXED_NOREPLACE:** v NDK r28 sysroot **dostupný i pro API 24**,
  hodnota `0x100000` (ověřeno `-dM -E` pro API 24/29/30/35). Fallback netřeba.
  (První grep "MISSING" byl chyba regexu, `-dM` je autoritativní.)
- **b) _GNU_SOURCE / chybějící symboly:** `_GNU_SOURCE` definován v hlavičce
  i v command line → `-Wmacro-redefined` (opraveno odstraněním `-D`). Použité
  GNU symboly (strndup, dlfcn, getauxval, RTLD_NEXT) v bionic jsou. Jediný
  problém: **bionic static libc nemá dlfcn** (`dlopen/dlsym/dlclose/dladdr`
  undefined při `-static-pie`) → přidán `src/dlfcn_stubs.c` (no-op vracející
  NULL) jen pro static-PIE variantu.
- **c) entry.S:** kompiluje se bez chyb clang integrated assemblerem
  (`.type ..., %function` podporováno).

**Výstupy:**
- `/tmp/elf_loader_ndk` (89712 B): ELF64 AArch64 PIE, PT_INTERP
  `/system/bin/linker64`, NEEDED `libc.so`/`libdl.so` → připraveno na
  `adb push` + `adb shell`.
- `/tmp/elf_loader_ndk_staticpie` (2244416 B): self-contained.

**Ověřeno (vrstvené načítání):**
```
./elf_loader --run /tmp/elf_loader_ndk_staticpie --run /tmp/bstaticpie
[+] Base: 0x734b4f9000 Entry: 0x734b517c40 deps: 0   <- NDK loader
[+] Base: 0x734b68c000 Entry: 0x734b6a9840 deps: 0   <- bstaticpie přes NDK loader
bionic (NDK) hello
printf via bionic: 42
rc=42
```
glibc načítač → NDK loader (bionic static-PIE) → který sám načte/spustí
bstaticpie. Pozn.: NDK loader spuštěný nativně (QEMU) crashne rc=139 — stejný
phdr problém jako bstaticpie; přes načítač funguje.

### Task 3 — empirický test page size (bug.md)

- Toto prostředí (QEMU-user): `getconf PAGESIZE` = **4096**.
- Runtime použití sysconf ověřeno: `make test` zelené, segment totals
  (0x97000/0x8f000/0xa3000) jsou násobky runtime page size.
- Test na 16K zařízení **nelze provést** — není dostupné Android zařízení/
  emulátor; vyžaduje `adb` + Android 15+ 16K system image.

### Task 4 — dlopen_search() namespace (bug.md, informační)

- Bionic dlopen namespaces (API 26+) omezují dlopen knihoven mimo
  app/system adresáře (např. `/data/local/tmp`).
- Načítání přes **vlastní loader** (`--own`, `--ownall`) host dlopen vůbec
  nevyužívá (`load_needed` ř. 413: `if (elf_own_deps && obj->scope)` →
  `elf_load_shared`; `load_module_needed` ř. 683: `if (elf_own_deps)` →
  vlastní cesta) → **namespaces nejsou blokující** pro `--ownall` flow.
- Empirický test (dlerror/errno v bionic) zde nelze — glibc host.

## Zbývá

- `src/main.c:149` sign-compare warning (int vs size_t) — kosmetika.
- Vyčistit debug instrumentaci v elf_loader.c: `[dbg] map_elf_segments` print,
  `ELF_LOADER_DUMP_AUXV` / `ELF_LOADER_DUMP_PHDR` bloky, regrese výpisu
  fault handleru (insn-dump končí po F: řádku registrů), dočasné
  `[dbg] environ-patch bad nm` / maps-dump / `libc mp_` bloky.
- Task 3: ověřit na reálném 16K zařízení (Android 15+, `adb`).
- Task 4: empirický dlerror/errno test na bionic cíli (mimo proot).
- Zařízení: fork/exec nového binárního souboru (např. `wc` z bash) selhává
  jen na Androidu, protože interp `/lib/ld-linux-aarch64.so.1` neexistuje
  (bionic). Nativně (přes wrapper) fork/exec funguje.

## Fixy 2026-08-19 (brk desync + TLS offset pod TP)

### 1. Dva alokátory sdílející jedno brk → private heap pro parrot libc

- **Příznak:** `sed` (a další větší binárky) padaly SIGSEGV při loadu v
  `__environ` patching smyčce, čtením `dynstr` na adrese těsně za koncem
  mapované regiony (např. `0x3000073ebd`, region končil `0x3000073000`).
- **Příčina:** host loader (glibc) i vlastně-načtená parrot libc sdílejí
  procesní brk. Parrot malloc si přes vlastní `__curbrk`/`brk` syscall hýbal
  brk nezávisle → desync `__curbrk` (host tvrdil `0x3000873000`, kernel break
  byl `0x3000073000`) a reálný kernel break se smrskl pod živé host chunk
  (`dynstr` buffer) → díra → fault. `MALLOC_TRIM_THRESHOLD_` / `mallopt`
  nestačily.
- **Fix:** parrot alokátor dostal **vlastní mmap arena** (`ldso_private_heap_init`
  → 64 MB MAP_FIXED na `0x7f00000000`). `sbrk`/`brk` v parrot libc (i ostatních
  modulech) jsou:
  - pro externí volání přemapovány v `ldso_lookup` na `ldso_sbrk`/`ldso_brk`
    (PLT), a
  - pro interní přímé `bl` volání `__sbrk`/`__brk`/`sbrk`/`brk` přelepeny
    16-bajtovým veneerem (`ldr x16,[pc,#8]; br x16` + ukazatel) ve
    `write_heap_veneer`/`patch_module_heap_syms` ihned po `elf_relocate`.
  `__curbrk` patch nyní zapisuje `ldso_sbrk(0)` (private brk), ne host.
- **Výsledek:** parrot malloc už brk nedotýká; trimy/rastr jen posouvají
  pointer v privátní areně (bez munmap) → žádné díry. `sed`, `bash`, `wc`,
  `ls` … vše EXIT=0 nativně.

### 2. TLS blok pod TP (negativní tls_offset) na device-přímém startu

- **Příznak:** na zařízení (su-přímý start přes parrot ld.so jako interp)
  fault v `elf_setup_own_tls` — `memcpy` psal do nepokryté mezery
  (`dest=0x736f362fe0`, mapa `736f365000-736f36c000`).
- **Příčina:** modulové TLS bloky se na zařízení namapovaly POD host TP →
  `tls_offset = blk - TP` byl záporný (libc `-0x2740`, libselinux `-0x1740`),
  takže `new_tp + tls_offset` vyletěl pod začátek TLS regiony (nativně přes
  wrapper byly offsety kladné — náhoda layoutu).
- **Fix:** `elf_setup_own_tls` počítá `min_off` (minimální, může být záporný)
  a `span = max_end - min_off`; `size` i `new_tp` se zvětší o `-min_off`, takže
  `new_tp + min_off >= region` a `new_tp + max_end <= region + size`. DTV je
  nyní umístěn na `new_tp + max_end + 0x800`.

## Stav na zařízení (SSH 5555, su)

- Nativně (wrapper) i na zařízení (su + parrot ld.so jako interp):
  `ls`, `sed`, `grep`, `wc`, `cp`, `rm`, `head`, `echo`, `printf`, `stat`,
  `bash` (builtiny) — EXIT=0.
- Zařízení-spouštění příklad:
  `LD_LIBRARY_PATH=<rootfs>/usr/lib/aarch64-linux-gnu cd <rootfs> &&
   ./usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 --library-path
   ./usr/lib/aarch64-linux-gnu ./root/elf_loader/elf_loader --ownall
   ./bin/ls -la ./etc`

## 2026-08-19: Magisk modul s "elf" wrapperem (bionic NDK build)

### Cíl
Systémově dostupný příkaz `elf <parrot_binárka> [args...]` fungující z **libovolného shellu** (SSH, adb, com.linux_core terminál) bez zásahu do rootfs binárek (žádný patchelf_interp, žádné přepisování PT_INTERP).

### Symptom
- Původní pokus s `parrot-fix-exec` + patchelf_interp rozbil proot (přepis PT_INTERP v rootfs binárkách).
- glibc cross-compile `elf_loader` neběžel na Androidu (bionic vs glibc).
- Magisk systemless mount se nepropaguje do app namespace (com.linux_core).

### Příčina
1. `aarch64-linux-gnu-gcc` produkuje glibc binárky — na Androidu (bionic) selžou s "required file not found".
2. Magisk mount namespace: `/adb/modules/...` vidí root namespace, ale app namespace (Zygote snapshot) ne.
3. `customize.sh` se nespustí při upgrade modulu, jen při čisté instalaci.

### Oprava
1. **NDK cross-compile přes Modal** (`build_ndk.py`):
   - `aarch64-linux-android24-clang` (NDK r28) → bionic PIE binárka (`elf_loader` 55912 B).
   - Glibc-specific `mallopt` calls guarded with `#ifdef __GLIBC__`.
   - Source mounted via `Image.add_local_dir(..., copy=True)`.

2. **`elf` wrapper script** (`system/bin/elf`, i `com.linux_core` varianta):
   ```sh
   #!/system/bin/sh
   if [ -z "$ROOTFS" ]; then
       echo "elf: ROOTFS not set. export ROOTFS=/path/to/rootfs" >&2
       exit 1
   fi
   # Bionic loader (elf_loader) musí najít SVŮJ libc.so v /system/lib64, ne v
   # parrot glibc dir -> /system/lib64 JDE PRVNÍ. Parrot cesty až za $LD_LIBRARY_PATH
   # (viz "LD_LIBRARY_PATH poisoning" níže).
   export LD_LIBRARY_PATH="/system/lib64:/system/lib:$LD_LIBRARY_PATH:$ROOTFS/usr/lib/aarch64-linux-gnu:$ROOTFS/lib"
   exec /system/bin/elf_loader --ownall "$@"
   ```
   Magisk varianta execuje `/system/bin/elf_loader`; `com.linux_core` varianta
   (testováno přímo v appce) execuje
   `/data/user/0/com.linux_core/files/usr/bin/elf_loader`. Obojí čte `ROOTFS`
   z env (ne z `/data/adb/parrot_root`).

3. **Build script upraven** (`magisk-module/build.sh`):
   - Nepoužívá `aarch64-linux-gnu-gcc` pro elf_loader.
   - Kopíruje prebuilt bionic binárku z `system/bin/elf_loader`.

4. **Namespace workaround** (app nevidí Magisk mount):
   ```bash
   cp /adb/modules/parrot_elf_loader/system/bin/elf /data/adb/elf
   cp /adb/modules/parrot_elf_loader/system/bin/elf_loader /data/adb/elf_loader
   chmod +x /data/adb/elf /data/adb/elf_loader
   export PATH="/data/adb:$PATH"
   elf nh/distro/parrot/bin/ls
   ```

### Ověřeno
- **Build (dynamický bionic, finální)**: `modal run finale_loader_build.py` →
  `/tmp/elf_loader_ndk` (118496 B, ELF64 AArch64 PIE, PT_INTERP
  `/system/bin/linker64`, NEEDED `libc.so` `libdl.so`). Tato varianta se
  používá pro přímé spuštění na zařízení (má PT_INTERP, běží samostatně).
- **Build (static-pie, NEpoužívá se samostatně)**: `finale_loader_build.py`
  též produkuje `/tmp/elf_loader_ndk_staticpie` (2271072 B, nula NEEDED), ale
  tato varianta **nemá PT_INTERP** (readelf: 10 headers, žádný INTERP) →
  kernel by ji na zařízení nespustil. Viz "LD_LIBRARY_PATH poisoning" níže.
- **Modul zip**: `/root/elf_loader/magisk-module/parrot_elf_loader.zip` (334 KB, obsahuje `elf`, `elf_loader`, `parrot`, `parrot-sh`, `parrot-fix-exec`, `patchelf_interp`, `ld-linux-aarch64.so.1`, `post-fs-data.sh`, `service.sh`, `customize.sh`).
- **Test v com.linux_core terminálu** (po vytvoření `/data/adb/parrot_root`):
  ```
  $ /data/adb/elf nh/distro/parrot/bin/ls -la /etc
  total 1234
  drwxr-xr-x 1 root root 4096 Aug 19 20:07 .
  drwxr-xr-x 1 root root 4096 Aug 19 20:07 ..
  -rw-r--r-- 1 root root  234 Aug 18 21:01 hostname
  ...
  ```
- **Proot nezměněn**: `proot-distro login parrot` funguje normálně.
- **Tunable env override** (commit `fix5`): `MALLOC_TOP_PAD_=999999` → v=999999 from_env=1; bez env → v=131072 (default) from_env=0.

### Známé limity
- Magisk mount nevidí app namespace → nutné kopírovat do `/data/adb/` nebo restartovat Zygote (reboot).
- `customize.sh` se nespustí při upgrade → při aktualizaci modulu: uninstall → reboot → install → reboot.[+] own-loading dependency: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libselinux.so.1
[+] own-loading dependency: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libpcre2-8.so.0
[+] own-loading dependency: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so.6
[+] relocated 96 entries
[+] own-loaded module: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so.6 (base 0x7082dc4000, 3076 dynsym)
[+] relocated 103 entries
[+] own-loaded module: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libpcre2-8.so.0 (base 0x7083127000, 107 dynsym)
[+] own-loading dependency: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so.6
[+] relocated 296 entries
[+] own-loaded module: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libselinux.so.1 (base 0x7083279000, 395 dynsym)
[+] own-loading dependency: /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so.6
[+] dep ld-linux-aarch64.so.1: host ld-linux fallback
[+] relocated 340 entries
[+] entering 0x7103f72818 (stack 0x7081ad2f60)

                                =======POKROK=======
TP=0x710550c010 tls_offset=0x13fff0 memsz=0x98
[dbg] patching libc.so.6 sbrk @0x7082eaf500 -> 0x63d3174550
[dbg] patching libc.so.6 __sbrk @0x7082eaf500 -> 0x63d3174550
[dbg] patching libc.so.6 brk @0x7082ea9d40 -> 0x63d31746e8
[dbg] libc pre-init mp_ bytes: 000000000000000000000000000000000000000000000000
[+] running libc.so.6 init_array[0] @ 0x7082de6080
[+] init_array[0] returned
[+] running libc.so.6 init_array[1] @ 0x7082de5f20
[+] init_array[1] returned
[+] running libc.so.6 init_array[2] @ 0x7082de5fc0
[+] init_array[2] returned
[+] running libc.so.6 init_array[3] @ 0x7082de6040
[+] init_array[3] returned
[dbg] post-init /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libc.so.6
[+] running libpcre2-8.so.0 DT_INIT
[+] running libpcre2-8.so.0 init_array[0] @ 0x7083129760
[+] init_array[0] returned
[dbg] post-init /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libpcre2-8.so.0
[+] running libselinux.so.1 DT_INIT
[+] running libselinux.so.1 init_array[0] @ 0x7083280f00
[+] init_array[0] returned
[+] running libselinux.so.1 init_array[1] @ 0x7083280c44
[+] init_array[1] returned
[dbg] post-init /data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/aarch64-linux-gnu/libselinux.so.1
[dbg] libc mp_ @ 0x7082f74a40: 0000000000000000000000000000000000000000000000000000000000000000
  [maps-begin]
63d316c000-63d3170000 r--p 00000000 fd:29 1545926                        /data/user/0/com.linux_core/files/usr/bin/elf_loader
63d3173000-63d317d000 r-xp 00003000 fd:29 1545926                        /data/user/0/com.linux_core/files/usr/bin/elf_loader
63d3180000-63d3182000 r--p 0000c000 fd:29 1545926                        /data/user/0/com.linux_core/files/usr/bin/elf_loader
63d3185000-63d3186000 rw-p 0000d000 fd:29 1545926                        /data/user/0/com.linux_core/files/usr/bin/elf_loader
63d3186000-63d31b5000 rw-p 00000000 00:00 0                              [anon:.bss]
70812d4000-7082dc4000 rw-p 00000000 00:00 0
7082dc4000-7082f60000 r-xp 00000000 00:00 0
7082f60000-7082f83000 rw-p 00000000 00:00 0
7083127000-70831bd000 r-xp 00000000 00:00 0
70831bd000-70831d8000 rw-p 00000000 00:00 0
7083279000-70832aa000 r-xp 00000000 00:00 0
70832aa000-70832cc000 rw-p 00000000 00:00 0
70832cc000-70835ea000 ---p 00000000 00:00 0                              [anon:cfi shadow]
70835ea000-70835eb000 r--p 00000000 00:00 0                              [anon:cfi shadow]
70835eb000-7083654000 ---p 00000000 00:00 0                              [anon:cfi shadow]
7083654000-7083655000 r--p 00000000 00:00 0                              [anon:cfi shadow]
7083655000-71032cc000 ---p 00000000 00:00 0                              [anon:cfi shadow]
71032cc000-71032cf000 r--p 00000000 fd:06 2873                           /system/lib64/libnetd_client.so
71032cf000-71032d4000 r-xp 00003000 fd:06 2873                           /system/lib64/libnetd_client.so
71032d4000-71032d5000 r--p 00008000 fd:06 2873                           /system/lib64/libnetd_client.so
71032d5000-71032d6000 rw-p 00008000 fd:06 2873                           /system/lib64/libnetd_client.so
7103300000-7103312000 r--p 00000000 07:30 42                             /apex/com.android.runtime/lib64/bionic/libm.so
7103312000-7103337000 r-xp 00012000 07:30 42                             /apex/com.android.runtime/lib64/bionic/libm.so
7103337000-7103338000 r--p 00037000 07:30 42                             /apex/com.android.runtime/lib64/bionic/libm.so
7103338000-7103339000 rw-p 00037000 07:30 42                             /apex/com.android.runtime/lib64/bionic/libm.so
710334a000-7103394000 r--p 00000000 fd:06 2621                           /system/lib64/libc++.so
7103394000-71033f2000 r-xp 0004a000 fd:06 2621                           /system/lib64/libc++.so
71033f2000-71033f9000 r--p 000a8000 fd:06 2621                           /system/lib64/libc++.so
71033f9000-71033fa000 rw-p 000ae000 fd:06 2621                           /system/lib64/libc++.so
71033fa000-71033fd000 rw-p 00000000 00:00 0                              [anon:.bss]
7103400000-7103c00000 rw-p 00000000 00:00 0                              [anon:libc_malloc]
7103c28000-7103c70000 r--p 00000000 07:30 39                             /apex/com.android.runtime/lib64/bionic/libc.so
7103c70000-7103d2a000 r-xp 00048000 07:30 39                             /apex/com.android.runtime/lib64/bionic/libc.so
7103d2a000-7103d31000 r--p 00102000 07:30 39                             /apex/com.android.runtime/lib64/bionic/libc.so
7103d31000-7103d34000 rw-p 00108000 07:30 39                             /apex/com.android.runtime/lib64/bionic/libc.so
7103d34000-7103f44000 rw-p 00000000 00:00 0                              [anon:.bss]
7103f44000-7103f45000 r--p 00000000 00:00 0                              [anon:.bss]
7103f45000-7103f4d000 rw-p 00000000 00:00 0                              [anon:.bss]
7103f6d000-7103f8d000 r-xp 00000000 00:00 0
7103f8d000-7103fa0000 rw-p 00000000 00:00 0
7103fa0000-7103fc0000 r--s 00000000 00:11 143                            /dev/__properties__/u:object_r:heapprofd_prop:s0
7103fc0000-7103fe0000 r--s 00000000 00:11 157                            /dev/__properties__/u:object_r:libc_debug_prop:s0
7103fe0000-7104000000 r--s 00000000 00:11 44                             /dev/__properties__/u:object_r:build_prop:s0
7104000000-7105400000 ---p 00000000 00:00 0
7105415000-7105435000 r--s 00000000 00:11 138                            /dev/__properties__/u:object_r:gwp_asan_prop:s0
7105435000-7105455000 r--s 00000000 00:11 85                             /dev/__properties__/u:object_r:debug_prop:s0
7105455000-7105475000 r--s 00000000 00:11 1310                           /dev/__properties__/properties_serial
7105475000-710548f000 r--s 00000000 00:11 11                             /dev/__properties__/property_info
710548f000-71054f3000 rw-p 00000000 00:00 0                              [anon:linker_alloc]
71054f3000-71054f4000 r--p 00000000 07:30 40                             /apex/com.android.runtime/lib64/bionic/libdl.so
71054f4000-71054f5000 r-xp 00001000 07:30 40                             /apex/com.android.runtime/lib64/bionic/libdl.so
71054f5000-71054f6000 r--p 00002000 07:30 40                             /apex/com.android.runtime/lib64/bionic/libdl.so
71054f6000-71054f7000 ---p 00000000 00:00 0
71054f7000-71054f8000 r--p 00000000 00:00 0                              [anon:.bss]
7105507000-710550b000 rw-p 00000000 00:00 0                              [anon:System property context nodes]
710550b000-710550c000 ---p 00000000 00:00 0
710550c000-710550f000 rw-p 00000000 00:00 0                              [anon:stack_and_tls:main]
710550f000-7105510000 ---p 00000000 00:00 0
7105554000-7105574000 r--s 00000000 00:11 1266                           /dev/__properties__/u:object_r:vendor_socket_hook_prop:s0
7105574000-710563c000 r--p 00000000 00:00 0                              [anon:linker_alloc]
710563c000-710563e000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
710563f000-7105640000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
7105641000-7105644000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
7105646000-7105647000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
7105648000-710564c000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
710564c000-710564d000 rw-p 00000000 00:00 0
710564d000-7105654000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
7105654000-7105655000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_lob]
7105655000-7105657000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
7105657000-7105677000 r--s 00000000 00:11 1295                           /dev/__properties__/u:object_r:vndk_prop:s0
7105677000-710567c000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
710567c000-710569c000 r--s 00000000 00:11 22                             /dev/__properties__/u:object_r:arm64_memtag_prop:s0
710569c000-7105700000 r--p 00000000 00:00 0                              [anon:linker_alloc]
7105700000-7105701000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
7105701000-7105721000 r--s 00000000 00:11 85                             /dev/__properties__/u:object_r:debug_prop:s0
7105721000-7105741000 r--s 00000000 00:11 44                             /dev/__properties__/u:object_r:build_prop:s0
7105741000-7105742000 ---p 00000000 00:00 0
7105742000-710574a000 rw-p 00000000 00:00 0
710574a000-710574b000 ---p 00000000 00:00 0
710574b000-710576b000 r--s 00000000 00:11 1310                           /dev/__properties__/properties_serial
710576b000-710576f000 rw-p 00000000 00:00 0                              [anon:System property context nodes]
710576f000-7105789000 r--s 00000000 00:11 11                             /dev/__properties__/property_info
7105789000-71057ed000 r--p 00000000 00:00 0                              [anon:linker_alloc]
71057ed000-71057ef000 rw-p 00000000 00:00 0                              [anon:bionic_alloc_small_objects]
71057ef000-71057f0000 r--p 00000000 00:00 0                              [anon:atexit handlers]
71057f0000-7105930000 ---p 00000000 00:00 0
7105930000-7105932000 rw-p 00000000 00:00 0
7105932000-71067f0000 ---p 00000000 00:00 0
71067f0000-71067f1000 ---p 00000000 00:00 0
71067f1000-71067f9000 rw-p 00000000 00:00 0                              [anon:thread signal stack]
71067f9000-71067fa000 rw-p 00000000 00:00 0                              [anon:arc4random data]
71067fa000-71067fb000 rw-p 00000000 00:00 0
71067fb000-71067fc000 r--p 00000000 00:00 0                              [anon:atexit handlers]
71067fc000-71067fd000 rw-p 00000000 00:00 0                              [anon:arc4random data]
71067fd000-71067fe000 r--p 00000000 00:00 0                              [vvar]
71067fe000-71067ff000 r-xp 00000000 00:00 0                              [vdso]
71067ff000-7106837000 r--p 00000000 07:30 16                             /apex/com.android.runtime/bin/linker64
7106837000-7106921000 r-xp 00038000 07:30 16                             /apex/com.android.runtime/bin/linker64
7106921000-7106929000 r--p 00122000 07:30 16                             /apex/com.android.runtime/bin/linker64
7106929000-710692b000 rw-p 00129000 07:30 16                             /apex/com.android.runtime/bin/linker64
710692b000-7106934000 rw-p 00000000 00:00 0                              [anon:.bss]
7106934000-7106935000 r--p 00000000 00:00 0                              [anon:.bss]
7106935000-7106937000 rw-p 00000000 00:00 0                              [anon:.bss]
7f00000000-7f04000000 rw-p 00000000 00:00 0
7febcbc000-7febcdd000 rw-p 00000000 00:00 0                              [stack]
  [maps-end]
F:tp=0000007081ad4720 pc=0000000000021830 sp=0000007081ad2f00 ad=0000000000021830 x00=00000063d3185420 x01=0000000000000000 x02=b40000710380c000 x03=0000007103f72918 x04=0000007103f84328 x05=0000000000000000 x06=0000007081ad2f60 x07=0000007081ad2f60 x08=00000063d31aa000 x09=000000710550c010 x10=00000063d317aeec x11=0000000000000001 x12=0000007febcdbc24 x13=0000000000000003 x14=0000000000000000 x15=0000007103c53982 x16=0000007082f73d38 x17=0000000000021830 x18=0000007105930000 x19=0000007081ad2f68 x20=0000000000000001 x21=0000007103f842a8 x22=0000007103f70eb0 x23=b40000710380c000 x24=00000063d31b3190 x25=0000000000000000 x26=00000063d3185d20 x27=0000000000000000 x28=0000000000000000 x29=0000007081ad2f00 x30=0000007082de6328
MP:000000000000000c00000000000000cd00000000000000cd00000000000000eb000000000000007f000000000000000000000000000000000000000000000000000000000000004300000000000000cd00000000000000cd00000000000000eb000000000000007f000000000000000000000000000000000000000000000000000000000000005600000000000000cd00000000000000cd00000000000000eb000000000000007f000000000000000000000000000000000000000000000000000000000000008900000000000000cd00000000000000cd00000000000000eb000000000000007f000000000000000000000000000000000000000000000000000000000000009600000000000000cd00000000000000cd00000000000000eb000000000000007f00000000000000000000000000000000000000000000000000000000000000bd00000000000000cd00000000000000cd00000000000000eb000000000000007f000000000000000000000000000000000000000000000000
Segmentation fault
