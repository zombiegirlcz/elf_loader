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