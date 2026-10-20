# nextgen — sada promptů: loader místo proot

Každý soubor `NN-*.md` je samostatný prompt pro agenta (Claude Code apod.):
zkopíruj celý obsah jako zadání. Teorie, ze které prompty vycházejí, je
v [proot-nahrada.md](proot-nahrada.md) — prompty odkazují na její kapitoly.

## Pořadí a závislosti

| # | prompt | kapitola studie | závisí na | typ | stav |
|---|---|---|---|---|---|
| 01 | [měření a fázový profil exec](01-mereni-benchmark.md) | 9, 5.1 | – | měření | **hotovo** 2026-10-16 |
| 02 | [rychlé výhry exec](02-exec-rychle-vyhry.md) | 5.2–5.4 | 01 | implementace | **hotovo** 2026-10-20 |
| 03 | [relativní symlinky, clamp `..`, rootfd](03-symlinky-a-rootfd.md) | 4.1–4.3 | – | implementace | – |
| 04 | [L1 — přepis `svc #0` při načtení](04-L1-prepis-svc.md) | 2.1 | 01 | implementace | – |
| 05 | [L2 — seccomp TRAP v guest okně](05-L2-sigsys-okno.md) | 2.2 | 04 | implementace | – |
| 06 | [tabulka mountů + `mount --bind`](06-mount-tabulka.md) | 3.1 | 04 (doporučeno) | implementace | – |
| 07 | [virtualizace `/proc` + `argv[0]`](07-proc-a-argv0.md) | 3.4, 3.5 | – | implementace | – |
| 08 | [fake root + link2symlink](08-fakeroot-l2s.md) | 3.2, 3.3 | 04 (doporučeno) | implementace | – |
| 09 | [proot-kompatibilní CLI + hybrid](09-proot-cli-hybrid.md) | 3.6, 7 | 06, 08 | implementace | – |
| 10 | [cache obrazů + líné vázání](10-exec-image-cache.md) | 5.5, 5.6 | 01, 02 | implementace | **hotovo** 2026-10-20 |
| 11 | [soft exec / zygote — výzkum](11-soft-exec-zygote.md) | 5.7, 5.8 | 01, 10 | výzkum + prototyp | – |
| 12 | [nativní ld.so mód — prototyp](12-nativni-ldso.md) | 6 | 04, 05 | výzkum + prototyp | – |

03 a 07 jdou dělat kdykoli, jsou nezávislé.

## Co je hotové (stav 2026-10-20)

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

### Co z toho plyne pro další prompty

- **11 (soft exec / zygote)** je teď jediná cesta, jak jít pod ~15 ms —
  zbytek exec není relokace ani lookup, ale `hooks_bti` + load + režie
  samotného `execve`.
- Prompt 10 je **uzavřený**, ne „k dodělání": jeho vstupní podmínka
  („relokace/lookupy jsou pořád velký podíl exec") po promptu 02 nedrží.
- Pro 03–09 se nic nemění, jsou na měření nezávislé.

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
