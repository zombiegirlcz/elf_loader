# Loader místo proot — teorie a nápady

*2026-10-15 · čistě na papíře, nic z toho není implementované. Fakta v kapitole 0
jsou ověřená na zařízení (M2101K6G, jádro 4.14.190, Parrot glibc 2.41). Čísla
v kapitolách 1 a 5 jsou řádové odhady — ověří je benchmark z kapitoly 9.*

## Shrnutí

Proot je pomalý ze tří důvodů: každý cestový syscall stojí dvě přepnutí kontextu
přes ptrace, proot kanonikalizuje cesty po komponentách vlastními `lstat`/`readlink`
a všechny procesy obsluhuje jediný tracer. Loader první dva problémy nemá, protože
překládá přímo v procesu. Prohrává ale ve dvou věcech:

- **pokrytí** — zachytává ~150 jmen funkcí místo syscallů, takže uniknou statické
  binárky, Go, Bun, raw `svc`, musl, JIT a interní volání glibc;
- **cena exec** — 48 ms proti 10 ms nativně.

Nejsilnější nápad je **přesunout zachytávání z jmen funkcí na syscally, ale
pořád v procesu**. Při načtení se přepíše všech 411 `svc #0` v glibc na skoky do
překladových stubů. Zbytek (statické binárky, JIT, Go) chytí seccomp filtr
`SECCOMP_RET_TRAP`, který platí jen pro adresní „okno" guest kódu. Loader tak
získá úplnost proot bez ptrace. Exec pak zrychlí cache připravených obrazů (stejný
princip, jakým Android sdílí RELRO pro WebView), případně „soft exec" bez `execve`.

Na konec strategická otázka: přejít na **nativní glibc proces**, tj. spouštět
`ld-linux-aarch64.so.1` přímo jako program s preload knihovnou? Zmizela by celá
třída chyb, které vznikají tím, že v jednom procesu žijí dvě libc (TP/TLS/signály).
Ztratil by se ale GPU bridge a nezávislost na verzi glibc v rootfs.

---

## 0. Výchozí fakta (ověřeno 2026-10-15)

| fakt | důsledek |
|---|---|
| SELinux doména `untrusted_app_27` (targetSdk ≤ 28) | `execve` souborů z app data funguje (proto běží proot i `su_wrapper`). S targetSdk ≥ 29 zakáže W^X `execve` souborů z app data. `mmap PROT_EXEC` zůstává povolený — proto termux-exec umí spouštět přes `/system/bin/linker64`. |
| jádro 4.14.190 | chybí `SECCOMP_RET_USER_NOTIF` (5.0), `clone3`/pidfd (5.3), `openat2` + `RESOLVE_IN_ROOT` (5.6), Syscall User Dispatch (5.11), rseq (4.18) |
| proces si smí nainstalovat vlastní seccomp filtr | loader to už dělá (`install_legacy_syscall_filter`) |
| app profil některé syscally KILLuje (rseq, clone3, …) | KILL nejde přebít filtrem ani SIGSYS handlerem (`src/sigsys_probe.c`), tam pomůže jen přepis `svc` |
| `mount(2)` → `ENOSYS`; user namespaces ani FUSE nejsou; hardlinky zakazuje SELinux | žádná kernelová emulace chrootu bez rootu |
| loader už umí přepsat `svc #0` (`elf_patch_syscall_sites`, `src/elf_loader.c:1942`) a má SIGSYS handler | infrastruktura pro kapitolu 2 existuje |
| jmenné záchytné body: 125 položek v tabulkách hooků + 26× `elf_register_override` + 7 inline hooků | to je ten „whack-a-mole", který chce kapitola 2 nahradit |
| glibc 2.41 má **411** míst `svc #0` na **62** z 293 stránek `.text`; u 401 je `x8` konstantní, u 10 dynamické | úplný přepis glibc = ~62 COW stránek (~250 KB) na proces |
| text glibc je v procesu pod loaderem file-backed `MAP_PRIVATE`, patchované stránky jsou COW (rozdělené VMA v `/proc/self/maps`) | page cache se mezi procesy sdílí, privátní jsou jen patchované stránky |
| `ld-linux-aarch64.so.1` jde na zařízení spustit přímo jako program (postup.md:318) | základ kapitoly 6 |
| přepis `PT_INTERP` v rootfs (`patchelf_interp`) rozbil proot (postup.md:334) | řešení musí s proot koexistovat, tj. žádné úpravy binárek v rootfs |
| exec (postup.md:3917): nativně 10 ms, loader přímo 14 ms, přes shim 48 ms | rezerva 38 ms na každý exec |

---

