# Testing loop in this environment (skills.md)

## ⚠️ POVINNÁ PRAVIDLA PŘED KAŽDÝM "funguje"/"opraveno" TVRZENÍM

Tohle jsou dvě nejčastější příčiny falešně pozitivních testů v tomhle projektu. Před tím, než
cokoliv prohlásíš za funkční/opravené, projdi obě:

1. **VŽDY testuj bionic binárku, NIKDY glibc build.**
   Ověř interpreter PŘED testem, ne až po chybě:
   ```sh
   readelf -h /tmp/elf_loader_ndk | grep Machine        # musí být AArch64
   readelf -l /tmp/elf_loader_ndk | grep -i interpreter  # musí být /system/bin/linker64
   ```
   Pokud vidíš `/lib/ld-linux-aarch64.so.1` → je to glibc build z proot toolchainu
   (`aarch64-linux-gnu-gcc`), NE výstup z `modal run finale_loader_build.py`. Na device
   spadne s `RC=126 / No such file or directory` — a tahle hláška vypadá jako "binárka
   neexistuje", i když existuje, jen ji kernel nemá čím spustit. Nikdy nedeployuj/netestuj
   binárku, u které jsi tohle neověřil.

2. **VŽDY testuj přes `ashell -c '...'`, NIKDY uvnitř proot session.**
   Proot je jen editor/kompilační sandbox (`/root/elf_loader`) — NENÍ to prostředí, ve kterém
   uživatel binárku skutečně spouští. Proot má vlastní glibc userland, jiný resolving cest,
   jinou seccomp/namespace situaci než skutečný device kontext (app uid 10310, žádný `su`).
   Test spuštěný uvnitř proot session (`bash`, přímé volání binárky bez `ashell -c`) **nic
   neříká o tom, jestli to funguje na device** — může projít v proot a spadnout v ashell, nebo
   naopak. Jediný platný test je:
   ```sh
   ashell -c '<přesný device příkaz, absolutní cesty>'
   ```
   Než napíšeš "funguje"/"opraveno", vlož SEM doslovný výstup + `RC=$?` z tohoto konkrétního
   `ashell -c` volání. "Zkompilovalo se to" nebo "proběhlo to v prootu" není důkaz.

3. **Nikdy nevolej `elf_loader` přímo s holým jménem binárky.**
   `elf_loader --ownall ls` hledá `ls` v aktuálním cwd, ne v `$ROOTFS` — selže s
   `open(ls): No such file or directory`, což vypadá jako regrese, ale je to jen špatná
   invokace. Buď použij plnou device cestu (`$L --ownall $R/usr/bin/ls`), nebo `elroot`
   wrapper, co dělá resolving za tebe.

## Prostředí — co kde je

- **Proot (lokální workspace agenta)**: `/root/elf_loader`. Tady píšu kód, tady běží `modal`, `bash`, kompilace.
- **Device mount**: `files/` v repu je bind‑mount na device `/data/user/0/com.linux_core/files`
  (privátní data složka appky, vlastník `u0_a310` = uid 10310).
  ⇒ zápis do `/root/elf_loader/files/...` == zápis na device. Žádný `adb push`, žádný `su`.
- **Sdílený rootfs**: `files/nh/distro/parrot` == device `/data/.../files/nh/distro/parrot`
  (parrot/NetHunter glibc rootfs, proměnná `$ROOTFS`). Proot a device vidí **identické** prostředí —
  to je důvod, proč testovací smyčka funguje konzistentně bez přepínání kontextu.
- **`ashell` (app shell)**: binárka na device, která spustí příkaz jako app uid 10310 a vrátí
  stdout/stderr. Volá se z prootu jako `ashell -c '<prikaz>'`. NENÍ root (žádné `su`/`chroot`/`mount`).

## Testovací smyčka — přesný popis

```
1. Napíšeš kód              →  /root/elf_loader/src/*.c  (v prootu)
2. Zkompiluješ             →  modal run finale_loader_build.py
                              (NDK cross‑compile, cloud image; výstup /tmp/elf_loader_ndk)
3. Pushneš ven z prootu    →  cp /tmp/elf_loader_ndk /root/elf_loader/files/usr/bin/elf_loader
                              && chmod 755        (jde rovnou na device přes mount)
4. Otestuješ přes ashell   →  ashell -c '<device prikaz>'
```

Krok po kroku:

