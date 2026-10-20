# Prompt 02 — rychlé výhry exec (cíl 48 → ~20 ms)

> **STAV: HOTOVO (2026-10-20).** Dva kandidáti implementováni (cache
> svc-site skenu, `DT_GNU_HASH` lookup), dva zamítnuty (statický loader,
> cache klasifikace ELF). Cíl splněn: exec 48 → **26 ms**. Viz
> [00-README.md — Co je hotové](00-README.md#co-je-hotové-stav-2026-10-20),
> `AGENTS.md` sekce 1 a `postup.md` 2026-10-20.

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
a `docs/nextgen/proot-nahrada.md` kapitolu 5. **Předpoklad:** prompt 01 je
hotový — použij jeho fázový profil a začni tím, co stojí nejvíc.

## Kandidáti (pořadí podle naměřeného profilu)

1. **Statický bionic loader** — NDK build s `-static` (nebo static-pie), aby
   odpadl linker64 a relokace loaderu samotného. Pozor: `dlfcn_stubs.c`
   a bionic bridge používají skutečný bionic `dlopen` — ověř, že to ve statickém
   buildu jde (statický bionic `dlopen` neumí). Když ne, zdokumentuj a přeskoč.
2. **Cache výsledků scanu `svc` a hook lookupů** podle BuildID modulu.
   `elf_patch_syscall_sites` (`src/elf_loader.c:1942`) dnes při každém exec
   lineárně prochází `.text` pro čísla 99, 151, 152, 293, 435 a hook tabulka
   (~125 položek) dělá lookupy přes celý scope. Cache: soubor
   `$LX_LOG/cache/<buildid>.sites` (offsety + čísla, výsledky lookupů), klíč
   BuildID + velikost + mtime + verze loaderu. Zápis atomicky (tmp + rename).
   Neplatná/poškozená cache = tichý fallback na scan.
3. **Cache klasifikace ELF v exec shimu** — `is_glibc_elf`,
   `elf_needs_real_exec`, shebang, řetěz symlinků. Klíč `(dev, ino, mtime,
   size)`. Pozor: shim běží pod guest TP → jen raw syscally, žádný malloc;
   pevná tabulka ve sdílené paměti (memfd/soubor mmapnutý `MAP_SHARED`, fd číslo
   v env), lock-free nebo s jednoduchým generačním čítačem.

## Hotovo, když

- Opakovaný `ELF_LOADER_TIMING=1` profil a benchmark 1000× `/bin/true` ukazují
  zlepšení (čísla do `postup.md`).
- Regresní test: cache se po změně binárky (jiný mtime/BuildID) zneplatní —
  A/B kategorie `test-all.sh execcache`.
- `test-all.sh all` bez regrese.
