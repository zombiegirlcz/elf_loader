# Prompt 13 — úklid `.l2s`, stropy tras a odolnost proti zaplnění disku

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md` (společná
pravidla), `AGENTS.md` sekce „Bezpečnost — destruktivní testy“, „Pravidla
a pasti“ a `docs/nextgen/08-fakeroot-l2s.md`. **Předpoklad:** prompt 08
(fakeroot + link2symlink) a prompt 11 (soft exec) jsou hotové.

## Charakter úkolu

Oprava dvou chyb, které v provozu zaplnily disk, plus údržbové nástroje.
Žádná nová funkcionalita, žádná změna defaultů.

## Nálezy, které to vyvolaly (2026-10-29)

Zařízení mělo volných 6,4 GiB ze 107 GiB. Příčiny byly dvě, nezávislé:

1. **Trasa soft execu bez stropu.** `$D/usr/sx.log` narostl na **6,4 GB**
   rychlostí ~1,4 MB/s. Osiřelý proces (`env /bin/echo T1`, pid 21220, uid
   10323, běžel 1,6 h ve stavu `R`) zacyklený v `sig-iter` smyčce psal
   do souboru nastaveného přes `ELF_LOADER_SOFTEXEC_TRACE`. Samotná smyčka
   je opravená v `c14655b` (kernelová `sigaction` má 32 B), ale **trasa
   žádný limit nemá** — každý další zacyklený proces udělá totéž.
   Místo: `src/softexec.h:68` (`sx_log`), zápis `O_WRONLY|O_CREAT|O_APPEND`
   bez počítadla.

2. **`l2s_unlink` se nevolá při `rename`.** `$ROOTFS/.l2s` mělo **9,39 GiB**,
   z toho **67 187 osiřelých backing souborů = 3,58 GiB (40 %)**. Příčina:
   `l2s_unlink` je zapojený jen do `shim_unlink`/`shim_unlinkat`
   (`src/main.c:1600`, `:1611`), ale **ne** do `shim_rename`/`shim_renameat`/
   `shim_renameat2` (`src/main.c:1567–1596`). `rename(a, b)`, kde `b` je l2s
   symlink, zahodí `b` bez snížení počítadla → backing soubor zůstane.
   Typický producent je git: `tmp_pack_X` → `rename` na `pack-<sha>.pack`.
   Největší zbytky: `.l2s.tmp_pack_*` (512 / 152 / 149 / 128 / 88 / 86 / 66 MB),
   `.l2s.tmp_obj_*`, `.l2s.dep-graph.bin*` (cargo), `.l2s.idm-*`.
   Shoduje se s mezerou zapsanou v `AGENTS.md` („`rename` přes l2s jméno
   nesnižuje počítadlo“) — tady má poprvé cenovku.

Vedlejší nález, který nepochází z loaderu: po operacích pod skutečným rootem
(apt/dpkg 23.–25. 10.) zůstalo **22 087 souborů `root:root`** a **30 291
souborů s labelem `app_data_file:s0` bez kategorií**. Appka (`u0_a323`) na ně
nemá přístup ani na `stat`, takže je `du` pod appkou nevidělo — odtud rozdíl
46,06 GiB (pod appkou) proti 52,71 GiB (pod rootem). To je přesně povinnost
z kořenového `CLAUDE.md`, která se po root operaci neudělala.

## Úkoly

### T1 — strop trasy soft execu

- `SX_LOG_MAX`, default **16 MiB**, override `ELF_LOADER_SOFTEXEC_TRACE_MAX`
  (v bajtech; `0` = bez limitu, pro ladění).
- `static size_t g_sx_logged;` v `sx_log` inkrementovat o zapsané bajty;
  po překročení zapsat **jednou** `[softexec] trace capped` a dál už do
  souboru nepsat. Stderr nechat beze změny — ten je omezený konzumentem.
- `sx_log` běží **pod guest TP** → jen `shim_raw_syscall6` a `shim_str*`,
  žádný `snprintf`/`getenv` (past č. 1 v `AGENTS.md`). Limit vyčti
  v `softexec_enabled()`, kde už `getenv` voláš, a ulož do statiku.
- Test: nová kategorie `softexec_cap` v `test-all.sh` — spustit se
  `ELF_LOADER_SOFTEXEC_TRACE_MAX=4096`, vygenerovat víc trasy a ověřit,
  že soubor nepřeroste limit + obsahuje `trace capped`.

### T2 — `l2s_unlink` při přepisujícím `rename`

- V `shim_rename`, `shim_renameat` a `shim_renameat2` zavolat
  `l2s_unlink(<cílová cesta>)` **před** samotným renamem, stejnou podmínkou
  jako na `src/main.c:1611` (jen pro `AT_FDCWD`/`-100`, jinak cestu
  nerozlišíš).
- **Nedělat** pro `renameat2` s `RENAME_EXCHANGE` (obě strany zůstávají
  živé) ani s `RENAME_NOREPLACE` (cíl se nepřepisuje). Jen pro přepis.
- Test: kategorie `l2s_rename`, **destruktivně jen na kali** (`AGENTS.md`):
  vytvořit hardlink → l2s záznam, přepsat cíl `rename`em, ověřit že
  v `.l2s` nezůstal backing soubor a že počítadlo zdroje je správné.
  Stará binárka FAIL, nová PASS.

### T3 — `tools/l2s-gc.sh`

Údržba `.l2s`, protože i po T2 zbytky vzniknou (crash, kill, starý proot).

- **Hostový** skript (`ashell`, `/system/bin/*`), nikdy pod loaderem:
  `rm -rf` pod loaderem maže zdroj bindu (`AGENTS.md`).
- Dvouúrovňový algoritmus (viz níže). Jednoúrovňový dává **falešné
  osiřelé** — ověřeno, viz „Pasti“.
- Běh **pod rootem**, ne pod uid appky: pod appkou je 4 153 symlinků
  nečitelných a jejich cíle pak vypadnou z množiny živých odkazů.
- `--dry-run` je **default**, mazání jen na `--delete`.
- `--keep <glob>` denylist, povinně předvyplněný `*status*`, `*dpkg*`,
  `*.l2s.diversions*` — viz „Co se stalo s dpkg“.
- `--quarantine <dir>` místo `rm` (přesun; místo neuvolní, ale je to
  vratné).
- Guard porovnávat **v MiB, ne v bajtech** — viz „Pasti“.

### T4 — `tools/fix-app-ownership.sh`

- Najde `-user 0` v adresáři appky, udělá `chown` (a zvlášť `chown -h`
  na symlinky) na vlastníka **odvozeného z cílové složky**, ne natvrdo.
- `chcon -h` labelem odvozeným ze samotné složky (`ls -ldZ`), ne hardcode:
  tahle appka má `u:object_r:app_data_file:s0:c67,c257,c512,c768`, zapsaná
  paměť projektu uváděla `c54…` a jiný uid (`u0_a310`) — kategorie plynou
  z uid, takže hardcode je chyba.
- Report před/po, `--dry-run` default.

### T5 — `tools/dpkg-status-rebuild.sh`

`$ROOTFS/var/lib/dpkg/status` v parrotu **neexistuje** (dpkg spadl 27. 10.
17:38 mezi zápisem `status-new` a přejmenováním; `status-new` má 0 B,
`status-old` chybí, `var/backups` je bez záloh). Poslední kopie obsahu
(2 250 273 B) žila jako osiřelý `.l2s.status0001.0001` a byla **smazána
úklidem 29. 10.** — reference-based kritérium neumí rozlišit uniklý temp
od poslední kopie databáze po havárii.

- Složit minimální `status` z `var/lib/dpkg/info/*.list` (2 431 balíčků):
  `Package:` / `Status: install ok installed` / `Architecture: arm64`.
- Verze dotáhnout z `var/lib/apt/lists/*Packages*` **pokud existují**
  (teď jsou prázdné → potřeba `apt update`).
- Výsledek psát do `status.rebuilt` a nasadit až po ruční kontrole.

### T6 — zápis pastí do `AGENTS.md`

Čtyři měřicí pasti z 29. 10., všechny potvrzené A/B (viz níže).

## Algoritmus GC pro `.l2s` (dvouúrovňový)

Řetězec je **dvoustupňový**:

```
cesta v rootfs (symlink) → .l2s/.l2s.<jméno><NNNN> (symlink) → .l2s/.l2s.<jméno><NNNN>.<nlink> (data)
```

1. `data` = `find $L -maxdepth 1 -type f -printf '%f\t%s\n'`
2. `mid` = `find $L -maxdepth 1 -type l -printf '%f\t%l\n'`, cíl na basename
3. `rootrefs` = cíle symlinků **mimo** `.l2s`, filtr `/\.l2s/`, basename, `sort -u`
4. `mid_live` = `comm -12 mid_names rootrefs`; `mid_dead` = `comm -23 …`
5. `live` = cíle `mid_live` ∪ `rootrefs` (rootfs smí mířit i přímo na data)
6. `orphans` = `comm -23 data_names live`

Smazat lze `orphans` + `mid_dead`. Ověřená čísla 29. 10. pod rootem:
172 023 data souborů / 104 823 mezistupňů (102 204 živých, 2 619 mrtvých) /
104 836 odkazů → **67 187 osiřelých, 3,58 GiB**. Po smazání: `.l2s`
9,39 → 5,74 GiB a **0 rozbitých odkazů** z 215 001 (ověřeno).

Rozdíl proti jednoúrovňovému: ten bere mezistupeň za důkaz života, takže
backing soubor s živým mezistupněm ale mrtvou cestou v rootfs vyjde jako
živý — unikne mu 2 619 mezistupňů a s nimi i 512 MB pack.

## Pasti (do `AGENTS.md`, všechny ověřené 2026-10-29)

11. **Prootí `readlink` vrací cíle zkrácené na 14 B.** Uvnitř prootu dá
    `find -printf '%l'` na l2s symlinku `/.l2s/.l2s.div` (14 B, konstantně
    pro všechny), host toybox dá správných 82 B
    (`/data/…/parrot/.l2s/.l2s.diversions0001.0001`). 2 874 cílů vyjde
    prázdných. **Jakákoli analýza symlinků v rootfs se musí dělat hostovými
    nástroji přes `ashell`**, jinak vychází nesmysl — konkrétně 169 391
    falešných „osiřelých“ místo 67 187 skutečných. Rozšiřuje past č. 7.
12. **Host `sh`/`test` je 32bitový.** `[ 3839243310 -lt 6442450944 ]` → `rc=1`,
    protože oba operandy přesahují 2³¹−1 a srazí se na `INT_MAX`. Guardy
    a kontroly velikostí počítej v MiB, nebo porovnávej `awk`em.
13. **`find -printf '%Z'` následuje symlinky**, takže hlásí label cíle.
    Z 2 241 „špatně olabelovaných“ bylo 2 099 `?` (visící odkazy),
    97 `toolbox_exec` (odkazy do `/system/bin`), 26 `null_device` — artefakty.
    Skutečně špatných bylo 11. Na label odkazu samotného čti `ls -ldZ`.
14. **Relativní cíl symlinku nelze testovat `[ -e "$target" ]`** — vyhodnotí
    se proti cwd procesu, ne proti adresáři odkazu. Testuj `[ -e "$link" ]`.
    Tato chyba vyrobila hlášení 1 919 rozbitých odkazů, kterých bylo 0.
15. **Toybox `find` nezná `-printf '%h'`** (`%f`, `%l`, `%s`, `%p`, `%Z`
    fungují). S `2>/dev/null` na tom `find` to spolkne chybu a vrátí prázdno.

## Výstup

- T1, T2 jako samostatné commity `fix(prompt13):` s A/B kategoriemi
  v `test-all.sh` (`softexec_cap`, `l2s_rename`), zaregistrované v `all`.
- T3–T5 jako `tools(prompt13):`.
- `test-all.sh all` bez regrese proti 213 PASS / 1 FAIL / 33 SKIP
  (ten FAIL je známý nesouvisející `nslookup localhost` → NXDOMAIN).
- Záznam do `postup.md` s čísly výše, pasti 11–15 do `AGENTS.md`.