1. **Kód** edituješ lokálně v `/root/elf_loader/src/`. Žádný vzdálený editor.
2. **Kompilace** běží přes `modal` (NDK není lokálně; Modal postaví debian_slim + android‑ndk‑r28
   a cross‑compiluje `aarch64-linux-android24-clang`). Výstupní binárka přistane v prootu
   (`/tmp/elf_loader_ndk`). Musí být **bionic** (interpreter `/system/bin/linker64`) — over to
   pokaždé přes `readelf -l /tmp/elf_loader_ndk | grep -i interpreter` PŘED deployem, ne až
   ve chvíli, kdy to na device spadne. Glibc build (`aarch64-linux-gnu-gcc`, interpreter
   `/lib/ld-linux-aarch64.so.1`) na device padá s `RC=126 / No such file or directory`
   (kernel nenajde glibc loader) — snadno se to splete se skutečnou regresí.
3. **Deploy** = prosté `cp` do `files/usr/bin/`. Mount to zapíše na device. Žádný `su`.
4. **Test VÝHRADNĚ přes `ashell -c`** — nikdy přímým voláním binárky v proot session (proot
   je jen kompilační sandbox, ne testovací prostředí; viz "POVINNÁ PRAVIDLA" výše). Device
   cesty jsou absolutní:
   `/data/user/0/com.linux_core/files/usr/bin/elf_loader`.
   Loader se obvykle řídí přes `elroot`:
   `ROOTFS=… ELF_LOADER=… GBSH=… /data/…/elroot --shim <guest-cmd>`.

## ashell konfigurace

- **Config**: `ashellrc=/root/elf_loader/files/ashell.conf` (na device, zrcadleno v `files/ashell.conf`).
  Nastavuje env app shellu (PATH, ROOTFS, ELF_LOADER, GBSH, TERMINFO, …) a ukazuje ashell na
  správné device adresáře.
- **`apps hell` (ashell)** = omezený shell appky; běží jako uid 10310 v SELinux doméně appky.
  Není root. Všechny testy jdou přes něj.

## Kritické zádrhely (ověřeno v praxi)

- **Proměnné (`$D $R $L $E $G`) se NEUDRŽÍ** mezi samostatnými `bash` voláními. Vždy je nastav
  ve stejném příkazu, co je používá.
- **ashell má svůj vlastní `$D`** (parrot rootfs). Holé příkazy uvnitř `ashell -c '…'`
  (`ls`, `head`, `id`) se přepíšou na parrot cesty → „No such file". Pro host nástroje používej
  absolutní `/system/bin/ls`; pro své binárky literální device cesty.
- **Limit `ashell` ~1024 znaků** na příkaz + stateful bezpečnostní filtr blokující mnoho podřetězců.
  Test příkazy drž krátké; dlouhé pushe děl na chunky; vyhýbej se zakázaným slovům.
- **seccomp filtr přežije `execve`, ale SIGSYS handler se resetuje** → re‑exec’nuté děti handler
  ztratí (to je hard limit multi‑process na tomto kernelu 4.14).
- **NESAHEJ na systémové ownery/perms** (bootloop riziko); deploy jen kopírováním do `files/`
  (== `$F`).

## Doplňky ze session 2026-10-07 (path-translace)

- **Větve: vývoj a testy jdou do `dev`, `master` je produkční.** Opravy commituj na `dev`
  (`git checkout dev && git pull origin dev`); `master` se posouvá jen z `dev` (ff) po
  ověření celé sady. Necommituj přímo na `master`.
- **Push na `master` vždy s tagem verze** (konvence `vX.Y`, poslední `v0.3`): po ff
  `master` ← `dev` vytvoř anotovaný tag a pushni oba najednou:
  ```sh
  git checkout master && git merge --ff-only dev
  git tag -a v0.4 -m "release: v0.4 — <shrnutí>"
  git push origin master v0.4
  ```
  Push na `master` bez tagu nedělej.

- **Repo vs. testovací kopie:** git repo je `/root/elf_pro`; `/root/elf_loader` je pracovní
  kopie, ze které běží `test-all.sh`. Po úpravě kopíruj **jen** `test-all.sh`:
  `cp /root/elf_pro/test-all.sh /root/elf_loader/`. NIKDY nekopíruj `src/*.c` do kořene
  `/root/elf_loader` (git-agent je auto-commitne do `dev`).