## 1. Kde proot ztrácí čas

**Mechanismus.** Proot je tracer, programy pod ním jsou tracees. Seccomp filtr proot
pošle cestové syscally do `PTRACE_EVENT_SECCOMP`; jádro zastaví tracee a probudí
tracer. Ten přečte registry, přečte cestu z paměti tracee, přeloží ji, zapíše novou
cestu na zásobník tracee, upraví registry a pustí tracee dál. U syscallů, které cestu
vracejí ve výsledku (`getcwd`, `readlink`, `/proc/self/exe`), je ještě jedno
zastavení na výstupu.

**Zdroje ceny:**

1. **Dvě přepnutí kontextu na syscall** (čtyři s exit-stopem), pokaždé přes plánovač.
2. **big.LITTLE.** Tracer a tracee na různých jádrech znamenají IPI, latenci
   probuzení a studené cache. (Pinuješ `nh cpu pin 7` — stojí za ověření, že tracer
   i tracee skončí na stejném clusteru.)
3. **Kanonikalizace v userspace.** Proot emuluje chroot sám, takže prochází cestu
   po komponentách a na každou volá `lstat`/`readlink`. Jeden
   `stat("/usr/lib/python3/dist-packages/x/y.py")` tak může stát ~8 syscallů navíc.
4. **Jeden tracer pro všechno.** Event loop nad `waitpid` běží v jednom vlákně, takže
   `make -j8` se na každém cestovém syscallu serializuje.
5. **Signály.** Každý signál prochází signal-delivery-stop. Platí za to JVM (SIGSEGV
   pro safepointy a null-checky), Go (SIGURG preempce ~každých 10 ms na vlákno)
   i Node. Týká se to i sdkman/Javy.
6. **fork/clone/exec.** Každý potomek a každé vlákno se auto-attachuje, exec jde
   přes proot loader.
7. Bez seccomp akcelerace (`PROOT_NO_SECCOMP`, staré jádro) se zastavuje *každý*
   syscall, nejen cestový.

**Řádový model přirážky na jeden cestový syscall** (odhad, ověřit v kapitole 9):

| mechanismus | přirážka |
|---|---|
| nativní syscall (stat z dcache) | základ, ~1 µs |
| GOT/inline hook loaderu | desetiny µs (prefix + jádro navíc projde 8 komponent `$ROOTFS`) |
| SIGSYS trap v procesu | jednotky µs |
| proot (seccomp + ptrace + kanonikalizace) | desítky µs |

Kde loader zachytí, tam proot poráží. Problém je, **co nezachytí** a **kolik stojí
exec**.

---

## 2. Pokrytí: zachytávat syscally, ne jména funkcí

Dnes loader zachytává podle jména funkce. Každý nový případ se řeší ručně: alias
`eaccess`, interní `bl` v glibc obcházející funkce s BTI prologem, `__mkdir`
v `mkdtemp`, `remove`, `glob`, `nftw`… Z principu nepokryje kód, který volá `svc`
sám: statické binárky (`su_wrapper`), Go, Bun (raw syscally, proto
`F2_NO_CWD_STRIP`), musl, JIT. Proot je úplný z konstrukce, protože sedí na hranici
syscallů. Cílem je dostat se na stejnou hranici, jen bez ptrace.

### 2.1 L1 — přepis všech cestových `svc #0` při načtení

Zobecnit `elf_patch_syscall_sites`: místo NOP/`movn` přepsat `svc #0` na skok do
stubu, který patří jen tomu jednomu místu.

- **Skákat přes `b`, ne `bl`.** `bl` přepíše `x30` a v listových funkcích (syscall
  wrappery glibc listové typicky jsou) leží v `x30` návratová adresa, takže by se
  rozbil `ret`. Každé místo proto dostane vlastní stub zakončený `b <místo+4>`
  a návratovou adresu zná staticky.
- Dosah `b` je ±128 MiB. Loader si adresy volí sám, takže vedle každého modulu
  umístí „ostrov stubů".
- Stub uloží registry a podle čísla syscallu přeloží cestové argumenty. Tabulka:
  `openat`→x1, `newfstatat`→x1, `execve`→x0, `renameat2`→x1+x3, `linkat`→x1+x3,
  `mkdirat`, `unlinkat`, `faccessat`, `statx`, `readlinkat`, `chdir`,
  `inotify_add_watch`, … Pak sám provede `svc #0` a opraví výsledek (`getcwd`,
  `readlinkat`).
- U 401 míst je `x8` konstantní (`mov x8,#nr` těsně před `svc`), takže číslo
  syscallu je známé staticky a stub překládá jen to, co je potřeba. U 10 míst je
  `x8` dynamické — `syscall()` a nejspíš i centralizovaná cancelace v 2.41 (ověřit
  disassemblací). Tam stub rozhodne podle `x8` až za běhu.
