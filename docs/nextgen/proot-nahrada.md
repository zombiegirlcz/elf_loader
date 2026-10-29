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

Raw `CLOCK_MONOTONIC` uvnitř procesu (ne wall-clock; session agenta běží jako
DW guest bash pod loaderem, `LX_DW=1`, absolutní čísla by zahrnovala zanořený
exec shim). Sloupec = delta fáze.

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

### Wall-clock: loader vs. proot vs. nativní (host zsh mimo DW, `lxdw off`)

Měřeno z bionic zsh spuštěného MIMO DW (`LX_DW=0`), izolované smyčky bez
`sh -c` obalu, takže každý `fork/exec` je čistý startovní úkon. Proot běží
s `PROOT_LOADER=$PREFIX/bin/loader`, `PROOT_TMP_DIR`, `TMPDIR` a flagy jako
`boot` (`--kill-on-exit --link2symlink -L --change-id=0:0 -0 -w /`).

**Lehké spuštění (30× `/bin/true`), ms/exec:**

| režim | ms/exec |
|---|---|
| nativní `/system/bin/toybox true` | **11** |
| proot | **28** |
| `$L --ownall $R/usr/bin/true` | **56** |

**Překvapení: u lehkého execu je proot (28 ms) ~2× rychlejší než loader
(56 ms).** Interní práce loaderu je ~61 ms (profil výše) — loader platí
relokace+hooky+svc_patch i pro `/bin/true`, proot jen naklonuje proces a
předá exec. To je přesně cena „vlastního načítání glibc" na každý start.

**Zátěž podle typu (medián 3 běhů, host-level):**

| zátěž | loader | proot | vítěz |
|---|---|---|---|
| `find /usr -type f` (107 784 souborů) | **0,44 s** | 11,0 s | **loader ~25×** |
| `git status` (velký repo) | ~0,20 s | ~0,14 s | proot |
| `python3 -c 'import numpy,pandas'` | ~0,33 s | ~0,31 s | ~remíza |
| `node -e 0` | ~0,50 s | ~0,19 s | proot |

**Interpretace:** proot vyhrává tam, kde je málo cestových syscallů (každý
start/exit, git status s cache), protože neplatí relokační daň. Loader
vyhrává drtivě na cestově těžkých úlohách (`find`), kde prootova ptrace
kanonikalizace každé komponenty cesty stojí 2 přepnutí kontextu — 25×.

To je **klíčové pro strategii**: loader se nevyplatí nasazovat kvůli
jednotlivým `exec` (tam prohrává), ale kvůli dlouhým procesům, které hodně
pracují se soubory (buildy, `find`, package managery). Rezerva pro prompt
02/10 je snížit startovní daň (61 ms → blíž nativním 11 ms), aby se loader
vyplatil i na krátkých úlohách.

### Stav měření (poctivě)

- **Fázový profil: HOTOVO** (relativní, raw clock — rozložení práce).
- **Loader vs. proot vs. nativní: HOTOVO** (tabulky výše, host-level mimo DW).
- **linuxsh chroot: SKIP** (session není root; `chroot` vyžaduje root).
- Naměřené hodnoty jsou z jedné session/zařízení; pro robustnost zopakovat
  vícekrát (BENCH_RUNS=5) a na jiném SoC.

### Nástroje

- `ELF_LOADER_TIMING=1` — fázový profil na stderr, default no-op.
- `tools/bench-nextgen.sh [proot|loader|linuxsh]` — zátěžový benchmark, medián.
- `test-all.sh timing` — A/B regrese (TIMING vypnutý = ticho).

---

## Soft exec / zygote — výsledek (prompt 11, 2026-10-29)

**Doporučení: jít dál s variantou A (soft exec), variantu B (zygota)
zavřít.** Prototyp je hotový za `ELF_LOADER_SOFTEXEC=1`, default vypnuto.
Detaily, pasti a čísla v postup.md 2026-10-29.

### Platil ještě předpoklad promptu?

Ano. Po promptech 02/10 je interní práce loaderu ~7–9 ms (z 61 ms
z promptu 01), ale vlastní own-load pořád stojí **9–12 ms na každý exec**
a na exec-heavy zátěži (400 execů `cat`/`grep`/`sed`/`expr`/`rm`) byl
loader **~3× pomalejší než proot** (12,6 s vs 3,9 s).

### Proč A a ne B