- **git-agent auto-commituje i `/root/elf_pro` a PUSHUJE na `origin/master`** (commit
  „<datum> git-agent" s rozpracovaným stavem). Nenechávej rozbitý mezistav v `src/` dlouho
  ležet; před vlastním commitem zkontroluj `git log`/`git status -sb`.
- **`su -c` na tomto zařízení = proot** (`PROOT_L2S_DIR`) → překládá cesty na úrovni syscallů
  a path bugy maskuje. Platí jen `ashell -c`.
- **Deploy s A/B zálohou:**
  ```sh
  cp $L $D/usr/bin/elf_loader.prev; cp /tmp/elf_loader_ndk $L; chmod 755 $L
  ```
  Každou opravu ověř A/B: stejný test s `$D/usr/bin/elf_loader.prev` musí FAILnout,
  s novou PASSnout (jinak test nic nedokazuje).
- **Celá sada** (trvá > 10 min → spouštěj na pozadí, výstup do scratchpadu):
  ```sh
  ashell -c "ROOTFS=$R ELF_ROOTFS=$R HOME=$D L=$L $L --ownall $R/bin/bash /root/elf_loader/test-all.sh all"
  ```
  Jednotlivá kategorie: `... test-all.sh pathops`. A/B kategorie: nastav `L=…elf_loader.prev`.
  Stav k 2026-10-07: **PASS 177 / FAIL 0 / SKIP 33**.
- **C testy glibc funkcí:** zdroj dej pod `$R/tmp/...`, kompiluj guest gcc přes loader:
  ```sh
  ashell -c "export ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L PATH=/usr/bin:/bin:/system/bin; \
    $L --ownall $R/usr/bin/gcc -o /tmp/tmpt/t3 /tmp/tmpt/t3.c && $L --ownall $R/tmp/tmpt/t3"
  ```
  Regresní testy do `test-all.sh` piš jako Python `ctypes` skript (`c = ctypes.CDLL(None)`)
  s výstupem `NAZEV <počet>` — viz `category_pathops` (`LIBCOPS`, `LIBCOPS2`).
- **Pasti v ashell prostředí:** `TMPDIR=$D/cache` (host) → `tempnam` ho podle POSIX
  použije přednostně; v testu `unset TMPDIR`. Bare `timeout`/`bash` uvnitř guestu se
  resolvují na host `/system/bin` → používej `$R/usr/bin/timeout`, `$R/bin/bash`.
- **Analýza glibc lokálně** (objdump/nm jsou v prootu):
  ```sh
  LIBC=$R/usr/lib/aarch64-linux-gnu/libc.so.6
  nm -D $LIBC | grep ' fstatat'            # aliasy = stejná adresa
  objdump -d --start-address=0x... --stop-address=0x... $LIBC   # prolog: bti c / paciasp / b
  ```
  `d503245f` = `bti c`, `d503233f` = `paciasp` → `hook_install` je nepatchuje, jen GOT override
  (interní `bl` ho obchází) → `hook_inline_prologue`. Po inline hooku musí všechny `g_orig_*`
  se stejnou adresou dostat trampolínu (jinak rekurze).
- **Shimy běží pod GUEST TP:** žádné bionic TLS funkce (`snprintf`, `getpid`, `getenv`,
  `fprintf`, `memset`) — jen `shim_*` helpery, `shim_raw_syscall6`, `shim_guest_errno_set`.
- **Po změně path shimů vždy otestuj i Bun (claude) s cwd pod `$R`** — Bun dělá fs raw
  syscally, guest cesty pro něj neexistují:
  ```sh
  ashell -c "export ROOTFS=$R ELF_ROOTFS=$R ELF_LOADER=$L HOME=$D; cd $R/tmp/cwdt; \
    $L --ownall $R/root/claudetest/claude.exe -p hi </dev/null"
  ```
  Úspěch = `Not logged in · Please run /login`; chyba = `Can't access working directory`.
  (`BUN_BE_BUN=1` u této verze nefunguje.) Dále node (`$R/root/.nvm/versions/node/v26.10.0/bin/node`),
  python, git a cJSON (`$R/tmp/cjson`, guest PATH `/usr/local/bin:/usr/bin:/bin:/system/bin`,
  cmake -S/-B → build → ctest 19/19).
- **Vypínače pro bisekci:** `F2_DISABLE`, `F2_ONLY=a,b`, `F2_NO_INTERNAL_HOOK`,
  `F2_NO_OPEN_HOOK`, `F2_NO_SPAWN_HOOK`, `F2_NO_BTI_HOOK`, `F2_NO_CWD_STRIP`;
  diagnostika `ELF_DEBUG=1` (`[hook] ... inline OK`), `ELF_LOADER_EXEC_TRACE=1`.

## Příklad (loader smoke test)

```sh
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot; L=$D/usr/bin/elf_loader; E=$D/usr/bin/elroot; G=$D/usr/bin/gbsh

# 2+3: compile (modal) -> /tmp/elf_loader_ndk, deploy do device
cp -f /tmp/elf_loader_ndk /root/elf_loader/files/usr/bin/elf_loader && chmod 755 /root/elf_loader/files/usr/bin/elf_loader

# 4: test pres ashell (literal device cesty, bez pipe kvuli head manglingu)
ashell -c "$L --ownall /system/bin/true; echo RC=\$?"
ashell -c "ROOTFS=$R ELF_LOADER=$L GBSH=$G $E --shim ls /usr/bin/awk"
```

Flow v jedné větě: **píšeš v prootu → `modal` zkompiluje NDK do `/tmp` → `cp` do `files/`
(= device) → `ashell -c` spustí na device pod app uid 10310**.