- **Překladový buffer nedávat na zásobník.** Go goroutines startují s 8 KB. Lepší je
  per-vláknová oblast indexovaná podle `tid`, bez TLS — stejná disciplína jako
  u dnešních raw shimů.
- Výsledek scanu (offsety a čísla) cachovat podle BuildID. Dnes
  `elf_patch_syscall_sites` při každém exec znovu prochází `.text` pro 5 čísel
  (99, 151, 152, 293, 435).
- Paměťová cena: ~62 COW stránek glibc na proces.

**Co to odstraní:** aliasy (`eaccess`), BTI prology, interní volání, whitelist
`__open64_nocancel` a většinu jmenných hooků. Navíc **přestane být potřeba
„stripovat $R"** u `glob`/`nftw`/`getcwd`. Když se překládá až na úrovni syscallu,
program celou dobu pracuje s guest cestami a host cesta se k němu nikdy nedostane.
Jedinou výjimkou jsou výsledky `getcwd`/`readlinkat` a ty opraví stub.

**Přechod je bezpečný postupně:** jmenné hooky a L1 můžou běžet současně. Cestu, která
už začíná `$ROOTFS`, `shim_translate` nechá být, takže nevznikne dvojitý prefix.

**Varianta 2.1b — vlastní build glibc.** Místo binárního patchování přeložit
`libc.so.6` 2.41 (stejná verze a symbol versions jako Parrot) s překladem přímo
v makru `INLINE_SYSCALL` pro cestové syscally. Loader ji pak načte místo té
z rootfs a upgrade `libc6` v rootfs bude ignorovat. Robustnější než patch, ale
vyžaduje build pipeline (GH Actions už máte) a sledování verzí Parrotu.

### 2.2 L2 — záchytná síť: seccomp `RET_TRAP` jen pro „okno" guest kódu

Pro kód, který L1 nepřepíše: statické binárky, Go, Bun, JIT, samomodifikující kód.

- **Filtr:** pro cestová čísla syscallů platí — leží-li
  `seccomp_data.instruction_pointer` v guest okně, `SECCOMP_RET_TRAP`, jinak `ALLOW`.
- **Guest okno** je vyhrazený rozsah VA, do kterého loader dává veškerý guest kód,
  např. `0x10_0000_0000–0x20_0000_0000`. Ve 39bitovém VA dává jádro PIE kolem
  `0x55…`–`0x5a…` a mmap kolem `0x71…`–`0x7f…` (viz adresy v logu loaderu), takže
  okno leží daleko od obou. Při startu ho stačí rezervovat jedním `PROT_NONE` mmapem
  a moduly do něj mapovat s `MAP_FIXED` (`MAP_FIXED_NOREPLACE` přišel až v 4.17).
- **Handler** běží v procesu, je async-signal-safe a používá jen raw syscally.
  Přečte registry z `ucontext`, přeloží cestu, provede syscall sám z textu loaderu
  (ten leží mimo okno, takže projde přes `ALLOW`), zapíše `x0` a vrátí se.
  **Líný přepis:** kde to jde, rovnou to místo přepíše podle L1 a další volání už
  signál nestojí (princip lazypoline; ten na x86 používá SUD).
- **Proč okno, a ne „povol rozsahy bioniku"?** Seccomp filtr dědí každý potomek
  a **nejde odstranit**. Host binárka spuštěná později (`/system/bin/sh`, `toybox`,
  `am`) má náhodné ASLR adresy, takže filtr „povol jen známé rozsahy bioniku" by ji
  zabil. Filtr „chytej jen v okně" je pro ni neškodný, protože její kód v okně nikdy
  neleží.
- **Okno přesně odpovídá sémantice bionic bridge.** Guest kód se překládá, host kód
  (bionic loader, Vulkan driver volaný přes `bb_stubs`) ne. Driver tak dál otevírá
  host cesty a nic dalšího se pro to dělat nemusí.
- **Statické non-PIE binárky** leží na pevných adresách (`0x200000`, `0x400000`),
  tedy mimo okno. Když je loader spustí raw (jako dnes `su_wrapper`), filtr se jich
  netkne. Do okna patří jen tehdy, když je loader own-loaduje a pod nimi je handler.
  Bez handleru by zemřely na SIGSYS.
- **Priorita stacked filtrů:** KILL > TRAP > ERRNO > ALLOW. App profil cestové
  syscally povoluje, takže náš TRAP vyhraje. Kde app profil KILLuje, nepomůže nic
  (proto zůstává patchování rseq/clone3).
