# Analýza 6. crashu (starship/rayon pthread_create) — _dl_allocate_tls

## TL;DR verdikt

Override `_dl_allocate_tls` JE zaregistrovaný (`main.c:1299`) a `override_lookup`
JE první v `resolve_jmp_symbol` i `elf_resolve_import`. Crash ale pokračuje
uvnitř guest ld.so. To znamená, že **volání NEJDE přes elf_loaderovu resolve
cestu** — guest ld.so volá sám sebe interně (přímé `bl`, vlastní GOT).

Navíc i kdyby override chytil, `src/ldso_tls.c` má **vadu**: nikdy nezapíše
`tcbhead.dtv` (offset +0x00 od TCB), takže `__tls_get_addr` by čte NULL.

## Disasm důkazy (guest ld.so, parrot glibc 2.41)

### `_dl_allocate_tls@0xfeb0`
```
feb0: paciasp
feb4: stp  x29,x30,[sp,#-16]!
febc: cbz  x0, fed4            ; mem == NULL -> fed4 (storage path)
fec0: bl   f360                ; mem != NULL -> _dl_allocate_tls_storage
fec4: ldp  x29,x30,[sp],#16
fecc: mov  w1, #0
fed0: b    fc50                ; -> _dl_allocate_tls_init
fed4: bl   fb60                ; !!! mem==NULL -> 0xfb60
fed8: ...
fee0: mov  w1, #0
fee4: b    fc50
```

### `_dl_allocate_tls_storage` = **0xfb60** (ne 0xf360!)
```
fb60: paciasp
fb64: stp  x29,x30,[sp,#-48]!
fb68: mov  w0, #1
fb6c: mov  x29, sp
fb70: stp  x19,x20,[sp,#16]
fb74: adrp x19, 3f000           ; x19 = &_rtld_global_ro (base+0x3fb68)
fb78: add  x19, x19, #0xb68
fb7c: stp  x21,x22,[sp,#32]
fb80: adrp x21, 40000           ; x21 = &_rtld_global (base+0x40000)
fb84: add  x22, x21, #0xf48     ; x22 = _rtld_global+0xf48
fb88: ldr  x20, [x19, #472]     ; x20 = GLRO(dl_tls_static_size)  @ro+0x1d8
fb8c: mov  x1, x22
fb90: bl   1d750                ; _dl_tls_allocate_begin? (lock)
fb94: adrp x1, 3f000
fb98: ldr  x0, [x19, #480]      ; x0 = GLRO(dl_tls_static_align) @ro+0x1e0
fb9c: ldr  x1, [x1, #2832]      ; x1 = *(base+0x3fb78)  !!! FIREMNÍ MALLOC
fba0: add  x0, x20, x0          ; size = static_size + align
fba4: add  x0, x0, #0x728       ; + 0x728 (TLS_PRE_TCB_SIZE?)
fba8: blr  x1                   ; malloc(...)  <-- druhá cache!
fbac: cbz  x0, fc28
fbb0: mov  x20, x0
fbb4: mov  x2, #0x730
fbb8: ldr  x0, [x19, #480]      ; align
fbbc: mov  w1, #0
fbc0: add  x19, x0, #0x727
fbc4: add  x19, x20, x19
fbc8: udiv x19, x19, x0         ; align up
fbcc: mul  x19, x19, x0
fbd0: sub  x0, x19, #0x720      ; result TCB
fbd4: bl   1d400                ; _dl_tls_allocate_end
fbd8: sub  x1, x19, #0x8, lsl #12
fbdc: mov  x0, x19
fbe0: str  x20, [x1, #30936]    ; uložit raw pointer
fbe4: bl   f360                 ; <-- volání allocate_dtv!
fbe8: mov  x19, x0
fbec: cbz  x0, fc14
...
```

### `allocate_dtv` = **0xf360** (kde padáme)
```
f360: paciasp
f364: stp  x29,x30,[sp,#-32]!
f368: adrp x1, 40000            ; &_rtld_global
f36c: add  x1, x1, #0xb58       ; +0xb58 = _dl_tls_dtv_slotinfo_list
f370: mov  x29, sp
f374: stp  x19,x20,[sp,#16]
f378: mov  x19, x0              ; x19 = result (TCB z allocate_tls_storage)
f37c: ldr  x0, [x1]             ; x0 = slotinfo_list <-- NULL
f380: adrp x2, 3f000
f384: add  x20, x0, #0xe
f388: mov  x1, #0x10
f38c: ldr  x2, [x2, #2816]      ; x2 = *(base+0x3fb00) <-- staticky 0
f390: add  x0, x0, #0x10
f394: blr  x2                   ; CRASH (x2 == 0)
f398: cbz  x0, f3ac
f39c: mov  x1, x0
f3a0: mov  x0, x19
f3a4: str  x20, [x1], #16
f3a8: str  x1, [x19]            ; tcbhead.dtv = ...
f3ac: ldp  x19,x20,[sp,#16]
f3b0: ldp  x29,x30,[sp],#32
f3b4: autiasp
f3b8: ret
```

**To je ono!** `allocate_dtv`:
- čte `_rtld_global+0xb58` = `_dl_tls_dtv_slotinfo_list` (NULL)
- čte `*(base+0x3fb00)` = **`__rtld_calloc` cache** (0)
- `blr x2` = `__rtld_calloc(dtv_length+2, 16)` → CRASH

