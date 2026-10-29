# AGENTS.md — elf_loader

Krátký provozní manuál: co to je, jak se to buildí/deployuje, jaká pravidla
platí a co je otevřené. **Historie, root cause, diffy a naměřená čísla jsou
v [postup.md](postup.md)** (chronologický deník; odkazy `[#L…]` = řádek tam).

## Cíl

Vlastní načítání glibc (`libc.so.6` + deps) do privátního scope a běh glibc
binárek v procesu na AArch64 Androidu (bionic host). Preference non-root
(`--ownall`); chroot (`linuxsh`) je rychlá alternativa jen s rootem.
Nahrazuje `proot` — žádný ptrace překlad syscallů, takže uživatel může
pouštět distro nástroje přímo v appce bez NDK rebuildu.

## Bezpečnost — destruktivní testy (POVINNÉ)

**Vývojové a testovací prostředí koexistují — NENÍ to kopie rootfs.** Živý
parrot (`…/nh/distro/parrot`) je zároveň prostředí, ve kterém agent běží.
Cokoli s možným destruktivním dopadem (`--ownall`/`--shim` nad adresářem,
`ELF_LOADER_IMGCACHE`, přepis binárek, in-place shim operace) se proto **NIKDY
nemíří na živý parrot** (`/bin`, `/usr/bin`, `/usr/lib`, `/etc`).

Na destruktivní testy je vedle parrotu **nepoužívaný kali rootfs** — používej ho:

```
K=/data/user/0/com.linux_core/files/nh/distro/kali
ROOTFS=$K elf_loader --ownall $K/bin/<elf> [args…]
```