- Každý exec přidá jeden filtr, protože filtry odebrat nejdou. Řetěz
  shell→make→gcc→cc1→as znamená 5 malých filtrů, to je zanedbatelné; strop je 32K
  instrukcí celkem.
- Guest programy s vlastním SIGSYS handlerem nebo seccompem (sandbox
  Chromia/Electronu) se virtualizují přes existující `guest_sigaction`.
- **JIT:** guest `mmap` s `PROT_EXEC` jde přes hook umístit do okna. Pak se chytí
  i JIT kód s raw syscally.

**Výsledek L1+L2:** rootfs vidí i statické binárky, Go, Bun, musl a JIT. Dnešní
oprava `su_wrapper` (statická binárka → raw exec, takže vidí host FS) by se pak
dala obrátit: binárku own-loadovat a překládat.

### 2.3 Výsledné vrstvy

| vrstva | co chytá | cena |
|---|---|---|
| L1 přepis `svc` při načtení | glibc a všechny načtené moduly | ~0 (jen překlad) |
| L2 SIGSYS v okně + líný přepis | statické binárky, Go, Bun, JIT | jeden signál na místo, pak ~0 |
| L3 jmenné hooky | jen sémantika nad úrovní syscallů (`posix_spawn` → loader, setuid shimy) | jako dnes |

---

## 3. Funkce proot, které loader musí umět, aby ho nahradil

### 3.1 Bind mounty (`proot -b`) → uživatelská tabulka mountů

`shim_excluded` je vlastně natvrdo zapsaná tabulka bindů. Zobecnit ji na
longest-prefix tabulku `guest prefix → host cesta [ro]`, konfigurovatelnou přes
`$ROOTFS/etc/lx-binds` nebo env `LX_BINDS`. Zpětné mapování (host→guest) pro
`getcwd`/`readlink` vezme ze stejné tabulky.

**Bonus — `mount --bind` uvnitř guestu.** `mount(2)` je stejně `ENOSYS`, takže shim
může `MS_BIND` místo toho zapsat do tabulky, sdílené pro potomky přes soubor nebo
memfd. Přesně to, co jsi zkoušel s `/home/deamon`, by pak fungovalo.

Pozor na `..` přes hranici mountu: před překladem cestu lexikálně normalizovat, jak to
dělá proot.

### 3.2 Fake root (`proot -0`)

- `getuid/geteuid/getresuid/getgid…` vracejí 0 (volitelně).
- `stat` souboru, jehož vlastník je app uid, hlásí vlastníka 0.
- `chown`/`lchown`/`fchownat` uspějí a zapíšou vlastníka do metadatové DB.
- `mknod` vytvoří obyčejný soubor a záznam v DB; `stat` pak hlásí `S_IFCHR`.

DB může být sidecar soubor klíčovaný `(dev, ino)` jako `faked` z fakeroot, nebo
`user.*` xattry, pokud je f2fs na `/data` povoluje (ověřit). Potřebuje to dpkg/apt.

### 3.3 link2symlink (`proot -l`, `PROOT_L2S_DIR`)

Hardlinky zakazuje SELinux. Emulace: `link()` přesune originál do skrytého
`.l2s/<id>`, z obou jmen udělá symlinky a vede počítadlo odkazů; `unlink` počítadlo
snižuje. Loader už má speciální případ pro `/dev/shm`
(`renameat2(RENAME_NOREPLACE)`), stačí ho zobecnit.

### 3.4 Virtualizace `/proc`

`/proc/self/exe` je hotové. Chybí `cmdline` (otevřený nález), `maps` (ukazuje
loader), `mounts`/`mountinfo` (vygenerovat z tabulky 3.1), `fd/N` a `cwd` (odříznout
`$ROOTFS`). Technika: při `openat` na takovou cestu vytvořit `memfd`, zapsat do něj
syntetický obsah a vrátit fd (`memfd_create` jádro 4.14 má; ověřit app profil).

### 3.5 Drobnosti

- `proot -k` (falešná verze jádra) → hook `uname`.
- `proot -q` (cizí architektura) → exec shim podle `e_machine` spustí `qemu-user`
  s `-L $ROOTFS`.
- `argv[0]` (otevřený nález): exec shim původní hodnotu zná, stačí ji předat loaderu
  zvlášť (`--argv0`).

### 3.6 Drop-in rozhraní

Naučit `elf_loader` příkazovou řádku proot (`-r`, `-b`, `-w`, `-0`, `-l`, `-k`), aby
v `ProotManager.kt` stačilo vyměnit binárku. Co loader neumí, předá skutečnému
proot (kapitola 7, hybrid).

