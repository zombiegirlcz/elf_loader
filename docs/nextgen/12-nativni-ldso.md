# Prompt 12 — prototyp: nativní glibc proces (`ld-linux-aarch64.so.1` jako program)

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(sekce 2, 5, 8 — chyby, které existují jen kvůli dvěma libc v procesu; sekce 9
bionic bridge), postup.md:310–340 (přímý běh ld.so, proč `patchelf_interp`
rozbil proot) a `docs/nextgen/proot-nahrada.md` kapitolu 6. **Předpoklad:**
prompty 04 a 05 (překladová logika L1/L2 existuje a dá se znovu použít).

## Charakter úkolu

Prototyp a strategické rozhodnutí, ne náhrada současného módu.

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

## Výstup

- Prototyp za přepínačem (`lx --native` nebo env), bez dopadu na default.
- Kapitola „Nativní ld.so — výsledek" v `docs/nextgen/proot-nahrada.md`:
  tabulka testů, čísla, doporučení (own-load / nativní / hybrid) s odůvodněním.
