# Prompt 12 — prototyp: nativní glibc proces (`ld-linux-aarch64.so.1` jako program)

> **Hotovo 2026-10-29.** Výsledek, čísla a doporučení (hybrid) jsou
> v `docs/nextgen/proot-nahrada.md`, kapitola „Nativní ld.so — výsledek";
> deník a taxonomie selhání v `postup.md` 2026-10-29 (3). Prototyp žije
> v `tools/lxnative*.c|sh` + `tools/liblx-audit.c`, přepínač
> `tools/lxnative-run.sh`, v harnessu `LX_NATIVE_MODE=1`. Text níže je
> původní zadání.

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(sekce 2, 5, 8 — chyby, které existují jen kvůli dvěma libc v procesu; sekce 9
bionic bridge), postup.md:310–340 (přímý běh ld.so, proč `patchelf_interp`
rozbil proot) a `docs/nextgen/proot-nahrada.md` kapitolu 6. **Předpoklad:**
prompty 04 a 05 (překladová logika L1/L2 existuje a dá se znovu použít).

## Charakter úkolu

Prototyp a strategické rozhodnutí, ne náhrada současného módu.

Celá dosavadní sada 01–11 řeší důsledky jednoho rozhodnutí: **v procesu jsou
dvě libc** (bionic host + guest glibc). Z toho plynou pasti 1, 2, 9 a 10
v `AGENTS.md`, `dl_enter_host`, bionic bridge, falešný `_rtld_global`
i to, že layout `_rtld_global(_ro)` je ověřený jen pro glibc 2.41 a 2.43
a každá další verze ho musí znovu změřit. Prompt 12 zkouší opačný konec:
nechat proces být **čistě glibc** a překlad cest dělat zevnitř přes
rtld-audit. Výstupem není hotový mód, ale **podložené doporučení**, kterým
směrem jít dál.

## Vstupní kontrola (udělej první, než začneš psát kód)

1. `cat /proc/self/attr/current` v app doméně — potřebuješ vědět, jestli je
   `execmem`/W^X vůbec dostupný, protože nativní ld.so musí mapovat guest
   kód sám.
2. `targetSdk` appky a obsah `nativeLibraryDir` — ld.so spustitelný z app
   datového adresáře je to, co u targetSdk ≥ 29 bývá zakázané; ověř na
   zařízení, netipuj.
3. Verze glibc v obou rootfs (`parrot` 2.41, `kali` 2.43) — prototyp musí
   fungovat na obou, jinak nemá cenu ho srovnávat s own-loadem, který to umí.
4. Ověř, že `ld.so --argv0 … --library-path … <prog>` v rootfs vůbec
   nastartuje **bez** našeho kódu (čistý `ashell -c`). Když ne, dál nemá
   smysl pokračovat a prompt končí záznamem proč.

## Úkoly

1. `liblx.so` — glibc (ne bionic!) sdílená knihovna, build v rootfs gcc:
   - rtld-audit rozhraní (`la_version`, `la_objopen`): pro každý načtený objekt
     (i `dlopen`) provést L1 přepis `svc` (sdílet kód s promptem 04);
   - L2: seccomp okno — v čistě glibc procesu je okno = rozsahy guest modulů;
   - opravy výsledků (`getcwd`, `readlinkat`, `/proc/self/exe` → program, ne ld.so);
   - exec shim: guest `execve` glibc programu → `ld.so --argv0 … --library-path …
     --audit liblx.so prog`; bionic/statické → raw.
2. Spouštěč: `$ROOTFS/lib/ld-linux-aarch64.so.1 --argv0 <n> --library-path
   <rootfs libdiry> --audit <liblx.so> <prog> args`. Žádná úprava binárek
   v rootfs.