Zygota by měla **stejnou cenu** jako soft exec — její potomek má glibc
načtenou a *pristine* z definice, takže nepotřebuje snapshot — jenže
platí identitou procesu: volající nedostane skutečné dítě (pid, `wait`,
`getppid`, `/proc/<pid>`, job control přes `setpgid` napříč sessions).
Soft exec dá tutéž úsporu a pid zůstane správný. Zygota by dávala smysl
jen tehdy, kdyby se ukázalo, že *pristine snapshot* nejde udržet — ten
ale funguje (4 oblasti / ~85 KB, obnova 0,34 ms).

### Co to umí a co to stojí

| zátěž | loader default | **SOFTEXEC** | proot |
|---|---|---|---|
| guest `sh`: 500× `/bin/true` | 12,3–15,2 s | **4,4–4,6 s** | 2,1–2,2 s |
| `fakeconf`, 400 execů | 12,6–13,3 s | **4,0–4,7 s** | 2,2–3,9 s |
| `find /usr -type f` | 0,44 s | 0,44 s | 11,0 s |

(reprodukce: `tools/bench-softexec.sh`, spouštět z hosta přes `ashell -c`)

**Exec-heavy zátěž 3× rychlejší** (31 → 11 ms na exec) při zachování 25×
náskoku na cestově těžkých úlohách. Proot je na *čistém* execu pořád ~2×
rychlejší.

In-process práce soft execu je **~4,1 ms** (`sx_load` 1,15 / `sx_tls`
1,14 / `sx_reloc` 0,64 / `sx_restore` 0,34 / zbytek pod 0,2 ms) místo
~18 ms kompletního startu loaderu.

### Co je teď úzké hrdlo (pro případné pokračování)

Ne exec, ale **`fork`**: 500× `( : )` bez execu dá loader 2,08 s
(4,2 ms/fork) proti prootovým 0,61 s (1,2 ms). Tlustý adresní prostor
(1 GB rezerva privátní arény, 64 MB guest stack, moduly) dělá kopii page
tables drahou. Druhá páka je recyklace TLS regionu a guest stacku místo
nového `mmap`+`memset` (~1,3 ms z těch 4,1 ms).

### Jak moc se tomu dá věřit

Celá sada `test-all.sh all` běží pod `ELF_LOADER_SOFTEXEC=1` (tedy i všechny
vnořené execy harnessu samotného) se stejným výsledkem jako default:
**212/2/33** proti **213/1/33**; oba FAILy jsou známé/flaky (`nslookup
localhost` → NXDOMAIN, a prahový `symhash`, který samostatně projde v obou
režimech). Kategorie `test-all.sh softexec` má 8 podtestů, 8/0.

### Známé nekompatibility

Není to `execve`: ladicí nástroje nevidí exec event, setuid/setgid se
neaplikuje, starý obraz exe / stack / TLS / mmapy zůstanou namapované
(RSS ~jednotky MB na soft exec), moduly `dlopen`-nuté předchozím
programem zůstanou ve scope s neobnoveným RW stavem, POSIX timery se
neresetují. Soft exec se nepoužije pro binárky s `PT_TLS` (blok hlavního
exe musí ležet na `TP+0x10`, ten drží předchozí exe), pro skripty,
pro vícevláknový proces a pod L2. `vfork` se pod knobem přepisuje na
`fork` (jinak by soft exec přepsal glibc rodiče); `clone(CLONE_VM)`
bez `CLONE_THREAD` zůstává nepokrytý.

---

## Nativní ld.so — výsledek (prompt 12, 2026-10-29)

**Doporučení: hybrid, s nativním módem jako cílem pro CLI zátěž.** Nativní
ld.so je na tomhle zařízení **4–10× rychlejší na exec** a **21× lehčí na RSS**
než own-loading, funguje na glibc 2.41 i 2.43 bez per-verzních konstant a
hákovací bod pro překlad cest v něm existuje. Co zbývá, není otázka
„jde to vůbec", ale dotažení překladové vrstvy — a ta se dá postavit ze
stejných dílů jako L1/L2.

### Vstupní kontrola

| bod | výsledek |
|---|---|
| 1. W^X / execmem | doména je `untrusted_app_27` (targetSdk ≤ 28) → `execute_no_trans` na `app_data_file` ještě není odebraný, `ld.so` z app dat jde spustit. **Obava z kapitoly 6 na tomhle zařízení neplatí**, pro targetSdk ≥ 29 ale platí dál. |
| 2. spustitelnost ld.so | `ld.so --version` → rc=0, bez našeho kódu. |
| 3. verze glibc | parrot 2.41, kali 2.43 — chovají se **identicky** ve všech měřeních níže. |
| 4. `ld.so --library-path … <prog>` | **selhalo**: SIGSYS (rc=159) na obou rootfs. Prompt tady měl skončit — neskončil, protože příčina je jen dva syscally, viz dál. |