---

## 4. Symlinky a `..` — největší sémantický rozdíl proti chrootu

**Problém:** absolutní symlink uvnitř `$ROOTFS` vyhodnotí jádro vůči *host* kořeni
(`/etc/alternatives/awk → /usr/bin/mawk` tak míří mimo rootfs). Loader to dnes řeší
jen pro exec (`shim_resolve_symlinks`), proot všude, ale drahou kanonikalizací.

**4.1 Absolutní symlinky jednorázově převést na relativní** (jako `symlinks -cr`):
`/etc/alternatives/awk → ../../usr/bin/mawk`. Jádro je pak uvnitř `$ROOTFS`
vyhodnotí správně, **za nulovou cenu za běhu**. Invariant udrží shim `symlinkat`:
absolutní cíl (dpkg, `update-alternatives`, `ln -s /x`) zapíše rovnou relativně.
Shim `readlinkat` vrací cíl v „kanonickém tvaru výstupu ke kořeni" (přesně
hloubka × `../` + zbytek) jako absolutní, takže `update-alternatives`, který cíle
porovnává, vidí původní tvar. Riziko heuristiky: skutečný relativní link přesně
tohoto tvaru se ohlásí jako absolutní — uvnitř chrootu je to ale sémanticky totéž.

**4.2 `..` nad kořenem.** V chrootu platí `/..` = `/`, kdežto přeložené `$ROOTFS/..`
uteče do `…/distro`. Řeší to lexikální clamp v překladači, levná řetězcová operace.

**4.3 rootfd místo prefixu.** Držet `O_PATH` fd na `$ROOTFS` a volat
`*at(rootfd, "usr/lib/…")` místo skládání
`/data/user/0/com.linux_core/files/nh/distro/parrot/usr/lib/…`. Jádro tak u každého
syscallu přeskočí 8 komponent (každá znamená dcache lookup, DAC a SELinux kontrolu).
Je to mikrooptimalizace, ale systémová — import v pythonu dělá tisíce statů.

**4.4 Novější jádra.** `openat2(…, RESOLVE_IN_ROOT)` (5.6+) dělá chroot resolution
přímo v jádře (absolutní symlinky i `..` vůči dirfd). Na telefonech s GKI 5.10+:
otevřít přes `openat2`, dál pracovat přes `*at(fd, "", AT_EMPTY_PATH)`. Na tomhle
zařízení ne (4.14; loader dnes pro syscall 437 vrací `ENOSYS`).

---

## 5. Rychlost exec (48 ms → cíl ~15 ms)

Na exec-heavy zátěži je tohle hlavní číslo. `./configure` spustí tisíce procesů;
2000 × 38 ms = **76 s navíc** proti nativnímu běhu.

**Nejdřív změřit.** Rozdíl 34 ms mezi „loader přímo" (14 ms) a „přes shim" (48 ms)
zatím vysvětlený není. Oba běhy dělají stejné relokace stejné binárky, takže
hypotéza z postup.md (cache symbolů) by srazila obě čísla, ne jen jedno. Kandidáti:
práce na straně rodiče (`shim_child_envp`, čtení whitelistu, resolve symlinků), jiné
env v potomkovi (helper knihovny, `ELF_LOADER_*`), re-exec přes `/proc/self/exe`,
stacking seccomp filtrů. Rozhodnou to fázové časy (`clock_gettime` raw syscallem do
logu).

| # | nápad | odhad zisku | náročnost |
|---|---|---|---|
| 5.1 | fázový profil exec | žádný přímo, ale rozhodne o zbytku | hodiny |
| 5.2 | statický bionic loader (`-static`): odpadne linker64 a relokace loaderu samotného | jednotky ms | nízká |
| 5.3 | cache scanu `svc` míst a výsledků hook lookupů podle BuildID | jednotky ms | nízká |
| 5.4 | cache klasifikace ELF (`is_glibc_elf`, shebang, řetěz symlinků) ve sdílené paměti podle `(dev, ino, mtime)` | malý, ale ve smyčkách se sčítá | nízká |
| 5.5 | **cache připravených obrazů** — viz níže | většina relokací | střední |
| 5.6 | **líné vázání `JUMP_SLOT`** — viz níže | velký u velkých binárek | střední |
| 5.7 | **soft exec** (exec v userspace) — viz níže | exec ≈ fork + mapování exe | vysoká |
| 5.8 | **zygote / fork server** — viz níže | exec ≈ jednotky ms | vysoká |

