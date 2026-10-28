# nextgen — sada promptů: loader místo proot

Každý soubor `NN-*.md` je samostatný prompt pro agenta (Claude Code apod.):
zkopíruj celý obsah jako zadání. Teorie, ze které prompty vycházejí, je
v [proot-nahrada.md](proot-nahrada.md) — prompty odkazují na její kapitoly.

## Pořadí a závislosti

| # | prompt | kapitola studie | závisí na | typ | stav |
|---|---|---|---|---|---|
| 01 | [měření a fázový profil exec](01-mereni-benchmark.md) | 9, 5.1 | – | měření | **hotovo** 2026-10-16 |
| 02 | [rychlé výhry exec](02-exec-rychle-vyhry.md) | 5.2–5.4 | 01 | implementace | **hotovo** 2026-10-20 |
| 03 | [relativní symlinky, clamp `..`, rootfd](03-symlinky-a-rootfd.md) | 4.1–4.3 | – | implementace | **hotovo** 2026-10-21 |
| 04 | [L1 — přepis `svc #0` při načtení](04-L1-prepis-svc.md) | 2.1 | 01 | implementace | **hotovo** 2026-10-21 (default vypnuto) |
| 05 | [L2 — seccomp TRAP v guest okně](05-L2-sigsys-okno.md) | 2.2 | 04 | implementace | **hotovo** 2026-10-21 (3) (default vypnuto) |
| 06 | [tabulka mountů + `mount --bind`](06-mount-tabulka.md) | 3.1 | 04 (doporučeno) | implementace | **hotovo** 2026-10-22 |
| 07 | [virtualizace `/proc` + `argv[0]`](07-proc-a-argv0.md) | 3.4, 3.5 | – | implementace | **hotovo** 2026-10-22 |
| 08 | [fake root + link2symlink](08-fakeroot-l2s.md) | 3.2, 3.3 | 04 (doporučeno) | implementace | **hotovo, ověřeno funkční** 2026-10-28 (default vypnuto) |
| 09 | [proot-kompatibilní CLI + hybrid](09-proot-cli-hybrid.md) | 3.6, 7 | 06, 08 | implementace | **hotovo** 2026-10-28 (posix_spawn cesta bez hybridu) |
| 10 | [cache obrazů + líné vázání](10-exec-image-cache.md) | 5.5, 5.6 | 01, 02 | implementace | **hotovo** 2026-10-20 |
| 11 | [soft exec / zygote — výzkum](11-soft-exec-zygote.md) | 5.7, 5.8 | 01, 10 | výzkum + prototyp | – |
| 12 | [nativní ld.so mód — prototyp](12-nativni-ldso.md) | 6 | 04, 05 | výzkum + prototyp | – |

07 jde dělat kdykoli, je nezávislý.

## Co je hotové (stav 2026-10-22)

**01 — měření a fázový profil.** `ELF_LOADER_TIMING=1` dává fázový profil na
stderr (raw clock, v defaultu no-op), `tools/bench-nextgen.sh [proot|loader|
linuxsh]` zátěžový benchmark (medián), `test-all.sh timing` A/B regresi.
Host-level čísla: nativ 11 / proot 28 / loader 56 ms na `/bin/true`, ale na
cestově těžkém `find /usr` je loader **~25× rychlejší** (0,44 s vs 11 s) —
loader se vyplatí na dlouhé procesy, ne na krátký exec. Kapitola „Naměřeno"
v [proot-nahrada.md](proot-nahrada.md).
**Metodická past:** první profil byl měřen zanořeně pod DW loaderem a jako
baseline neplatil; pinování CPU mění absolutní čísla o desítky procent →
měřit při stejné CPU politice a A/B dělat **knobem na téže binárce**, ne
dvěma buildy.

**02 — rychlé výhry exec.** Ze čtyř kandidátů prošly dva a dva padly:

- *Cache svc-site skenu* (hotovo): `cache_nr_t.scanned` (formát v2) cachuje
  i **negativní** výsledek skenu, `svc_patched` ruší dvojí patchování,
  `cache_dirty` zapisuje jen po skutečném skenu, klíč nese identitu buildu
  loaderu. `svc_patch` 966 µs → **4 µs** (`python3` 30,2 ms → 5 µs).
- *`DT_GNU_HASH` lookup* (hotovo, největší zisk celé sady): `elf_scope_find`
  byl lineární `strcmp` sken přes dynsym všech modulů (~19 µs na lookup,
  stovky lookupů na exec). Po bloom/bucket/chain lookupu `hooks_bti`
  12,3 → **1,8 ms**, interní total `python3` 45,9 → **17,4 ms**, wall-clock
  1000× `/bin/true` 51,6 → **25,8 ms** (−50 %), `python3 -c 0` 150,6 →
  **102,8 ms** (−32 %).
- *Statický bionic loader* (zamítnuto): static-pie na tomhle zařízení
  nefunguje a statický bionic `dlopen` neumí, což bionic bridge potřebuje.
- *Cache klasifikace ELF v exec shimu* (zamítnuto měřením): 38–60 µs na exec
  proti ~26–48 ms celkem = 0,1 %, hluboko pod šumem. Vrátit se, až bude exec
  pod ~1 ms.

Knoby: `ELF_LOADER_NO_CACHE`, `ELF_LOADER_CACHE_TRACE`,
`ELF_LOADER_DOUBLE_PATCH`, `ELF_LOADER_NO_HASH`, `ELF_LOADER_HASH_CHECK`.
A/B `test-all.sh execcache`, `test-all.sh symhash`, host unit test
`test/cache_test.c` (20/20).

**10 — cache obrazů + líné vázání.** Obě části implementované, **obě default
vypnuté** — ne kvůli pádu, ale protože po promptu 02 už nemají co ušetřit:
relokace je jen ~1 ms z 15–30 ms exec.

- *Část A — cache RELRO obrazů* (`src/imgcache.c`): ukládá `PT_GNU_RELRO`
  úseky, moduly mapuje na tytéž báze (`MAP_FIXED_NOREPLACE`), klíč =
  BuildID+size+mtime exe + build tag loaderu + `PAGE_SIZE` + `AT_HWCAP(2)`.
  Pointery do loaderu se ukládají jako **offset od báze jeho obrazu**
  (pevnou bázi PIE vynutit nelze), při restore se přičte aktuální báze.
  `ELF_LOADER_IMGCACHE` má úrovně: 1 = záznam + verifikace (bezpečné),
  2 = skutečné přeskočení RELRO relokace (**experimentální, padá**).
  `ELF_LOADER_IMG_VERIFY=1` → **0 rozdílů** z 749 (ld.so) i 1520 slov (libc).
  Úroveň 1 je ale pomalejší (kopíruje a zapisuje ~20 KB bez přeskakování).
- *Část B — líné vázání*: doplněn eager IFUNC (`if (lazy_binding)` bylo před
  `is_local_ifunc_reloc` → lokální IFUNC `JUMP_SLOT` zůstával na stubu) a
  `obj_wants_bind_now()` pro `DT_BIND_NOW`/`DF_BIND_NOW`/`DF_1_NOW`/
  `LD_BIND_NOW`. **Pád `python3` vyřešen** (`c69eeac`): `elf_lazy_resolve()`
  se volá z guest kódu (PLT stub v `entry.S`), takže běžel pod **guest TP**,
  zatímco uvnitř dělá loaderův C kód závislý na bionickém TLS → wrapper
  s `dl_enter_host()`/`dl_leave_host()`. Měření ale zisk nepotvrzuje
  (`python3 -c pass` 205,3 vs 174,9 ms — v šumu): Parrot `libc`/`ld.so` jsou
  `-z now`, eager si vynutí samy.

Zapínání `ELF_LOADER_IMGCACHE=1`, `ELF_LOADER_LAZY=1` (dědí se do zanořených
exec). A/B `test-all.sh imgcache`, `test-all.sh lazybind`. Plná sada:
default **192/0/33**, jen část A **192/0/33**, obě části **192/0/33**.