### Proč nativní ld.so umíral

Postup izolace (každý krok měřen, ne odvozen):

1. Umírá i program s **nulovou** závislostí na libc (`-nostdlib -fPIE -pie`,
   jen `svc` na `exit`). Vinník je tedy v `ld.so` samotném, ne v libc ani
   v programu.
2. `LD_DEBUG=all` končí **po version checkech a před relokací** — to je
   přesně místo, kde `dl_main` volá `init_tls()` → `TLS_INIT_TP` →
   `__tls_init_tp`.
3. `GLIBC_TUNABLES=glibc.pthread.rseq=0` (tunable existuje, `--list-tunables`
   ho ukazuje) **nepomohlo** → není to `rseq`.
4. Freestanding ptracer (`tools/lxnative.c`) přečetl z `PTRACE_GETSIGINFO`:
   **`si_syscall=99` = `set_robust_list`**, `si_code=1` (`SYS_SECCOMP`),
   `si_arch=0xc00000b7` (`AUDIT_ARCH_AARCH64`). Shodně parrot i kali.
5. Po pohlcení SIGSYS proces **doběhne**. Druhý a poslední trap je
   `293` = `rseq`. Dohromady **dva traps na proces** (a dva na každé další
   vlákno, protože `__tls_init_tp` běží per-thread).

`set_robust_list` volá glibc v `__tls_init_tp` nepodmíněně a nemá na to
tunable. Návratovou hodnotu přitom **toleruje** (`__set_robust_list_avail`
zůstane `false`) — jádro před SIGSYS nastaví `x0 = -ENOSYS`, takže pohlcení
signálu je pro glibc korektní odpověď, ne hack.

### Korekce: app profil TRAPuje, nekilluje

Freestanding sonda v čistém `ashell` kontextu (rozsah 0–600, raw `svc`,
vlastní `RET_ERRNO|66` filtr na každé zkoumané číslo): **0 KILL, 362 TRAP**.
Profil je whitelist s `SECCOMP_RET_TRAP` jako defaultem — proto TRAPuje
i všechna neexistující čísla nad 470.

Komentáře v `src/elf_loader.c:2177` a `:4141` říkají „app seccomp KILLuje
rseq(293), set_robust_list(99) a clone3(435)" a odvozují z toho, že „KILL
nejde přebít filtrem ani SIGSYS handlerem". **První část je chybná**, druhá
proto taky. Nevyplývá z toho ale, že by byl `elf_patch_syscall_sites`
zbytečný: `RET_ERRNO` filtr TRAP nepřebije (jádro bere z filtrů **nejnižší**
akci, `RET_TRAP` 0x00030000 < `RET_ERRNO` 0x00050000) a handler by u loaderu
běžel pod cizím TP (past č. 1). Přepis instrukce zůstává nejlevnější cesta,
jen z jiného důvodu, než je u ní napsaný. Pro **nativní** mód je ale rozdíl
zásadní: TRAP se pohltit dá, KILL by prototyp zabil v zárodku.

Proč se to nezjistilo dřív: glibc sonda (`src/sigsys_probe.c`) dá v obou
dostupných prostředích nesmysl — pod loaderem je `syscall()` hookovaný
a `svc` místa přepsaná, pod prootem se syscall interponuje. Nahlásila
**0 KILL a 0 TRAP**. Past č. 17 v `AGENTS.md`.

### Čím SIGSYS pohltit

Zevnitř procesu to nejde, a to je důležitější než vypadá:

- **vlastní seccomp filtr** — `RET_ERRNO` prohraje s `RET_TRAP` (viz výše);
- **SIGSYS handler** — dispozice signálů se při `execve` resetují, a
  `--audit` ani `LD_PRELOAD` se nevolají dřív než `__tls_init_tp`
  (změřeno: bez pohlcení se `la_version` **vůbec nespustí**);
- **`SIG_IGN`** — exec by přežilo, ale seccomp posílá SIGSYS přes
  `force_sig_info`, který ignorování přebije.