**`base+0x3fb00` = `_rtld_global_ro - 0x68` = `__rtld_calloc` / `__rtld_malloc`
cache, kterou plní `_dl_start`.** V glibc 2.41 se to jmenuje `_rtld_malloc_fn`
nebo `__rtld_malloc` — pointer na aktivní malloc implementaci.

## Druhá cache: `base+0x3fb78` = `_rtld_global_ro + 0x10`
V `_dl_allocate_tls_storage@fb9c` se čte `*(base+0x3fb78)` a volá jako malloc.
`_rtld_global_ro + 0x10` v glibc = `_dl_rtld_malloc` (nebo podobná cache).
Tedy **také nula** → i kdybychom opravili `0x3fb00`, spadneme na `0x3fb78`.

## Třetí cache: `base+0x3fb90` = `_rtld_global_ro + 0x28`
V `_dl_allocate_tls_init@fc90` se čte `*(base+0x3fb90)` (`ldr x1, [x1, #2848]`)
a volá jako funkce (lock `_dl_load_tls_lock`).

## Závěr: guest ld.so má **tři** neinicializované malloc/lock cache

| Adresa | Offset od _rtld_global_ro | Význam | Kde se čte |
|---|---|---|---|
| `base+0x3fb00` | -0x68 | `__rtld_calloc` cache | `allocate_dtv@f38c` |
| `base+0x3fb78` | +0x10 | `__rtld_malloc` cache | `_dl_allocate_tls_storage@fb9c` |
| `base+0x3fb90` | +0x28 | `_dl_load_tls_lock` | `_dl_allocate_tls_init@fc90` |

**Všechny tři jsou staticky nula a nejsou v žádné relokaci** (ld.so má jen
1 GLOB_DAT `__stack_chk_guard@0x3ffd8` + RELR pokrývající `.data.rel.ro`
0x3e8a0–0x3e960, 0x3fd60, 0x3fdc0–0x3fe00, 0x3ffe0–0x3fff8).

## Co to znamená pro override strategii

**Override `_dl_allocate_tls` NEMŮŽE zabránit tomuto crashi**, protože:
1. Crash je v `allocate_dtv@f360`, volané z `_dl_allocate_tls_storage@fb60`,
   volané z `_dl_allocate_tls@feb0` **všemi přímými `bl` uvnitř ld.so**.
2. Aby se sem kód dostal, musel někdo zavolat guest `_dl_allocate_tls@feb0`
   přímo (ne přes elf_loaderovu resolve).
3. Kdo? Pravděpodobně **guest libc.so.6 přes vlastní PLT** — ale ta je
   v `--ownall` flow taky relokovaná elf_loaderem... **ledaže lazy binding
   u libc.so.6 je vypnutý** a `_dl_allocate_tls` se resolvuje hned při
   `elf_relocate` → `resolve_jmp_symbol` → override_lookup → mělo by chytit.

**Zbývá jediné vysvětlení:** volání jde z **guest ld.so vlastního kódu**
(např. `__tls_get_addr@0x1020c` lazy DTV init, nebo `_dl_allocate_tls_init`
volaná z `pthread_create` uvnitř guest libc, ale přes **přímý GOT guest
ld.so** — který elf_loader **nerelokoval**, protože ld.so má jen 1 relokaci).

## NUTNÝ EXPERIMENT na device

Spustit starship s diagnostikou a zjistit, zda se override vůbec zavolá:

```
ELF_LOADER_DEBUG=1 elf_loader --ownall starship 2>&1 | grep -E 'DIAG-TLS|override'
```

- Pokud `[DIAG-TLS] ldso_allocate_tls called` **není** → override nechytil
  (volá se guest ld.so interně) → **nutné B1: inicializovat cache v guest
  ld.so datech**.
- Pokud **je** → override chytil, ale `ldso_allocate_tls` je vadná (DTV).

## Dvě možné opravy

### Opce A: B1 — inicializovat tři cache v guest ld.so
Najít `ldso_base` (guest ld.so ve scope), zapsat:
- `ldso_base + 0x3fb00` = pointer na `__rtld_calloc`-like funkci
- `ldso_base + 0x3fb78` = pointer na `__rtld_malloc`-like funkci
- `ldso_base + 0x3fb90` = pointer na lock funkci (nebo no-op)
- `ldso_base + 0x40000 + 0xb58` = validní `dtv_slotinfo_list`

**Problém:** musíme replikovat malloc/calloc z ld.so. Můžeme použít náš
`ldso_private_heap` (už existuje v elf_loaderu!) a vystavit ho jako
calloc/malloc. Slotinfo list musíme postavit ručně.

### Opce A': override přes **GOT guest ld.so**
Guest ld.so má jen 1 relokaci, ale **jeho GOT slots** pro volání přes PLT
by mohly jít přepsat. Museli bychom najít, odkud se volá `_dl_allocate_tls@feb0`
— a to je v guest libc PLT.

### Opce B: opravit `ldso_tls.c` a vynutit override
Pokud override chytí (experiment to potvrdí), stačí opravit `ldso_tls.c`:
- `tcbhead.dtv` musí ukazovat na generation slot.
- DTV entries musí být naplněné pro moduly ve scope.

## Další krok
Spustit experiment na device s `ELF_LOADER_DEBUG=1` a zjistit, zda override
chytí. Pak rozhodnout A vs B.