Kali („Kali GNU/Linux Rolling", `…/nh/distro/kali`) nikdo nepoužívá, takže jeho
případné poškození nic nezlomí; parrot ber v testech jen read-only.

**Proč:** `elf_loader --ownall /bin` + `--ownall /usr/bin` nad živým parrotem
(s `IMGCACHE`) poškodilo `bin`/`lib`/`etc` → nutná reinstalace (2026-10-21).
dpkg DB přežila, obnova šla z `dpkg --get-selections`. Viz i bezpečnostní
pravidla v kořenovém `CLAUDE.md` (bootloop 2026-08-23).

**Bindy (prompt 06) jsou stejně nebezpečné:** `rm -rf <mountpoint>` pod
loaderem maže **zdroj** bindu (jako v Linuxu). Pravděpodobná skutečná příčina
incidentu: starý `category_binds` nechal bind `/tmp/lxbt → /etc` v tabulce
harnessu a úklid `rm -rf /tmp/lxbt` pod loaderem smazal `$R/etc` (postup.md
2026-10-22). V testech: zdroj bindu **jen jednorázový adresář**, pod-testy
s `LX_BINDS_FILE=` (vlastní tabulka), úklid **hostovými** nástroji
(`/system/bin/*`) na host cestách.

## Build a prostředí

gcc 14.2 v prootu, `as` symlink fix [#L8]. Testy: `make test`, `--ownall`
[#L15]. Ověřené runtime hodnoty (GOT/TLS/arena) [#L76].
Nasazený loader je NDK clang build **s `-O0`** (Makefile i `build.yml`) —
bajtové smyčky v shimech nejsou zadarmo. Host unit testy (bez zařízení):
`test/{cache,relsym,l1,l1_common,l1_logic}_test.c`, build v hlavičce souboru.

## Výkon — co má a nemá smysl řešit

Měřeno host-level (mimo DW, `LX_DW=0`): nativ 11 / proot 28 / loader ~26 ms
na `/bin/true`; `find /usr` ~25× rychlejší než proot. Loader se vyplatí na
dlouhé procesy, ne na krátký exec.

Hotové optimalizace (detaily [#L92] a postup.md 2026-10-20): cache svc-site
skenu (`cache_nr_t.scanned`, i negativní výsledek) a DT_GNU_HASH lookup
(`elf_scope_find` už není lineární `strcmp`). Dohromady `python3 -c 0`
208 → 102,8 ms, 1000× `/bin/true` 47,4 → 25,8 ms.

**Relokace je po hashi jen 2–12 % exec** → cache RELRO obrazů i lazy binding
jsou hotové, ale defaultně vypnuté a měřením nepotvrzené. Neotvírat znovu
bez nového měření.

**Soft exec** (prompt 11, `src/softexec.h`, `ELF_LOADER_SOFTEXEC=1`, default
vyp): `shim_execve` místo re-execu loaderu vymění guest program **uvnitř
procesu** — obnoví pristine snapshot RW stavu guest glibc (pořízený
v `run_ownall` před relokací exe), dodělá co jinak dělá jádro při `execve`
(O_CLOEXEC fd, dispozice signálů, sigaltstack, timery) a načte nový exe.
Exec-heavy zátěž **3× rychlejší** (31 → 11 ms/exec), in-process práce
~4,1 ms místo ~18 ms startu loaderu. Jen PIE ELF s `PT_INTERP` a **bez
`PT_TLS`**, jednovláknový proces, ne skript, ne pod L2; `vfork` se pod
knobem přepisuje na `fork`. **Zbývající hrdlo je `fork`** (4,2 ms/fork
u loaderu vs 1,2 ms u prootu — 1 GB aréna + 64 MB stack), ne exec.
A/B měření: `tools/bench-softexec.sh` (z hosta, `ashell -c`).

**Nativní ld.so mód** (prompt 12, **prototyp, není to mód loaderu**): guest
glibc program běží pod `ld.so` z rootfs, ne own-loadem. `tools/lxnative.c`
(freestanding statický ptrace supervizor, pohlcuje SIGSYS — app profil
TRAPuje `set_robust_list`/`rseq` v glibc TLS initu) + `tools/liblx-audit.c`
(rtld-audit, `la_symbind64` přesměruje PLT vazby cestových funkcí + exec
shim). Přepínač `tools/lxnative-run.sh`, v harnessu `LX_NATIVE_MODE=1`.
**Oba kusy staví rootfs gcc** (`tools/lxnative-build.sh`), žádný NDK ani
Modal. Naměřeno: exec 3,5–7,0 ms proti 25–41 ms u `--ownall`, RSS 252 kB
proti 5 496 kB; `python3 -c 0` 36–54 ms proti 93–98 ms. Default nedotčený,
žádný dopad na loader. Čísla, taxonomie selhání a doporučení (hybrid)
v `docs/nextgen/proot-nahrada.md`, kapitola „Nativní ld.so — výsledek".

Knoby: `ELF_LOADER_TIMING`, `ELF_LOADER_NO_CACHE`, `ELF_LOADER_NO_HASH`,
`ELF_LOADER_HASH_CHECK`, `ELF_LOADER_LAZY`, `ELF_LOADER_IMGCACHE`,
`ELF_LOADER_NO_RELSYM`, `ELF_LOADER_NO_BIONIC_BRIDGE`, `ELF_LOADER_EXEC_TRACE`,
`ELF_LOADER_L1` (`=1`/`=2`), `ELF_LOADER_L1_SCAN`, `F2_SKIP=a,b` (denylist
jmenných hooků), `F2_ONLY` (allowlist), `ELF_LOADER_L2` (`=1` záchytná síť),
`ELF_LOADER_L2_LAZY` (líný přepis, default vyp), `ELF_LOADER_GOMODE=0`
(vypne Go mód; L2 ho nahrazuje), `ELF_LOADER_NO_PROCVIRT` (prompt 07),
`ELF_LOADER_DIAG=1` (rutinní stopy do `$D/usr/diag.<pid>.txt`; pádové vždy),
`ELF_LOADER_NO_CWD_FIX=1` (vypne přepočet cwd do ROOTFS, viz past č. 22),
`ELF_LOADER_NO_GUEST_PATH=1` (nenastavovat guest `PATH`, past č. 24),
`ELF_LOADER_NO_STATIC_PIE_PROOT=1` (neposílat static-PIE na reálný proot).
`ELF_LOADER_SOFTEXEC=1` (prompt 11, soft exec; `ELF_LOADER_SOFTEXEC_TRACE`
= stderr, nebo absolutní cesta = i do souboru).
`LX_FAKEROOT=1` (prompt 08, `src/fakeroot.h`, ekvivalent `proot -0`):
uid/gid funkce → 0, `stat` rodina hlásí app uid jako 0:0, `chown`/setuid
`chmod`/`mknod` zařízení se ukládají do xattru `user.lxfakeroot` na inode
(f2fs `/data` user.* povoluje; xattr chce právo zápisu/čtení → při EACCES
dočasné u+w/u+r). `LX_L2S=1` (prompt 08, `src/l2s.h`, ekvivalent `proot -l`):
`link`/`linkat` → symlinky do `$ROOTFS/.l2s`, formát přesně jako proot
(`.l2s.<jméno><NNNN>` + `.<nlink>`), rootfs funguje pod oběma. Oba default
vypnuté; apt/dpkg potřebuje **oba** (dpkg zálohuje hardlinkem).
Bindy: `LX_BINDS=host:guest[:ro],…`,
`$ROOTFS/etc/lx-binds` (fstab), `LX_BINDS_FILE` (sdílená tabulka, vnucuje se
potomkům; prázdné = nová session).
`LX_L2S=1` (default vypnuto, prompt 08 část 2 — link2symlink/`src/l2s.h`;
formát PŘESNĚ podle proot (`.l2s.<basename><NNNN>` + `.<MMMM>` = aktuální
nlink, `$ROOTFS/.l2s`, hostově absolutní cíle — kompatibilní i se soubory,
které v rootfs vytvořil skutečný proot). `ln`/`ls` fungují (coreutils volají
jen `linkat`/`statx`, ne `link`/`lstat` — doplněno do existujícího
`hook_inline_prologue` fallbacku pro BTI prolog vedle `fstatat`/`unlink`/
`mkdir`/`rmdir`/`link`). `test-all.sh l2s` (destruktivní, jen kali) PASS.
~~**Otevřené:** obecný bug — cesty se v child-exec-pod-`--ownall`
scénářích nepřekládají pod ROOTFS (vzácné/časové)~~ — **vyřešeno**
2026-10-29 (5): nebylo to časování, ale **nepřeložený cwd** (past č. 22).
`run_ownall` teď cwd přepočítá do ROOTFS. `LX_L2S` zůstává default vypnuté,
ale už **ne kvůli tomuhle** — chybí jen rozhodnutí o defaultu; přes
proot-kompatibilní CLI (`-l`) se zapíná samo.

**Metodika:** pinování CPU mění čísla o desítky % → měřit při stejné CPU
politice a A/B dělat knobem na téže binárce, ne dvěma buildy.

## Git / GitHub

Ruleset `protect-master` (zákaz update/force-push/smazání, bypass jen admin),
`protect-dev` (zákaz force-push/smazání). `feedback-automerge.yml`: PR do
`dev` měnící **jen** soubory ve `feedback/` se sám squash-mergne; jinak ruční
review. Před synchronizací `git pull origin dev`, pak `master` ← `dev` (ff).

## Deploy na zařízení

- Magisk modul + `elf` wrapper (bionic NDK build), namespace workaround
  `/data/adb/` [#L328]. `LD_LIBRARY_PATH` poisoning fix — `/system/lib64`
  PRVNÍ [#L406].
- `linuxsh` (chroot + `unshare -m`) [#L558]. **KRITICKÉ:** hned po
  `unshare(CLONE_NEWNS)` → `mount --make-rprivate /` (jinak mounty leaknou
  do global NS) [#L605]. Architektura spouštění [#L598], [#L731].
- `bzsh` — bundled bionic zsh 5.9.2, instaluje se do `/data/adb/` stejně jako
  `linuxsh` (Magisk mount není vidět v app namespace).
- **`mount(2)` je v app doméně ENOSYS** (seccomp) — pod loaderem `mount
  --bind` emuluje tabulka bindů (prompt 06, viz „Překlad cest"); mimo loader
  (proot, host) použít symlink.

## Shelly: `lx`, `init`, double world

- **`init zsh|bash`** — univerzální ROOTFS, žádný hardcode:
  `elf_print_init_env` odvozuje D/ROOTFS/R/L/LX_LOG z `$HOME`. Host
  `.zshrc`/`.bashrc` = `eval "$(… init …)"` [#L2395].
- **`.elfrc`** — jediný uživatelský rc pro host shell (starship/zoxide/aliasy),
  `ELF_RC` override, `LX_NO_RC=1` skip; `init example` / `init install`.
- **`lx` toolkit** (žije jen na zařízení, není v gitu) [#L2425].
  `lx -c '<řádka>'` posílá celou řádku guest shellu → pipy/`&&`/redirecty.
  `lxwhich`/`lx` zkouší první `$VIRTUAL_ENV/bin`.
- **Double world** (`init zsh|bash -dw`, `lxdw on|off|status`): vstup cwd do
  `$ROOTFS` spustí guest shell pod loaderem; `cd ..` z `/` ukončí guest přes
  marker `$LX_LOG/dw/mark.<pid>`. Guest rc = `$LX_LOG/dw/rc`.
- **`lxfb [-m pozn.] <cmd>`** — běh s debug env, `tee` do logu, report do
  `$LX_LOG/feedback/` → PR do `dev`/`feedback/`.

## Seccomp / exec

App seccomp (kernel 4.14): `clone3` SIGSYS, `close_range`… →
`install_legacy_syscall_filter()` (RET_ERRNO ENOSYS pro 435/436/437/439),
fork veneer (vfork-style clone) [#L685].

## Překlad cest — vrstvy

- **L3 jmenné hooky** (`g_f2_hooks`, GOT override + inline/BTI hooky) —
  default, plné pokrytí sémantiky (exec/own-load, symlinky, getcwd, Bun).
- **L1** (prompt 04, `src/l1.h`, `src/l1_emit.h`, `src/l1_common.S`):
  cestová `svc #0` v načtených modulech → `b` do per-site stubu, překlad na
  hranici syscallu. **Default vypnuto.** `ELF_LOADER_L1=1` = L1 vedle hooků,
  `=2` = navíc vypne 52 hooků „jen překlad" (`g_l1_translate_only`).
  Patchuje jen cestová čísla (libc 48 + 10 dynamických `x8` na 17 stránkách,
  ld.so 9), seznam míst v cache (v3, slot `CACHE_NR_L1`). Cena ~1 % na
  `find`, 0,1 % na python importu, RSS +85 KB. Mimo L1: exec, mount,
  bind/connect. `L1=2 all` = 194/0/33 [2026-10-21 (2)].
- **L2** (prompt 05, `src/l2.h`): seccomp `RET_TRAP` záchytná síť pro kód,
  který L1 nepřepíše — statické binárky, Go, Bun (raw syscally), JIT. Cestové
  `svc` z **textu guest exe** TRAPnou, `sigsys_handler`→`go_handle` přeloží.
  Zobecněný Go mód (`l2_install_range_filter` sdílí Go mód i L2, nastavuje
  `g_go_lo/hi`). PIE exe se přesune do pevného okna `0x11_0000_0000`
  (`MAP_FIXED`), aby text-range nekolidoval s ASLR host binárek; ET_EXEC
  (gh/Bun/static) mají pevnou nízkou adresu. `go_handle` + `getcwd`/`readlinkat`
  strip → Bun bez `F2_NO_CWD_STRIP`. **Default vypnuto** (`ELF_LOADER_L2=1`;
  pod ním se místo Go módu instaluje L2). Lazy přepis `svc`→L1 stub
  (`ELF_LOADER_L2_LAZY=1`, default vyp — není ARM-safe souběžně). `L2=1`
  dělá statickou glibc i Go viditelnou do rootfs. Pod `LX_FAKEROOT` navíc
  TRAPuje identitní čísla (174–177, `getres*id`, `get/setgroups`, 143–152)
  a `go_fakeroot_pre/_post` dodají fake identitu i opravu `stat` výsledků —
  bez toho statická/Go binárka fakeroot nevidí (2026-10-29 (8)). `all`
  default 195/0/33 [2026-10-21 (3)].
- **Tabulka mountů** (prompt 06, `src/binds.h`): longest-prefix
  `guest → host [ro]`, default = bývalý `shim_excluded` (identity) + remap
  `/dev/shm`. MAP_SHARED soubor (`$LX_LOG/binds.*.tab`, jinak
  `$ROOTFS/tmp/.lxbinds.*.tab`) sdílený session přes `LX_BINDS_FILE`;
  `shim_mount`/`umount2` = runtime `mount --bind` (cross-process). Jiné
  `LX_BINDS` → soukromá kopie (`unshare -m`), jiný `ROOTFS` → nová tabulka;
  osiřelé soubory maže GC přes `flock`. Ro → `EROFS`, mountpoint
  `rmdir`/`rename` → `EBUSY`, `getcwd` zpětně (`bind_reverse`). L1 i L2 přes
  `shim_bind_deny` (`l1_before` umí syscall odmítnout) a `go_translate` hook.
  util-linux `umount`/`findmnt` fungují přes virtualizovaný mountinfo
  (prompt 07) [2026-10-22].
- **Virtualizace `/proc`** (prompt 07, `src/procvirt.h`): `open` jen pro
  čtení na `/proc/self/{cmdline,maps,mounts,mountinfo}` (+ `<můj pid>`,
  `/proc/mounts`, `/etc/mtab`) → memfd se syntetickým obsahem; cizí
  `/proc/<pid>/cmdline` bez nulové vycpávky. Jmenné open hooky + L1/L2 přes
  `shim_sys_intercept`. `run_ownall` před `elf_run` přepíše kernelovou
  oblast argv na guest argv (`ps` u cizích procesů) a nastaví `comm`.
  `readlink` výsledky pod `$ROOTFS` → guest. Exec shim předává vlastní
  `argv[0]` přes `--argv0`. `ELF_LOADER_NO_PROCVIRT=1` vypne comm + přepis
  oblasti [2026-10-22 (2), (3)].

## Pravidla a pasti (než sáhneš na kód)

1. **Guest TP vs bionický TLS.** Kód, který běží pod guest (parrot) TP, nesmí
   volat bionické TLS-závislé funkce (`snprintf`, `getpid`, `getenv`,
   `fprintf`, `sigaction`, `sigaltstack`). Buď raw syscall, nebo obalit
   `dl_enter_host()`/`dl_leave_host()`. `dl_enter_host` **musí** blokovat
   async signály. Tato past se v projektu opakovala 3× (`4602b18`, `80c3ffd`,
   `c69eeac`).
2. **glibc 2.41 funkce začínají `BTI c`** → `hook_install` je nepatchuje a
   glibc-interní `bl` obejde shim. Řešit wrapperem na vnější funkci nebo
   `hook_inline_prologue`.
3. **GOT override je per-jméno.** Při přidání shimu zkontrolovat aliasy
   (`nm -D libc.so.6`) a importy binárek (`nm -D --undefined-only`).
4. **Jeden rc pro oba světy — nikdy `/root/…` natvrdo.** `/root/.zshrc`
   a `$ROOTFS/root/.zshrc` jsou tentýž inode, liší se jen `$HOME`; host
   bionic shell `/root` nemá. Vždy `$HOME/…`. Po instalaci
   `grep -n '/root/' ~/.zshrc ~/.bashrc`.
5. **`su -c` běží přes proot a maskuje path bugy** — platný test je jen
   `ashell -c`.
6. **`test-all.sh`:** bare `grep` = host toybox (nezná BRE `\|`) → `grep -E`.
   `ashell_out` má `timeout 2` → chladný zápis cache se nevejde, použít
   `$R/usr/bin/timeout 25`. Sadu `all` nespouštět v `nohup … &` uvnitř
   background volání (osiřelý proces).
7. **Rootfs absolutní symlinky** jádro vyhodnocuje proti HOST kořeni →
   v rootfs držet kanonicky relativní tvar (`src/relsym.h`). Test `relsym`
   musí měřit HOST nástroji, jinak ho shim zachrání a A/B nic neprokáže.
8. **Hardlinky (`link`) zakazuje SELinux app domény** i nativně — platformní
   limit.
9. **Kernelová `struct sigaction` má na aarch64 i `sa_restorer`** — je
   **32 B**, ne 24. Raw `rt_sigaction(sig, NULL, &old, 8)` s menším
   bufferem tiše přepíše sousední lokálku na zásobníku (v promptu 11 to
   bylo počítadlo smyčky → nekonečná smyčka ve stavu R). Totéž hrozí
   u každé kernelové struktury, kterou si deklarujeme ručně.
10. **Kód volaný z L1 stubu** (`l1_before/after`, `shim_translate`, relsym)
   běží pod guest TP **na cizím zásobníku** (Go goroutine 8 KB) → žádné
   velké buffery na stacku (použít slot). Syscall ABI zachovává **všechny**
   registry kromě `x0` — i `x1..x7` se guestu vrací původní, ne přeložené.
   `stp/ldp` X párů má dosah jen 504 B. Host test asm musí mít mock, který
   caller-saved registry a NZCV úmyslně ničí (jinak negativní kontrola projde).

11. **Prootí `readlink` vrací cíle zkrácené na 14 B.** Uvnitř prootu dá
   `find -printf '%l'` na l2s symlinku `/.l2s/.l2s.div` — konstantně 14 B pro
   všechny, 2 874 cílů prázdných; host toybox dá správných 82 B. Analýzu
   symlinků v rootfs proto dělej **jen hostovými nástroji přes `ashell`**:
   jednou už to vyrobilo 169 391 falešných „osiřelých“ místo 67 187
   skutečných. Rozšiřuje past č. 7.
12. **Host `sh`/`test` je 32bitový.** `[ 3839243310 -lt 6442450944 ]` → `rc=1`
   (oba operandy přesahují 2³¹−1 a srazí se na `INT_MAX`). Guardy a kontroly
   velikostí počítej v MiB, nebo porovnávej `awk`em.
13. **`find -printf '%Z'` následuje symlinky** → hlásí label cíle, ne odkazu.
   Z 2 241 „špatně olabelovaných“ bylo 11 skutečných; zbytek byly visící
   odkazy (`?`), odkazy do `/system/bin` (`toolbox_exec`) a na `/dev/null`.
   Label odkazu samotného čti `ls -ldZ`.
14. **Relativní cíl symlinku nelze testovat `[ -e "$target" ]`** — vyhodnotí se
   proti cwd procesu, ne proti adresáři odkazu. Testuj `[ -e "$link" ]`.
   Tato chyba vyrobila hlášení 1 919 rozbitých odkazů, kterých bylo 0.
15. **Toybox `find` nezná `-printf '%h'`** (`%f`, `%l`, `%s`, `%p`, `%Z` jdou).
   S `2>/dev/null` na tom `find` chybu spolkne a vrátí prázdný výstup.

16. **Proot tiše zkracuje argv na ~504 položek.** `cmd $dir/*` nad velkým
   adresářem zpracuje jen zlomek: na `var/lib/dpkg/info/*.list` (2 431
   souborů, 220 936 B argv, `ARG_MAX` 2 097 152) dá shellový builtin 2 431,
   ale externí program po `exec` jen **504**; `xargs` dělí na dávky a každou
   zkrátí taky (1 008). Host i **náš loader dají správných 2 431** — je to
   výhradně proot. V prootu proto počítej `find`em nebo `ls -1 | grep -c`,
   nikdy globem předaným externímu programu. Tahle past vyrobila číslo
   „503 balíčků“ místo 2 431.
17. **Seccomp profil nelze měřit glibc sondou.** Pod loaderem je `syscall()`
   hookovaný a cestová `svc` místa se navíc přepisují
   (`elf_patch_syscall_sites`), pod prootem se syscall interponuje — obojí
   výsledek maskuje. `src/sigsys_probe.c` (glibc) nahlásil v rozsahu 0–600
   **0 KILL a 0 TRAP**, freestanding `-static -nostdlib` ET_EXEC s raw `svc`
   přes `ashell` nahlásil **0 KILL a 362 TRAP**. Platný je jen ten druhý.
18. **KOREKCE ke komentářům v `src/elf_loader.c:2177` a `:4141`: app profil
   tyhle syscally NEKILLuje, ale TRAPuje.** Naměřeno 2026-10-29:
   `set_robust_list` (99), `rseq` (293) i `clone3` (435) dávají
   `SECCOMP_RET_TRAP`, `si_code=1` (`SYS_SECCOMP`), `si_arch=0xc00000b7`.
   Profil je whitelist s TRAPem jako defaultem (proto TRAPuje i všechna
   neexistující čísla). Dopad: SIGSYS **je** zachytitelný, takže věta „KILL
   nejde přebít filtrem ani SIGSYS handlerem“ neplatí. Neplatí ale ani
   obrácený závěr, že je `elf_patch_syscall_sites` zbytečný: ERRNO filtr
   TRAP nepřebije (jádro bere z filtrů **nejnižší** akci a `RET_TRAP`
   0x00030000 < `RET_ERRNO` 0x00050000) a handler by u loaderu běžel pod
   cizím TP (past č. 1). Přepis instrukce zůstává nejlevnější cesta, jen
   z jiného důvodu, než je u ní napsaný.
19. **Staticky linkovaná binárka s prázdnou `.data` se nespustí.** Čistá
   `.bss` vyrobí `PT_LOAD` s `p_filesz == 0` a jádro na tomhle zařízení
   takový segment nenamapuje → SIGSEGV **před první instrukcí programu**
   (padne i volání bez argumentů, takže to vypadá na chybu v argv).
   Řešení: jedna inicializovaná globálka, nebo `-Wl,-N` (ten ale vyrobí RWX
   segment). Viz `tools/lxnative.c`.
20. **`la_symbind64` dostává `st_value` už jako finální runtime adresu.**
   glibc předává audit knihovně **kopii** symbolu, ve které `_dl_fixup`
   přepsal `st_value` na výslednou adresu. Přičtení `l_addr` ji posune
   podruhé → wrapper volá smetí a proces padne na SIGSEGV.
21. **Android má `/bin` → `/system/bin`.** `[ -e /bin/ls ]` proto na hostu
   uspěje a „guestovou“ absolutní cestu je nutné hledat **nejdřív v rootfs**.
   Opačné pořadí spustí Androidí toybox přes glibc `ld.so` a projeví se jako
   `libcrypto.so: cannot open shared object file` (bionic jména knihoven
   nemají verzi). A `PT_INTERP` sám nestačí k rozhodnutí „glibc binárka“ —
   bionic binárky ho mají taky (`/system/bin/linker64`), čti jeho obsah.

22. **Relativní cesty resolvuje jádro proti skutečnému cwd — shim je nevidí.**
   Když loader nastartuje s hostovým cwd mimo `$ROOTFS` (typicky `/`), guest
   si myslí, že je v guest korenu, ale `creat("xx00")` jde do HOSTOVÉHO `/`
   a padne na EACCES. Žádná jmenná ani L1/L2 vrstva to opravit nemůže — ta
   cesta se k nám nedostane. `run_ownall` proto cwd přepočítá na
   `$ROOTFS<cwd>`, pokud existuje (jako proot `-w`); `ELF_LOADER_NO_CWD_FIX=1`
   vypne. Tahle past byla **skutečná příčina** otevřeného nálezu „cesty se
   v child-exec-pod-`--ownall` scénářích nepřekládají, vzácné/časové"
   (2026-10-22 (7) bod 3) — nebylo to časování, ale cwd při spuštění.
   Projevila se na `test-all.sh` testu `csplit` (postup.md 2026-10-29 (5)).

23. **`setenv()` v loaderu nedojde do guestu.** Mění jen `environ` loaderu,
   ne pole `envp`, které se předává `run_ownall` a dědí do potomků. Pod
   `--proot-compat` tak `-0`/`-l` platilo jen pro PRVNÍ proces: `apt` vidělo
   euid 0, jeho dcera `dpkg` už ne („requested operation requires superuser
   privilege", rc=100). Kdo nastaví env po startu, musí ho promítnout i do
   `envp` (postup.md 2026-10-29 (6)).

24. **Guest procesy dědí HOSTOVÝ `PATH` — a na Androidu `/system/bin` existuje.**
   Guest shell pod loaderem má `PATH=$D/usr/bin:/system/bin:/system/xbin:…`,
   takže `bash -c 'id'` spustí **Androidí toybox** `/system/bin/id`, který běží
   nativně **mimo loader** — žádný fakeroot, žádný překlad cest. Projeví se to
   jako „fakeroot nefunguje pro potomky": `--ownall bash -c 'id'` dá uid=10323,
   ale `--ownall bash -c '/usr/bin/id'` dá uid=0. Stejná příčina shodila
   `dpkg --configure`: „warning: 'ldconfig' not found in PATH", „error:
   7 expected programs not found in PATH". Guestovi je proto nutné nastavit
   guest `PATH` (`/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`)
   Rozšiřuje past č. 21 (2026-10-29 (9)). **Opraveno 2026-10-29 (12):**
   `run_ownall` guest `PATH` nastavuje (hostový zůstává v `LX_HOST_PATH`
   v env loaderu), přepis jen když PATH obsahuje `/system/{bin,xbin}` —
   PATH poskládaný `lx`/double-world zůstává nedotčený; vypnutí
   `ELF_LOADER_NO_GUEST_PATH=1`. Pozor: přepisuje se **ukazatel v původním
   poli `envp`**, ne nová calloc kopie — `envp` je kernelová oblast na
   zásobníku, navazuje na ni auxv a `procvirt_rewrite_argarea` z ní počítá
   hranice, takže heapová kopie shodí guest libc init na SIGSEGV (naměřeno).

25. **Hybridní dispatch na skutečný proot nikdy nefungoval** (do 2026-10-29).
   `proot_compat_exec_real()` vkládalo před příkaz `--`, které tenhle proot
   build nezná (`proot error: unknown option '--'`), a pak předávalo
   **hostovou** cestu, i když proot má `-r rootfs` a čeká guest cestu
   (`'<rootfs>/usr/sbin/ldconfig' not found`). Mrtvé proto byly VŠECHNY cesty,
   co na něm visí: `lx-force-proot`, qemu pro cizí `e_machine`
   i `ELF_LOADER_STATIC_PROOT`. Důsledek pro ladění: selhání se tvářilo jako
   „loader to neumí", ne „záchytná cesta je rozbitá". Fallback v
   `proot_compat_main` (neznámá volba → celé argv prootu) posílal `--` dál
   ještě i po té první opravě.

26. **Toybox `find … -exec cat {} +` tiše vrátí prázdno.** Ne chybu, ne
   nenulový rc — prostě nic. Vyrobilo to „0 očekávaných `/etc` položek"
   místo 1050 při analýze `dpkg/info/*.list`. Používej `while read -r f; do
   … "$f" …; done < seznam`. Rozšiřuje past č. 15 (toybox `find` nezná
   `-printf '%h'`).

## Otevřené bugy

- ~~**Statický glibc ELF v rootfs končí SIGSYS** (RC=159)~~ — **vyřešeno pod
  L2** (prompt 05, 2026-10-21 (3)): `ELF_LOADER_L2=1` dá statické binárce
  filtr na její text-range, cestové syscally se přeloží a doběhne (RC=0, vidí
  rootfs). Bez L2 (default) pořád padá — na default přepnout až s L2.
- ~~**Vlastní `argv[0]` se při exec zahazuje**~~ — **vyřešeno** (prompt 07,
  2026-10-22 (2)): exec shim předává `--argv0`, Claude Code `grep`/`find`
  (`ugrep`/`bfs`) funguje bez `command grep`.
- ~~**`/proc/self/cmdline` není shimnutý**~~ — **vyřešeno** (prompt 07,
  2026-10-22 (3)): memfd pro vlastní `cmdline`/`maps`/`mounts`/`mountinfo`,
  přepis kernelové oblasti argv pro cizí čtenáře (`ps`), `comm` přes
  `PR_SET_NAME`.
- ~~**Binárku mimo `$ROOTFS` nelze spustit**~~ a ~~**vnořený `make`**~~ —
  **nereprodukovatelné 2026-10-29**, oba nálezy (prefix přebývá / prefix
  chybí) prošly: glibc binárka v `$D/tmp/.../scratchpad` běží jak přímo
  (`--ownall <abs cesta>`), tak jako dítě guest bashe, a `make -C sub`
  s `which cmake` uvnitř dá `/usr/bin/cmake` + rc=0. Pravděpodobně to
  spravila identity tabulka bindů (prompt 06) + větev `is_glibc_elf` mimo
  ROOTFS. Původní popis [#L4029].
- **rustc/cargo odvozuje `sysroot` z `cwd`, ne z binárky** → `E0463: can't
  find crate for std`. Root cause neizolován. Workaround
  `--sysroot $R/root/.rustup/toolchains/stable-aarch64-unknown-linux-gnu`
  [#L3988].
- **L1: `pthread_cancel` vlákna blokovaného v cestovém syscallu** se projeví
  až po návratu (PC je v `l1_common`, ne v okně `__syscall_cancel_arch`).
  Bezpečné, jen odložené. Default L1 zatím vypnutý.
- **Celorootfs konverze absolutních symlinků čeká na potvrzení** (1093 linků,
  1081 převoditelných) — `tools/relsymlinks.sh --dry-run`. V nativním módu
  (prompt 12) to **není kosmetika, ale nutná podmínka**: `/usr/bin/awk`
  a `/usr/bin/which` jsou symlinky na absolutní `/etc/alternatives/…`, které
  jádro vyhodnotí proti HOSTOVÉMU korenu už při otevírání programu — tedy
  dřív, než se načte audit knihovna → `error while loading shared libraries:
  …/awk: cannot open shared object file`. Překladem to opravit nelze.
- **glibc 2.42+ podpora je odvozená, ne obecná** — layout falešných
  `_rtld_global(_ro)` je ověřen jen pro 2.41 (parrot) a 2.43 (kali);
  2.42 dostane layout 2.43 neověřeně. Při další verzi glibc znovu změřit
  (`ldso_apply_glibc_layout`, postup.md 2026-10-22 (4)).
- **Prompt 08 mezery:** ~~fakeroot/l2s jen přes jmenné hooky (ne L1/L2 →
  statické/Go binárky je nevidí)~~ — **vyřešeno** 2026-10-29 (8): L2 filtr
  pod fakerootem TRAPuje i identitní čísla (`go_fakeroot_pre/_post`)
  a `l1_after` opravuje výsledek `stat` rodiny. `L1=1` samotné identitu
  nedodá (patchuje jen cestová `svc`, `getuid` cestu nemá) → fakeroot pro
  statické/Go vyžaduje **L2**. `lstat`
  symlinku hlásí velikost šifrovaného cíle (fscrypt na 4.14, i nativně →
  dpkg varuje „size has changed“). ~~`rename` přes l2s jméno nesnižuje
  počítadlo~~ — opraveno promptem 13 (T2) a **ověřeno 2026-10-29**
  (`.0002`→`.0001` při `mv x <l2s jméno>`, po `rm` v `.l2s` nic nezbyde;
  `tools/l2s-rename` scénář). ~~Vzácné ENOENT u dítěte pod `--ownall`~~ —
  byl to nepřeložený cwd, past č. 22.
- ~~**Vzácné nepřeložení cesty u potomka pod `--ownall`**~~ — **vyřešeno**
  2026-10-29 (5): relativní cesty resolvuje jádro proti skutečnému cwd
  (past č. 22), `run_ownall` ho přepočítá do ROOTFS. `test-all.sh all`
  217/1/33 (jediný FAIL = známý `nslookup localhost`).
- **Glibc static-PIE v rootfs loader nespustí** — `ldconfig` (Debian ho má jako
  static-PIE, `--check` dá `ET_DYN, deps=0`) padá: přes exec cestu RC=159
  (SIGSYS), při přímém `--ownall` SIGSEGV uvnitř svého vlastního textu. Pod
  skutečným prootem rc=0. **Důsledek:** `libc-bin.postinst` dělá
  `ldconfig -r "$DPKG_ROOT/"`, má `set -e` → balíček zůstane half-configured
  a apt/dpkg od té chvíle odmítá všechno další. Postihne to **každý balíček
  se shared library** (trigger `libc-bin`); `apt-get install hostname`, na
  kterém stála deklarovaná parita, žádnou `.so` neinstaluje, takže ldconfig
  nikdy nespustil — ten test paritu neprokazoval. Padá ve 0.9.3, 0.9.5
  i 0.9.7, bez fakerootu i bez l2s, a `ELF_LOADER_L2=1` nepomůže
  (static-PIE na tomhle zařízení nefunguje ani u prázdného `main`).
  **Workaround ověřený 2026-10-29:** `ELF_LOADER_STATIC_PROOT=1` → rc=0,
  nebo basename v `$ROOTFS/etc/lx-force-proot`. Obojí funguje až po opravě
  pasti č. 25. **Vyřešeno 2026-10-29 (12):** `elf_is_static_pie()` (ET_DYN
  bez PT_INTERP) rozlišuje glibc static-PIE od bionic static (ashell,
  su_wrapper = ET_EXEC → zůstávají na přímém real execve; dynamické bionic
  mají `PT_INTERP=/system/bin/linker64`), takže se na proot směruje
  **automaticky bez knobu**. Platí pro obě cesty: exec shim
  (`elf_hybrid_needs_real_proot`) i přímé `--ownall <static-PIE>`
  (`run_ownall`, předtím rc=139). Naměřeno na kali: `ldconfig --version`
  přímo rc=0, přes exec rc=0. Vypnutí `ELF_LOADER_NO_STATIC_PIE_PROOT=1`.
- Test na reálném 16K Android 15+ (Task 3). `nmap` není v rootfs [#L680];
  ostatní síťové binárky ověřeny OK.
- Kosmetika: `src/main.c:149` sign-compare, `\]` escape.

## Mapa postup.md

| téma | řádky |
|---|---|
| build, testy, runtime hodnoty, stav | L8–L92 |
| IFUNC resolver | L49 |
| static TLS pro exe, tcbhead | L101 |
| `strerror`, tunable env override | L121, L139 |
| bionic static-PIE rebase | L154 |
| PAGE_SIZE 16K | L189 |
| NDK cross-compile (Modal), 16K/namespaces | L210, L246, L254 |
| brk desync / private arena | L279 |
| TLS blok pod TP | L303 |
| Magisk modul, `elf` wrapper, `LD_LIBRARY_PATH` | L328, L406 |
| `linuxsh`, mount propagace, architektura | L558, L598, L605, L731 |
| fixy #1/#2 (stack_chk, rseq, emutls) | L493, L509 |
| C++ inity pod bionic TP, apt, nano, TUI | L628, L651, L662, L795, L870 |
| seccomp filtr, fork veneer | L685, L704, L710 |
| shell/integrace, `init`, `.elfrc`, `lx`, dw | L2376, L2395, L2425, L2795 |
| uv/venv, shebang, fstat, test runner, NSS | L2529, L2570, L2606, L2715 |
| Node saga | L1004–L2272 |
| `pi`/`pi-starship`, dl_enter_host | L2742 + 2026-10-06 (2)–(6) |
| Bun, long argv, bionic bridge | L2273 + 2026-10-06 (7), 2026-10-12 |
| `/proc/self/exe`, `eaccess`, pathops, posix_spawn | 2026-10-07 (2)–(7) |
| relsym + rootfd | 2026-10-21 |
| L1 — překlad na hranici syscallu | 2026-10-21 (2) |
| L2 — seccomp záchytná síť (okno, go_handle) | 2026-10-21 (3) |
| tabulka mountů, bindy, incident binds testu | 2026-10-21 (4), 2026-10-22 |
| `--argv0`, virtualizace `/proc`, comm | 2026-10-22 (2), (3) |
| soft exec (prompt 11) | 2026-10-29 |
| výkon: timing, cache, hash, imgcache, lazy | 2026-10-16, 2026-10-20 |
| zátěžový test multi-runtime | L3885, L4047 |
| otevřené nálezy exec/argv | L3939, L3973, L3988, L4029 |

Otevřené bugy též v auto-paměti `[[elf-loader-open-bugs]]`.