Zbývá tracer. Narozdíl od prootu to **není překlad syscallů**: zastavujeme
se jen na SIGSYS, tedy 2× za proces, ne u každého syscallu. Proto je ta
režie v šumu (tabulka níže). ptrace navíc přežívá `execve`, takže potomci
spuštění guestem absorbér neztratí; `PTRACE_O_TRACEFORK|TRACEVFORK|TRACECLONE`
pokryje i vlákna.

**Cena té volby:** jeden tracer na procesový strom a guest nejde současně
ptracovat ničím jiným (gdb, proot). Pro CLI zátěž to nevadí, pro ladění ano.
Alternativa bez traceru: privátní **kopie** `ld.so` mimo rootfs se dvěma
NOPnutými `svc` místy (`elf_patch_syscall_sites` to umí vzorem, bez
per-verzních konstant) — rootfs zůstane nedotčený, takže to neporušuje
zákaz úprav binárek v rootfs. Per-thread `set_robust_list` z `libc.so.6`
by pak musela řešit audit knihovna přepisem textu libc, což už je L1.
Neměřeno.

### Co se postavilo

| soubor | co to je |
|---|---|
| `tools/lxnative.c` | freestanding statická ET_EXEC: ptrace supervizor, pohlcuje SIGSYS z allowlistu (99, 293, 435, 436, 437, 439), sleduje potomky i vlákna, propaguje exit kód |
| `tools/liblx-audit.c` | rtld-audit knihovna: `la_symbind64` přesměruje PLT vazby cestových funkcí na vlastní wrappery, + exec shim, + `/proc/self/exe` |
| `tools/lxnative-run.sh` | přepínač („mód"): nastaví guest `PATH`, `LX_ROOTFS`, `LX_LDSO`, `LX_LIBPATH`, `LX_AUDIT`, `LX_PROG` |
| `tools/lxnative-ownall.sh` | shim, který se tváří jako `elf_loader --ownall` — kvůli harnessu |
| `tools/lxnative-build.sh` | build obojího **rootfs gcc**, žádný NDK ani Modal |
| `tools/bench-native-ldso.sh`, `tools/bench-native-python.sh` | A/B měření exec a RSS |
| `tools/compare-native-mode.sh` | proběhne `test-all.sh` v obou módech a sečte to |
| `test-all.sh` | `LX_NATIVE_MODE=1` přeskočí `check_interpreter` a vezme shim; default nedotčený |

Podstatné: **oba kusy staví rootfs gcc**, takže se nativní mód dá iterovat
bez buildu loaderu přes Modal/GitHub Actions. `lxnative` je statická ET_EXEC
bez interpretu, takže běží v hostovém bionic kontextu.

### Čísla

Stroj je hlučný (`toybox true` vyšel v jednom běhu 11,9 ms a ve druhém
25,9 ms), proto jsou uvedené **rozsahy ze dvou pořadí** a rozhodují poměry,
ne absolutní hodnoty. Metodika podle `AGENTS.md`: A/B přepínačem na téže
binárce, obě pořadí, `ashell -c`.

`/bin/true`, N=40, dva běhy:

| mód | ms/exec |
|---|---|
| nativní ld.so | **3,5–7,0** |
| nativní ld.so + audit | 3,6–4,6 |
| proot | 14,7–25,9 |
| loader `--ownall` | 25,2–41,5 |
| `/system/bin/toybox true` (bionic, jiná binárka) | 11,9–25,9 |

`python3 -c 0`, N=5, dvě kola:

| mód | ms/start |
|---|---|
| nativní ld.so | **35,7 / 54,1** |
| nativní ld.so + audit | 36,9 / 48,0 |
| proot | 28,0 / 39,4 |
| loader `--ownall` | 98,4 / 93,4 |

Referenční číslo z dřívějšího měření (102,8 ms pod loaderem) se potvrdilo.
**Cena rtld-auditu je v šumu** i na pythonu, který váže stovky symbolů —
`la_symbind64` je jen `strcmp` smyčka a `_dl_profile_fixup` se proti startu
interpretu neprojeví. To byl největší otevřený risk a nevyšel.

RSS, `cat /proc/self/status` (tatáž binárka, tatáž práce):

| mód | VmHWM | VmSize |
|---|---|---|
| nativní ld.so | **252 kB** | 2 652 kB |
| loader `--ownall` | 5 496 kB | 3 271 484 kB |

`python3`, `VmHWM`:

| mód | VmHWM | VmSize |
|---|---|---|
| nativní ld.so | **8 216 kB** | 13 740 kB |
| nativní + audit | 8 880 kB | 15 700 kB |
| loader `--ownall` | 13 036 kB | 3 310 360 kB |

Tedy **21,8× nižší RSS** na triviálním procesu a **1,6× nižší** na pythonu;
`VmSize` 1 234× resp. 241× nižší (1 GB aréna + 64 MB stack own-loadu).
Hlavní argument kapitoly 6 — sdílení file-backed stránek — platí.

### `test-all.sh` v nativním módu

Harness zůstává pod loaderem a přepíná se jen `L`, tedy to, čím se spouští
každý jednotlivý test (`LX_NATIVE_MODE=1`). Vnořený tracer by kolidoval
s vnějším: jeden tracee má jen jednoho tracera a `PTRACE_O_TRACEFORK`
vnějšího by vnitřnímu `lxnative` sebral dítě, takže by jeho `PTRACE_TRACEME`
selhal na `EPERM`.

**Pustila se nedestruktivní podmnožina, ne `all`.** Důvod je bezpečnostní:
překlad cest je v PoC částečný, a kategorie, které mažou (`l2s`,
`l2s_rename`, `binds`, `fakeroot`, `imgcache`), by `rm -rf` mířily na
nepřeložené, tedy **hostové** cesty. Zároveň nemá smysl v nativním módu
měřit kategorie, které testují **mechanismy loaderu** (`l1`, `l2`,
`softexec`, `softexec_cap`, `execcache`, `symhash`, `lazybind`, `relsym`,
`procvirt`, `imgcache`) — ty v nativním módu neexistují. To je samo o sobě
nález: velká část sady testuje loader, ne guest funkcionalitu, takže
„`test-all.sh all` v nativním módu" je jen částečně smysluplná metrika.

Podmnožina 20 kategorií (`basic text files system datetime compression math
diff archive shell python pathops fstat symlink selfexe argv0 extended extra
networking nss`):

| mód | PASS | FAIL | SKIP |
|---|---|---|---|
| own-load (default) | 158 | 2 | 32 |
| nativní, PoC v1 (15 jmen v tabulce) | 131 | 29 | 32 |
| nativní, PoC v2 (36 jmen) | 141 | 19 | 32 |
| nativní, PoC v3 (+ uid/gid v allowlistu traceru) | **142** | **18** | 32 |

Trend 29 → 19 → 18 je podstatnější než absolutní číslo: první dvě iterace
byly **jen doplnění jmen a čísel do tabulek**, žádná změna architektury.
Zbylých 18 už z větší části architektura je — viz taxonomie.

Oba FAILy v defaultu jsou známé a nesouvisející (`nslookup localhost`
→ NXDOMAIN, flaky `csplit`).

### Taxonomie selhání — co musí přijít z L1/L2

Tohle je vlastní odpověď promptu. FAILy v nativním módu nejsou náhodné,
spadají do čtyř tříd a jen jedna z nich je strukturální:

1. **Chybějící jména v tabulce** (ne strukturální, jen práce). PoC v1 neměl
   `mkdtemp`/`mkstemp`, takže harness si vytvořil `/tmp/pathops.NNN`
   v **hostovém** `/tmp`, které na Androidu neexistuje → celá kategorie
   `pathops` 4/4 FAIL. Doplnění `fstatat`/`faccessat` zvedlo
   `find /etc | wc -l` z **1 na 229**.
2. **Interní volání v libc** (strukturální → **L1**). `bl` uvnitř `libc.so.6`
   neprochází PLT, takže ho `la_symbind` nevidí — tatáž mez jako u jmenných
   hooků (past č. 2). Projevuje se u `make` (`/usr/bin/echo: No such file`),
   u NSS (`getprotobyname` → dlopen modulu zevnitř libc) a u všeho, co
   používá `fts`/`glob`/`scandir`. Řeší se přepisem `svc` v textu libc, což
   audit knihovna umí udělat v `la_objopen` — `la_objopen` **vidí i `ld.so`
   a `libc.so.6`** (změřeno), takže hákovací bod existuje.
3. **Raw syscally** (strukturální → **L2**). Go, Bun, statické binárky.
   V nativním módu je okno pro `l2_install_range_filter` jednodušší než pod
   own-loadem: rozsahy guest modulů jsou všechno, co v procesu je.
4. **`open` programu, který dělá `ld.so` sám** (strukturální, ale už
   rozpracované jinde). `/usr/bin/awk` a `/usr/bin/which` v rootfs jsou
   symlinky na **absolutní** cíl (`/etc/alternatives/…`); ten jádro
   vyhodnotí proti HOSTOVÉMU korenu ještě předtím, než se audit knihovna
   načte → `error while loading shared libraries: …/awk: cannot open shared
   object file`. Překladem to opravit nelze, protože se to děje před námi.
   **Nativní mód tím dává konkrétní cenovku už otevřené položce
   „celorootfs konverze absolutních symlinků" (`tools/relsymlinks.sh`,
   1 093 linků, 1 081 převoditelných).** Past č. 7 platí v nativním módu
   dvojnásob.