**Čtyři pasti z části A**, které stojí za zapamatování: celé `PF_W` segmenty
ukládat nelze (`.data`/`.bss` drží `__curbrk`/`__libc_stack_end`/stack guard);
přeskočit celé `elf_relocate` nelze (neproběhne `apply_segment_prots` →
`.text` bez `PROT_EXEC`); „je to pointer?" nejde heuristikou (`0x430f0a10`
v RELRO ld.so a `0x706f706364` = ASCII jsou data); úroveň 1 nesmí do paměti
zapsat vůbec nic.

**03 — absolutní symlinky, clamp `..`, rootfd.** Úkoly 1–4 hotové
(commit `2af1bf5`), úkol 5 (rootfd) **zamítnut měřením**.

Místo řešení absolutních symlinků shimem za běhu držíme invariant „v rootfs
jsou takové linky relativní v **kanonickém tvaru**" (přesně hloubka × `../`
+ cesta od guest kořene) — jádro je pak uvnitř rootfs vyhodnotí samo a
správně, za nulovou cenu za běhu. `src/relsym.h` (vkládá se do `main.c`,
běží pod guest TP): zápis `shim_abs_target_to_rel` v `symlink`/`symlinkat`
(host mountpointy a proot `.l2s` cíle zůstávají absolutní), čtení
`shim_rel_target_to_abs` v `readlink`/`readlinkat` (kanonický tvar se hlásí
zpět absolutně — `update-alternatives` cíle porovnává), a `shim_clamp_dotdot`,
který odstraní **jen** ty `..`, co by utekly nad kořen; plná lexikální
normalizace by měnila význam u `/a/symlink/../b`. Knoby
`ELF_LOADER_NO_RELSYM`, `ELF_LOADER_NO_DOTDOT_CLAMP`.

Konverze rootfs `tools/relsymlinks.sh` (`--dry-run`/`--check`/`--revert`,
žurnál TSV). V rootfs je **1093** absolutních linků, z toho **1081
převoditelných**; **celorootfs běh čeká na potvrzení** (zásah do živého
sdíleného rootfs), ověřeno zatím na reálné kopii `/etc/alternatives`
(169/169 + revert). Testy: host unit `test/relsym_test.c` 33/33, A/B
`test-all.sh relsym`, plná sada **193/0/33** (baseline 192/0/33).

*rootfd* (kapitola 4.3) dává per-syscall **−52 %** (`newfstatat`
2626 → 1246 ns, `test/rootfd_bench.c`), ale nemá kam se projevit:
`python3 -c pass` má jen **258** path syscallů s plnou cestou (= 0,36 ms,
2 % totalu), a zátěže, které by vydělaly nejvíc, **už jedou přes `dirfd`**
(`find`: 3088 syscallů s `dirfd` vs 36 s `AT_FDCWD`). Studie předpokládala
„tisíce statů"; měření říká 258. Kapitola 4.4 (`openat2`) na 4.14 nejde.

**Dvě pasti z promptu 03:** (1) konverzní nástroj nesmí volat `readlink` na
každý link (fork na desítky tisíc linků = >20 min); `find -printf '%p\t%l\n'`
to zvládne za 8 min 45 s, ale `-printf` je GNU → nástroj patří spouštět
v guestu, ne pod host toyboxem. (2) A/B test tohohle musí měřit **host**
nástroji (`/system/bin/cat`, `/system/bin/readlink`) — pod loaderem absolutní
cíl zachrání `shim_resolve_symlinks`, takže A/B nedokáže nic (první verze
testu na to FAILovala).