**5.5 Cache připravených obrazů.** Stejný princip, jakým Android sdílí RELRO pro
WebView. Po relokaci na deterministické adresy uložit RW/RELRO stránky do
`$LX_CACHE/img/<buildids>`; další exec je pak `mmap` s `MAP_PRIVATE|MAP_FIXED`
místo relokace. Klíč: BuildID + mtime + verze loaderu + báze. Cena: méně ASLR (dá se
zmírnit rotací báze s každou generací cache).

**5.6 Líné vázání `JUMP_SLOT`** (jako ld.so): PLT položka se resolvuje až při prvním
volání, takže velké binárky (node, clang) platí jen za to, co opravdu použijí. Pozor
na IFUNC a TLS deskriptory.

**5.7 Soft exec.** Když guest proces spouští jiný glibc program, zůstat po
`fork`/`vfork` ve stejném procesu: zavřít `O_CLOEXEC` fd, resetovat dispozice
signálů, odmapovat starý program (podle `/proc/self/maps`), obnovit pristine RW
snapshot glibc z doby po initu, načíst nový exe, postavit zásobník a skočit. Ušetří
se exec v jádře, start loaderu i relokace glibc. Rizika: stav, který resetuje jen
skutečný `execve` (`comm` přes `PR_SET_NAME` jde, `/proc/self/exe` a `cmdline` přes
shimy, robust futex list, sigaltstack); debuggery navíc exec event neuvidí.

**5.8 Zygote / fork server** (trik, kterým startuje Android sám). Trvalý `lxd` má
glibc a běžné knihovny už načtené a zahákované. Exec je požadavek přes unix socket:
zygota se forkne a dotáhne jen zbytek. Problém je, že volající potřebuje skutečné
dítě (wait, exit status, signály, řídicí tty, job control). Řeší se tenkým proxy
stubem, který přeposílá fd přes `SCM_RIGHTS`, signály a exit kód (jako
nailgun/emacsclient). Nejtěžší je job control (`TIOCSPGRP`, `setpgid` napříč
sessions).

5.5 a 5.7 se nevylučují: soft exec tam, kde exec spouští guest proces, image cache
tam, kde musí proběhnout skutečný exec.

**Sdílená paměť pro všechny guest procesy.** Jeden `memfd` (číslo fd v env)
s tabulkou mountů, cache klasifikace ELF a cache výsledků scanu. Invaliduje ho
generační čítač, který se atomicky zvýší při `rename`/`unlink`/`symlink`/`mount`.
Odpadne tak opakované počítání téhož v každém procesu.

---

## 6. Radikální alternativa: nativní glibc proces

Místo glibc uvnitř bionic procesu spouštět:

```
$ROOTFS/lib/ld-linux-aarch64.so.1 --argv0 <jméno> --library-path <libdiry rootfs> \
    --preload <liblx.so> <program> args…
```

glibc ld.so je ET_DYN bez interpretu a umí se sám zrelokovat, takže ho jádro spustí
přímo (na zařízení ověřeno, postup.md:318). `--argv0` (glibc ≥ 2.33) navíc opraví
otevřený nález s `argv[0]`. Žádný `patchelf` ani přepis `PT_INTERP`, takže to
koexistuje s proot.

`liblx.so` je obyčejná glibc knihovna, která dělá kapitolu 2 uvnitř čistě glibc
procesu:

- L1 přepis `svc` každého nově načteného objektu přes **LD_AUDIT** — `la_objopen`
  dostane notifikaci o každém objektu včetně `dlopen`, `la_symbind64` umí oficiálně
  přesměrovat vazby symbolů (čistší než patchování GOT);
- L2 SIGSYS okno;
- opravy výsledků (`getcwd`, `readlinkat`, `/proc`).

**Výhody:**

- Zmizí celá třída chyb „dvě libc v jednom procesu": přepínání `tpidr_el0`, emutls
  guard, fault handler pod špatným TP (`4602b18`), async signál pod bionic TP
  (`80c3ffd`), IFUNC, statický TLS layout, desync brk, `__libc_stack_end`, init bajt
  pro `strerror`… Většina sekcí 2, 5 a 8 v AGENTS.md existuje jen kvůli
  own-loadingu.
- Exec poběží rychlostí ld.so (GNU hash, líné vázání ze základu). Čekal bych blízko
  nativních 10–15 ms místo 48.
- SIGSYS handler v čistě glibc procesu může normálně používat glibc i TLS.

**Nevýhody:**