### Strategické body kapitoly 6 — revize po měření

- **W^X / targetSdk.** Na tomhle zařízení neaktuální (`untrusted_app_27`).
  Obava ale zůstává platná pro cílení na targetSdk ≥ 29, a tam by ld.so
  musel ležet v `nativeLibraryDir` — a pak ho upgrade `libc6` v rootfs
  rozbije (GLIBC_PRIVATE). **Dokud se ld.so bere z rootfs, je konzistence
  zaručená zdarma.** Tohle je jediný argument, který own-loadingu zůstává
  bez výhrad.
- **Bionic bridge (GPU).** V čistě glibc procesu nejde. Pro Vulkan/Adreno
  zátěž zůstává own-loading, bez alternativy.
- **RAM.** Změřeno, viz tabulky — nativní vyhrává výrazně.
- **`/proc/self/exe`.** Shim je potřeba a je hotový (`LX_PROG`). Vedlejší
  zjištění: **bez** shimu to nahodou funguje pro python, který si z cesty
  k ld.so (ležícího v rootfs) správně odvodí `sys.prefix` — náhoda, na
  kterou se nedá spoléhat.
- **Dvě libc.** V nativním módu neplatí past č. 1: wrappery smí volat
  cokoli z libc včetně `snprintf`, žádné `dl_enter_host`. To je ta úspora,
  o které kapitola 6 mluvila, a je reálná.

