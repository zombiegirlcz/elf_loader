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
