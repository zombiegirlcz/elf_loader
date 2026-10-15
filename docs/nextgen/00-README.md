# nextgen — sada promptů: loader místo proot

Každý soubor `NN-*.md` je samostatný prompt pro agenta (Claude Code apod.):
zkopíruj celý obsah jako zadání. Teorie, ze které prompty vycházejí, je
v [proot-nahrada.md](proot-nahrada.md) — prompty odkazují na její kapitoly.

## Pořadí a závislosti

| # | prompt | kapitola studie | závisí na | typ |
|---|---|---|---|---|
| 01 | [měření a fázový profil exec](01-mereni-benchmark.md) | 9, 5.1 | – | měření |
| 02 | [rychlé výhry exec](02-exec-rychle-vyhry.md) | 5.2–5.4 | 01 | implementace |
| 03 | [relativní symlinky, clamp `..`, rootfd](03-symlinky-a-rootfd.md) | 4.1–4.3 | – | implementace |
| 04 | [L1 — přepis `svc #0` při načtení](04-L1-prepis-svc.md) | 2.1 | 01 | implementace |
| 05 | [L2 — seccomp TRAP v guest okně](05-L2-sigsys-okno.md) | 2.2 | 04 | implementace |
| 06 | [tabulka mountů + `mount --bind`](06-mount-tabulka.md) | 3.1 | 04 (doporučeno) | implementace |
| 07 | [virtualizace `/proc` + `argv[0]`](07-proc-a-argv0.md) | 3.4, 3.5 | – | implementace |
| 08 | [fake root + link2symlink](08-fakeroot-l2s.md) | 3.2, 3.3 | 04 (doporučeno) | implementace |
| 09 | [proot-kompatibilní CLI + hybrid](09-proot-cli-hybrid.md) | 3.6, 7 | 06, 08 | implementace |
| 10 | [cache obrazů + líné vázání](10-exec-image-cache.md) | 5.5, 5.6 | 01, 02 | implementace |
| 11 | [soft exec / zygote — výzkum](11-soft-exec-zygote.md) | 5.7, 5.8 | 01, 10 | výzkum + prototyp |
| 12 | [nativní ld.so mód — prototyp](12-nativni-ldso.md) | 6 | 04, 05 | výzkum + prototyp |

03 a 07 jdou dělat kdykoli, jsou nezávislé.

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