**04 — L1, překlad cest na hranici syscallu.** Hotovo, **default vypnuto**
(`ELF_LOADER_L1=1` vedle jmenných hooků, `=2` navíc vypne 52 hooků „jen
překlad"). Cestová `svc #0` v načtených modulech se přepíší na `b` do
per-site stubu (`src/l1_emit.h`, 9 instrukcí v ostrově do ±128 MiB), jehož
tělo `src/l1_common.S` obnoví všechny registry kromě `x0` a mezi
`l1_before` (překlad) a `l1_after` (fixup `getcwd`/`readlinkat`) provede
syscall sám. Data promptu sedí přesně (411/401/10/62), ale **cestových je
jen 48** (+10 dynamických) na **17** stránkách — COW ~20 stránek, ne 62.

- `test-all.sh l1`: osm případů, které jmenné hooky historicky minuly
  (`eaccess`, `mkdtemp`, `remove`, `glob`, `nftw`, `scandir`, `ftok`,
  `tempnam`), s **vypnutými** hooky: s L1 8/8, bez L1 0/8.
- `test-all.sh all` na finální binárce: default i `ELF_LOADER_L1=2` = **194/0/33** (baseline + `l1`); smoke node/git/gcc/Bun pod `L1=2` OK.
- Cena: bez cache skenu +4 ms na každý exec → seznam míst v cache (v3).
  Po rychlé cestě stub +130 ns (relativní cesta) / +350 ns (absolutní);
  `find /usr` má jen 27,6k cestových syscallů → ~1 %, python import 765 →
  0,1 %, pod šumem. RSS +85 KB.
- Vědomě mimo L1: exec (own-load je sémantika nad syscallem), `mount`,
  `bind`/`connect`; dynamické `x8` jen v libc (Go stacky patří L2).

**Pasti z promptu 04:** syscall ABI zachovává i `x1..x7` (guestu se vrací
původní argumenty, ne přeložené); `stp/ldp` X párů má dosah 504 B; mock
v host testu asm musí registry a NZCV úmyslně ničit, jinak negativní
kontrola projde; žádné velké buffery na zásobníku v cestě z L1 stubu;
loader je `-O0`, takže „už pod `$ROOTFS`" testovat před skenem `..`.

**05 — L2, seccomp RET_TRAP záchytná síť.** Hotovo, **default vypnuto**
(`ELF_LOADER_L2=1`). Pro kód, který L1 nepřepíše (statické binárky, Go, Bun
raw syscally, JIT): cestové `svc` z **textu guest exe** TRAPnou a
`sigsys_handler`→`go_handle` cestu přeloží. Je to **zobecněný Go mód** — ten
dělal přesně tohle, jen pro Go-detekované binárky; `l2_install_range_filter`
(vyčleněno z `elf_go_mode_setup`) teď slouží oběma a nastavuje `g_go_lo/hi`,
takže handler se nemění. PIE exe se přesune do pevného okna `0x11_0000_0000`
(`MAP_FIXED` v `map_elf_segments`), aby text-range nekolidoval s ASLR host
binárek; ET_EXEC (gh/Bun/static) mají pevnou nízkou adresu.

- Statická glibc (`-static`, `-static -no-pie`, `-static-pie`) i Go (`gh`)
  pod L2 vidí rootfs (`/etc/os-release` = Parrot). **Otevřený bug „statický
  glibc SIGSYS RC=159" je pod L2 vyřešený.**
- Bun (`claude --version`) pod L2 **bez `F2_NO_CWD_STRIP`** — `go_handle` +
  `getcwd`/`readlinkat` strip vrací guest cesty.
- Host binárka (`/system/bin/sh`) spuštěná z guestu pod zděděným filtrem
  běží normálně (ASLR text mimo rozsah).
- `test-all.sh l2`: statická binárka vidí rootfs **jen pod L2** (A/B přes
  `ashell -c`, útěk na host — harness běží uvnitř parrot kontextu, jinak
  maskováno). `all` default i `ELF_LOADER_L2=1` **195/0/33** (bez regrese).
- `test-all.sh bionic` pod L2: bridge PASS (host driver mimo guest rozsah).
- **Cena záchytu ~17 µs** (SIGSYS round-trip; 50k× stat: L2=0 ~102 ms vs
  L2=1 ~974 ms). `ELF_LOADER_L2_LAZY=1` → ~24 ms (po 1. záchytu L1 stub,
  ~40× rychleji). Záchyt je drahý → L2 je síť, ne pro horký path syscall.
- Líný přepis `svc`→L1 stub (`ELF_LOADER_L2_LAZY=1`, default vyp): náhrada za
  běhu není ARM-safe souběžně s vykonáváním na jiném jádře; L1 ho dělá
  bezpečně při načtení.
- Pokrytá path čísla jsou běžná sada (open/stat/access vč. faccessat2 439/
  getcwd/readlink/rename/link/symlink/chdir); vzácná (xattr, statfs, chroot…)
  zůstávají na L1/jmenných hookách.

**06 — tabulka mountů + `mount --bind`.** Hotovo (stage 1 `99ae043`,
stage 2 `a2764d6`, dokončení 2026-10-22). `src/binds.h`: longest-prefix
tabulka `guest → host [ro]` nahradila natvrdo zapsaný `shim_excluded`
(default identita = dnešní chování), konfigurace `$ROOTFS/etc/lx-binds`
+ `LX_BINDS`, runtime `mount --bind`/`umount2` přes `shim_mount` do sdílené
MAP_SHARED tabulky (sourozenci i potomci session vidí okamžitě). Ro bindy
`EROFS`, mountpoint `EBUSY`, zpětné `getcwd`. Jiné `LX_BINDS` uvnitř session
= soukromá kopie (`unshare -m`), osiřelé tabulky uklízí GC přes `flock`.
L1 (`l1_before` umí syscall odmítnout) i L2 (`go_translate` hook) sdílí
`shim_bind_deny`.

- `test-all.sh binds` 6/0 (stará binárka 2 PASS + 4 nálezy), totéž pod
  `L1=2` i `L2=1`; `test-all.sh l2` + statická binárka vidí bind jen s novou.
  `all` 201 PASS; 2 FAIL nesouvisí (`nslookup localhost` → NXDOMAIN od
  8.8.8.8 po obnově parrotu, stejně se starou binárkou; imgcache nad kali,
  kde glibc 2.43 pod loaderem visí → nově SKIP).
- **Bezpečnostní nález:** starý `category_binds` je nejpravděpodobnější
  příčina poškození parrotu 2026-10-21 (bind `/tmp/lxbt → /etc` zůstal
  v tabulce harnessu, úklid `rm -rf` pod loaderem smazal `$R/etc`).
- util-linux `umount` *příkaz*, `findmnt` a `cat /proc/mounts` fungují od
  promptu 07 (virtualizovaný mountinfo).

**07 — virtualizace `/proc` + `argv[0]`.** Hotovo 2026-10-22. *Etapa 1:*
exec shim (`shim_execve`, `posix_spawn`, Go/L2 `go_execve`) předává
`--argv0 <argv[0] volajícího>`, guest už nedostává resolvovanou host cestu —
`exec -a`, zsh `ARGV0=`, perl `exec {} LIST` a Claude Code `grep`/`find`
(`ugrep`/`bfs` přes `exec -a`) fungují; `test-all.sh argv0` stará 0/6, nová
6/6. *Etapa 2* (`src/procvirt.h`): `open` jen pro čtení na
`/proc/self/{cmdline,maps,mounts,mountinfo}` → memfd se syntetickým
obsahem (jmenné hooky + L1/L2 přes `shim_sys_intercept`); cizí procesy přes
přepis kernelové oblasti argv v `run_ownall` (`ps -o args` = `foo 2`),
`comm` přes `PR_SET_NAME` (`pgrep -x sleep`), `readlink /proc/self/fd|cwd`
na guest cesty. `test-all.sh procvirt` stará 0/12, nová 12/12 (i pod L1=2
a L2=1); util-linux `umount` příkaz a `findmnt` nad bindem fungují.

- Limity: u skriptů zůstávají v argv host cesty interpretu/skriptu,
  `/proc/self/exe` dál vrací host cestu guest binárky (vědomě), cizí
  `cmdline` čtený host nástrojem má nulovou vycpávku, `df` přiřadí bind k `/`
  (syntetické `0:N`).

### Co z toho plyne pro další prompty

- **11 (soft exec / zygote)** je teď jediná cesta, jak jít pod ~15 ms —
  zbytek exec není relokace ani lookup, ale `hooks_bti` + load + režie
  samotného `execve`.
- Prompt 10 je **uzavřený**, ne „k dodělání": jeho vstupní podmínka
  („relokace/lookupy jsou pořád velký podíl exec") po promptu 02 nedrží.
- **05 (L2) hotovo:** líný přepis využívá `l1_emit_stub`+`l1_common` jako
  předpokládáno; okno ale stačilo jen pro PIE exe (ET_EXEC mají pevnou nízkou
  adresu, závislosti řeší L1), ne „veškerý guest kód". `l1_common` provádí
  `svc` z textu loaderu, tedy mimo okno i mimo L2 rozsah → ALLOW.
- **06/08** (mount tabulka, fake root) mají teď JEDNO místo pro L1
  (`l1_before` → `shim_translate`) i pro L2 (`go_handle` → `go_translate`/
  `f2_realpath`) — rozšiřovat tam, ne v 52 jmenných hookách. Pozn.: L1 a L2
  mají oddělené překladače (`shim_translate` vs `go_translate`), takže
  mount/fakeroot logika se musí dát do obou nebo sjednotit.
- Přepnutí L1 na default: nejdřív `ELF_LOADER_L1=2` v běžném provozu
  (lx/dw, těžké binárky); úplné odstranění open/stat hooků s
  `shim_resolve_symlinks` předpokládá celorootfs konverzi z promptu 03.
- Pro 06–09 se jinak nic nemění, jsou na měření nezávislé.
- **rootfd se k ničemu nevrací, dokud nebude exec pod ~1 ms** — stejný závěr
  jako u kandidáta 3 promptu 02. Měřit vždy *kolik* takových volání cíl má,
  ne jen cenu jednoho.

## Společná pravidla (platí pro každý prompt)

- Repo: `/root/elf_loader` (guest cesta) = `$ROOTFS/root/elf_loader`. Pracovní
  větev `dev`; před prací `git pull origin dev` (feedback auto-merge přidává commity).
- Nejdřív si přečti `AGENTS.md` (rozcestník) a příslušnou kapitolu
  `docs/nextgen/proot-nahrada.md`.
- **Kód, který běží pod guest TP** (shimy patchnuté do glibc, stuby, SIGSYS
  handler) nesmí volat bionic funkce závislé na TLS (`snprintf`, `getpid`,
  `errno`, `malloc`…). Jen `raw_*` / `shim_raw_syscall6`, `shim_str*`.
  Viz past s `/proc/self/exe` fixem v AGENTS.md sekce 10.
- Každá oprava/feature: **A/B regresní test** jako nová kategorie v
  `test-all.sh` (vzor `category_staticexec`), zaregistrovat v `all` i jako
  samostatný argument. Stará binárka FAIL, nová PASS, kde to dává smysl.
- Před commitem: `test-all.sh all` bez regrese (poslední známý stav PASS 179/0).
- Nasazený loader je **bionic NDK build** — na zařízení NDK není. Syntaxi ověř
  `gcc -fsyntax-only -Iinclude src/main.c`, build + deploy přes
  `tools/gh_build_deploy.sh --push` (pushne `dev`, počká na GH Actions, nasadí).
- Ladění: jediný platný testovací kanál je `ashell -c` / loader přímo; `su -c`
  běží přes proot a bugy maskuje.
- Po dokončení: záznam do `postup.md` (deník, čísla, A/B) a hutný řádek do
  `AGENTS.md` do příslušné sekce. Zprávy commitů ve stylu repa
  (`fix:`/`feat:`/`docs:`).
- Neměnit binárky v rootfs (`patchelf` / `PT_INTERP`) — rozbíjí to proot,
  se kterým musí vše koexistovat.
