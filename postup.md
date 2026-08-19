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