### Rozhodovací pravidlo pro exec shim (hybrid)

Doporučení je hybrid, takže je potřeba pravidlo. Podle **cíle execu**:

| cíl | mód | proč |
|---|---|---|
| glibc ELF s `PT_INTERP` obsahujícím `ld-linux` | **nativní** | rychlost + RSS |
| bionic ELF (`PT_INTERP` = `/system/bin/linker64`) | raw exec | pod glibc ld.so nepojede |
| statická ELF / Go / Bun | own-load s `L2=1` | nativní mód je nevidí, dokud není L2 |
| program, který potřebuje bionic bridge (GPU) | own-load | bridge v nativním módu neexistuje |
| skript (`#!`) | podle interpretu po rozkladu shebangu | — |

**Když se spleteš:** nejhorší případ není pád, ale **tiché spuštění hostové
binárky**. Změřeno: `sh -c "/bin/ls --version"` bez exec shimu vrátí
`toybox 0.8.6-android`, protože Android má `/bin` → `/system/bin`. Žádná
chyba, jiné chování, jiné `/etc`. Proto musí být rozhodnutí postavené na
**obsahu `PT_INTERP`**, ne na jeho přítomnosti (bionic binárky ho mají taky),
a guest `PATH` nesmí obsahovat `/system/bin` před rootfs. Past č. 21.

### Co zbývá, kdyby se do toho šlo

V tomto pořadí, protože každý krok je měřitelný samostatně:

1. Konverze absolutních symlinků v rootfs (`tools/relsymlinks.sh`) — bez ní
   třída 4 nikdy nezmizí a je to **nutná podmínka**, ne optimalizace.
2. L1 v `la_objopen` nad `libc.so.6` — zruší třídu 2, a tím většinu FAILů.
3. L2 okno pro statické/Go — zruší třídu 3.
4. Teprve pak má smysl pustit `test-all.sh all` a porovnávat s 216/2/33.
5. Bionic bridge a targetSdk ≥ 29 nechat own-loadingu; hybrid podle tabulky
   výše.

**Stop kritérium, které se naplnilo:** ani jedno z promptových („`--audit`
se nevolá", „W^X nejde") — `--audit` se volá a `la_objopen` vidí i `ld.so`
a `libc.so.6`. Prototyp se nezastavil, zastavil se rozsah: dotáhnout L1/L2
do audit knihovny je samostatná práce na dny, ne zbytek tohoto promptu.
