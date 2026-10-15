# Prompt 05 — L2: seccomp `RET_TRAP` jen v guest okně + líný přepis

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(sekce 6 seccomp, sekce 9 bionic bridge), `src/sigsys_probe.c` a
`docs/nextgen/proot-nahrada.md` kapitolu 2.2. **Předpoklad:** prompt 04 (stuby
a překladač existují).

## Cíl

Překládat cesty i v kódu, který L1 nepřepíše: statické binárky, Go, Bun (raw
syscally), musl, JIT.

## Návrh (závazné body)

- **Guest okno:** při startu rezervovat jedním `PROT_NONE` mmapem rozsah VA
  (návrh `0x10_0000_0000–0x20_0000_0000`; ověř proti adresám, které jádro
  na zařízení dává PIE a mmapům — 39bitový VA) a veškerý guest kód mapovat do
  něj `MAP_FIXED` (4.14 nemá `MAP_FIXED_NOREPLACE`). Arena `0x7f00000000` zůstává.
- **Filtr:** pro cestová čísla syscallů: `instruction_pointer` v okně →
  `SECCOMP_RET_TRAP`, jinak `ALLOW`. Pozor: 64bitové porovnání v cBPF = dvě
  32bitová (`instruction_pointer` hi/lo).
- **Proč okno a ne whitelist bioniku:** filtr se dědí a nejde odebrat; host
  binárky spuštěné později mají náhodné ASLR adresy mimo okno, takže je filtr
  neovlivní. To otestuj explicitně (guest → `/system/bin/sh -c 'ls /system'`).
- **SIGSYS handler** (rozšířit stávající): async-signal-safe, raw syscally,
  překlad přes stejný překladač jako L1, syscall provede z textu loaderu (mimo
  okno), výsledek do `uc_mcontext.regs[0]`, PC za `svc`. Musí fungovat i na
  malém zásobníku (Go) — zvaž `SA_ONSTACK` a respektuj sigaltstack guestu.
- **Líný přepis:** po prvním zachycení přepsat dané místo na L1 stub, pokud jde
  o kód modulu, který loader zná (ne JIT bez znalosti) → další volání bez signálu.
- Guest vlastní SIGSYS handler / seccomp (Chromium, Electron): řetězit přes
  `guest_sigaction`; guest filtr instalovat normálně (stacking).
- JIT: hook guest `mmap`/`mprotect` s `PROT_EXEC` → hint adresa v okně.
- Statické binárky: dnes `elf_needs_real_exec` → raw exec (vidí host FS).
  Přidej režim own-loadu statických ELF (ET_EXEC na pevné adrese mimo okno → buď
  přidat jejich rozsah per-proces dalším filtrem, nebo je relokovat nelze —
  rozhodni a zdůvodni). Za proměnnou, default zatím raw exec.
- Zapnout za `ELF_LOADER_L2=1`.

## Hotovo, když

- Statická glibc binárka (`gcc -static`) a Go binárka (`gh`) pod L2 vidí rootfs:
  `open("/etc/os-release")` vrátí Parrot, ne Android.
- Bun (`claude`) funguje bez `F2_NO_CWD_STRIP` hacku (cwd z raw `getcwd` je guest).
- Host binárka spuštěná z guestu pod filtrem běží normálně.
- Bionic bridge (`test-all.sh bionic`) dál PASS — driver otevírá host cesty.
- A/B test `test-all.sh l2`; `all` bez regrese; cena zachycení změřená.