3. Projdi `test-all.sh all` v tomto módu (přepínač v harnessu) a sepiš rozdíly.
4. Změř exec a stat-heavy zátěž (prompt 01) proti `--ownall`.
5. Vyhodnoť strategické body z kapitoly 6:
   - vazba ld.so ↔ libc.so.6 (GLIBC_PRIVATE) a co to znamená pro W^X
     (targetSdk ≥ 29, ld.so v `nativeLibraryDir`) — ověř současnou doménu
     `cat /proc/self/attr/current`;
   - bionic bridge (GPU) v tomto módu nejde → hybrid: jak by exec shim volil mód;
   - RAM: sdílení stránek (file-backed) proti own-loadu.

## Metodika měření (bez ní je prompt neplatný)

- A/B **knobem na téže binárce**, nikdy dvěma buildy — pinování CPU mění
  čísla o desítky % (`AGENTS.md`, Metodika).
- Obě pořadí, nejmíň 2 kola, stejná CPU politika.
- Referenční čísla, proti kterým se to srovnává (host-level, `LX_DW=0`):
  nativ 11 / proot 28 / loader ~26 ms na `/bin/true`; `python3 -c 0`
  102,8 ms; 1000× `/bin/true` 25,8 s. Soft exec (prompt 11) dává
  11 ms/exec a `fork` 4,2 ms proti 1,2 ms u prootu.
- Měř i **RSS**, ne jen čas — hlavní argument pro nativní ld.so je sdílení
  file-backed stránek, ne rychlost.
- Jediný platný kanál je `ashell -c`; `su -c` jde přes proot a maskuje
  path bugy (past č. 5).

## Kritéria úspěchu

Prototyp je úspěšný, když **všechno** níže platí:

- `test-all.sh all` v nativním módu nemá *nové* FAIL proti defaultu
  (213 PASS / 1 FAIL / 33 SKIP; ten FAIL je známý `nslookup localhost`);
  kategorie, které v nativním módu nejdou principiálně, smí být SKIP
  s vysvětlením.
- Exec není pomalejší než `--ownall` bez soft execu (~26 ms).
- RSS na stejné zátěži je nižší než u own-loadu, měřitelně.
- Funguje na parrotu (2.41) i kali (2.43) bez per-verzních konstant.

Když některé selže, je to taky platný výstup — zapiš **které a proč**,
to je pro rozhodnutí stejně cenné jako úspěch.

## Kdy přestat

- Vstupní kontrola bod 1 nebo 2 vyjde negativně (W^X / spustitelnost ld.so
  z app adresáře) → prototyp nemá cestu na zařízení, skonči záznamem.
- `--audit` se v guest glibc nevolá na objekty načtené před auditem
  (ld.so sám, libc) a nejde to obejít → popiš, co by muselo přijít z L2.
- Víc než dva dny bez průchodu `test-all.sh all` → vrať se a napiš, co blokuje.

## Co nedělat

- **Neupravovat binárky v rootfs** (`patchelf`, `PT_INTERP`) — rozbíjí to
  proot, se kterým musí vše koexistovat; tohle už se jednou stalo
  (postup.md:310–340).
- Neměnit default. Prototyp žije za přepínačem (`lx --native` nebo env).
- Nic destruktivního na živý parrot — `--ownall`/`IMGCACHE`/in-place
  operace jen na **kali** (`AGENTS.md`, Bezpečnost).
- Neotvírat znovu cache RELRO obrazů ani lazy binding — relokace je po
  hashi jen 2–12 % exec a bez nového měření se k tomu nevrací.

## Výstup

- Prototyp za přepínačem (`lx --native` nebo env), bez dopadu na default.
- Kapitola „Nativní ld.so — výsledek" v `docs/nextgen/proot-nahrada.md`:
  tabulka testů, čísla, doporučení (own-load / nativní / hybrid) s odůvodněním.
- Řádek do tabulky v `00-README.md` a záznam do `postup.md` s A/B čísly.
- Pokud doporučení je „hybrid“, pak i konkrétní rozhodovací pravidlo pro
  exec shim: podle čeho vybere mód, a co se stane u binárky, kde se spletl.