- **Vazba na verzi glibc.** `ld-linux-aarch64.so.1` a `libc.so.6` musí být ze
  stejného buildu (sdílejí GLIBC_PRIVATE ABI). Dokud se ld.so bere z rootfs, je to
  v pořádku. Pod W^X (targetSdk ≥ 29) ale nejde `execve` z app data, takže by ld.so
  musel ležet v `nativeLibraryDir` APK — a pak by ho upgrade `libc6` v rootfs
  rozbil. Own-loadingu stačí mít v APK loader a na verzi glibc v rootfs nezávisí.
  To je strategická hodnota současné architektury, kdyby appka někdy mířila na
  Play Store.
- **Bionic bridge** (Vulkan/Adreno přes bionic `dlopen`) v čistě glibc procesu
  nejde, takže pro GPU zátěž by zůstal současný loader.
- `/proc/self/exe` = ld.so, potřebuje shim jako dnes.

**Hybrid:** nativní ld.so mód jako výchozí, own-loading pro GPU a pro W^X. Exec shim
vybírá podle cíle.

---

## 7. Zrychlit samotný proot (dokud ho nejde vyhodit)

- Ověřit, že běží se seccomp akcelerací (`PROOT_NO_SECCOMP` nenastavené). Jinak se
  zastavuje každý syscall.
- Cache kanonikalizace cest (cesta → kanonická host cesta) s invalidací na
  `rename`/`unlink`/`symlink`/`mkdir`/`rmdir`. Zmizí `lstat` po komponentách.
- Vícevláknový tracer: ptrace váže tracee na *vlákno* traceru, takže se tracees dají
  rozdělit mezi vlákna a `make -j8` se přestane serializovat.
- Afinita traceru i tracee na stejný cluster (`sched_setaffinity`).
- `process_vm_readv/writev` místo `PTRACE_PEEKDATA` (proot ho používá, pokud je
  k dispozici) a vynechat exit-stop všude, kde ve výsledku není co opravovat.
- **Hybrid s loaderem — krátkodobě nejspolehlivější cesta k plné korektnosti.** Exec
  shim už ELF klasifikuje (`elf_needs_real_exec`). Místo raw exec by mohl
  statické/Go binárky spustit pod proot a glibc dynamické pod loaderem.

---

## 8. Mimo hru na tomhle zařízení

| mechanismus | proč ne tady | kdy ano |
|---|---|---|
| seccomp `USER_NOTIF` supervisor | jádro ≥ 5.0 (`ADDFD` od 5.9) | GKI 5.10+: supervisor bez ptrace. Pořád ale přepíná kontext a má TOCTOU, takže cestové syscally musí emulovat sám a fd vkládat přes `ADDFD`. |
| `openat2` + `RESOLVE_IN_ROOT` | ≥ 5.6 | chroot resolution v jádře (4.4) |
| Syscall User Dispatch | ≥ 5.11, primárně x86 | – |
| user namespaces → rootless chroot/bind | aplikacím blokované | – |
| FUSE | `/dev/fuse` aplikace nemají | root |
| binfmt_misc | root | s rootem: jádro samo spouští loader pro každý glibc ELF a exec shimy zmizí úplně |
| chroot (`linuxsh`) | root | už existuje, nativní rychlost |
| io_uring | jádro ≥ 5.1, novější Android ho aplikacím blokuje | – |
| AVF/pKVM VM (Linux terminál v Androidu 15/16) | chybí pKVM | Pixel a další zařízení s pKVM: plnohodnotný Debian ve VM |
| přestavět balíky proti bionicu (styl Termux) | jde proti cíli projektu | – |

---

## 9. Benchmark plán (než se cokoli staví)

Tři režimy: proot, dnešní loader a `linuxsh` chroot (strop = nativní rychlost).

| zátěž | co měří |
|---|---|
| 1000× `/bin/true` | čistou cenu exec |
| `./configure` malého projektu | exec + stat (autoconf je ideální mučírna) |
| `make -j8` (cJSON nebo podmnožina coreutils) | paralelismus (single-tracer proot) |
| `find /usr -type f \| wc -l`, `git status` na velkém repu | stat-heavy zátěž |
| `python3 -c 'import numpy, pandas'` | tisíce open/stat |
| `java -version` + `javac Hello` (sdkman), Go binárka (`gh --version`), start node | signal-heavy zátěž |
| `apt-get install --reinstall <malý balík>` | fake root, l2s, dpkg |

Metriky: wall time a u loaderu fázové časy exec. Rozhodovací pravidlo: investovat
tam, kde je (loader − chroot) × četnost největší.

---

## 10. Doporučené pořadí

1. **Měření** (kapitola 9) + fázový profil exec (5.1). Bez toho jsou čísla
   v kapitolách 1 a 5 jen odhady.
2. **Rychlé výhry exec:** 5.2, 5.3, 5.4. Cíl: 48 → ~20 ms.
3. **Absolutní → relativní symlinky + clamp `..`** (4.1, 4.2): levné a odstraní
   celou třídu chyb.
