# Prompt 01 — měření: proot vs. loader vs. chroot + fázový profil exec

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md` (společná
pravidla), `AGENTS.md` a `docs/nextgen/proot-nahrada.md` kapitoly 1, 5 a 9.

## Cíl

Získat tvrdá čísla, podle kterých se rozhodne o všech dalších promptech.
Studie má v kapitolách 1 a 5 jen řádové odhady.

## Úkoly

1. **Fázový profil exec v loaderu.** Přidej za proměnnou `ELF_LOADER_TIMING=1`
   časové značky (raw `clock_gettime(CLOCK_MONOTONIC)` přes syscall, výpis raw
   `write(2)`) na hranicích fází: start `main`, parsování argv/env, čtení
   whitelistu/configu, mapování exe a deps, relokace, registrace hooků
   (`[hook]` lookupy), `elf_patch_syscall_sites`, F2 inline patching, instalace
   seccomp filtru, init guest modulů, skok na entry. Na straně rodiče (exec shim)
   změř `shim_execve`/`shim_posix_spawnp` od vstupu po skutečný `execve`.
2. **Vysvětli rozdíl 34 ms** z postup.md:3917 (`$ROOTFS/bin/true` přes shim
   48 ms vs. `elf_loader $ROOTFS/bin/true` přímo 14 ms). Oba běhy dělají stejné
   relokace, takže hypotéza „cache symbolů" sama nestačí. Porovnej env potomka,
   argv loaderu, re-exec přes `/proc/self/exe`, počet seccomp filtrů. Výsledek:
   tabulka fází pro obě cesty.
3. **Benchmark skript** `tools/bench-nextgen.sh` se třemi režimy: proot (binárka
   `/data/user/0/com.linux_core/files/usr/bin/proot`, se stejnými volbami jako
   appka — zjisti z env/ProotManageru), loader (`--ownall`), `linuxsh` chroot
   (jen pokud je root; jinak režim přeskoč a napiš to). Zátěže z kapitoly 9:
   1000× `/bin/true`, `./configure` malého projektu, `make -j8` (cJSON),
   `find /usr -type f | wc -l`, `git status` na velkém repu,
   `python3 -c 'import numpy, pandas'`, `java -version` (sdkman), `gh --version`,
   `node -e 0`, `apt-get install --reinstall` malého balíku (jen v režimu, kde to
   dává smysl). Každé měření 3–5×, medián. Ověř, zda proot běží se seccomp
   akcelerací (`PROOT_NO_SECCOMP`).
4. Výsledky zapiš do `postup.md` a shrň do nové kapitoly „Naměřeno" na konci
   `docs/nextgen/proot-nahrada.md` (tabulka zátěž × režim + fázový profil).
   Odhady v kapitolách 1 a 5 oprav podle reality.

## Hotovo, když

- `ELF_LOADER_TIMING=1` vypisuje fáze a v defaultu nic nestojí.
- Je známo, kam jde 34 ms, a doporučení, které z promptů 02/10/11 má největší
  smysl (podle pravidla „(loader − chroot) × četnost").
- Benchmark je opakovatelný jedním příkazem.