4. **Překlad na úrovni syscallů:** nejdřív L1 (2.1) a postupně jím nahrazovat jmenné
   hooky, potom L2 (2.2) pro statické binárky, Go a Bun. **Tímhle loader funkčně
   dožene proot.**
5. **Funkce proot:** tabulka mountů + shim `mount --bind` (3.1), syntéza `/proc`
   (3.4), fake-root DB + l2s (3.2, 3.3), tedy apt/dpkg bez proot. Potom drop-in CLI
   (3.6).
6. **Velký exec projekt:** image cache (5.5), soft exec (5.7) nebo zygote (5.8),
   podle výsledku měření.
7. **Strategické rozhodnutí:** own-loading (nezávislý na verzi glibc v rootfs, GPU
   bridge) vs. nativní ld.so mód (kapitola 6: jednodušší a rychlejší) — nebo hybrid.

---

## Naměřeno (2026-10-16, prompt 01 — část 1 a 3)

Fázový profil loaderu (`ELF_LOADER_TIMING=1`) je hotový a přesný; srovnávací
tabulka zátěž × režim se plní přes `tools/bench-nextgen.sh` nativně (viz níže
„stav měření"). Čísla odhadů v kapitolách 1 a 5 nahrazuje reálný profil.

### Fázový profil exec (loader, `--ownall $ROOTFS/bin/true`)

Raw `CLOCK_MONOTONIC` uvnitř procesu (ne wall-clock; session agenta běží pod
proot, absolutní čísla by byla zkreslená). Sloupec = delta fáze.

| fáze | ms | co to je |
|---|---|---|
| main_start … seccomp_compat | 1,7 | start + compat seccomp filtr |
| overrides_reg | 3,8 | `elf_register_override` × N (dl*, pthread, mmap, …) |
| **load_done** | **19,6** | `elf_load`: čtení + mmap exe a všech deps |
| hooks_got | 2,6 | GOT override tabulka |
| hooks_nocancel/open/spawn | 3,5 | inline hooky `__open64_nocancel`, open64, posix_spawn |
| **hooks_bti** | **14,8** | inline hooky BTI: fstatat/unlink/mkdir/rmdir/link |
| reloc_done | 2,3 | `elf_relocate` |
| **svc_patch** | **8,4** | `elf_patch_syscall_sites` × 5 syscallů × moduly |
| tls_done + run + entry | 1,3 | TLS setup, guest stack, skok |
| **celkem** | **~61** | práce loaderu (bez wall-clock režie exec/CI) |

**Tři dominantní fáze (~43 ms, 70 %):** `hooks_bti` (14,8), `load_done`
(19,6), `svc_patch` (8,4).

### Důsledek pro kapitolu 5 (kde je rezerva)

Hypotéza z postup.md:3917 („34 ms jde na opakované resolvování symbolů, námět
cache symbolů") **není podle profilu hlavní**. Samotná relokace je 2,3 ms.
Čas jde na:

- **mapování a čtení binárek** (`elf_load`, 19,6 ms) → prompt 10 (image cache).
- **skenování .text kvůli prologům** při inline hookách (`hooks_bti` +
  nocancel/open/spawn = 18,3 ms) → zúžit skenované rozsahy, cachovat per
  (BuildID+mtime), případně hookovat jen symboly, které binárka reálně importuje.
- **přepis `svc #0`** (`svc_patch`, 8,4 ms) → prompt 04/05 to stejně řeší; teď je
  vidět cena.

Rozpad 34 ms (shim vs. direct) čeká na nativní `bench-nextgen.sh` — shim cesta
navíc platí druhý exec (bionic linker64 → loader) a env rekonstrukci
(`env_without_preload`), ale to je wall-clock, ne interní profil.

### Stav měření (poctivě)

- **Fázový profil: HOTOVO** (relativní, raw clock — vypovídá o rozložení práce).
- **Wall-clock tabulka zátěž × režim: NEDOKONČENA.** Session agenta běží uvnitř
  proot guestu parrotu, takže `fork/exec` měření jsou zkreslená; `ashell`
  (nativní kanál) v této session koliduje s fault handlerem loaderu. Spustit
  `tools/bench-nextgen.sh` na zařízení nativně (přes app terminál / bzsh).

### Nástroje

- `ELF_LOADER_TIMING=1` — fázový profil na stderr, default no-op.
- `tools/bench-nextgen.sh [proot|loader|linuxsh]` — zátěžový benchmark, medián.
- `test-all.sh timing` — A/B regrese (TIMING vypnutý = ticho).
