# Prompt 09 — proot-kompatibilní CLI loaderu + hybridní dispatch

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
a `docs/nextgen/proot-nahrada.md` kapitoly 3.6 a 7. **Předpoklady:** prompty
06 (binds) a 08 (fake root, l2s); ideálně 05 (L2).

## Cíl

V aplikaci (`ProotManager.kt` v com.linux_core) půjde proot vyměnit za loader
jen záměnou binárky. Co loader neumí, předá skutečnému proot.

## Úkoly

1. Zjisti, jak appka proot volá (argumenty, env: `PROOT_L2S_DIR`,
   `PROOT_TMP_DIR`, `PROOT_LOADER`…) — z procesů (`/proc/*/cmdline` běžících
   proot instancí), env a případně ze zdrojů ProotManageru, pokud jsou dostupné.
2. Mód `elf_loader --proot-compat <proot argv>` (a/nebo detekce podle
   `argv[0] == proot`): parsovat `-r/--rootfs`, `-b/--bind` (`host[:guest]`),
   `-w/--cwd`, `-0/--root-id`, `-l/--link2symlink`, `-k/--kernel-release`,
   `--kill-on-exit`; neznámé volby → předat celé volání skutečnému proot.
3. **Hybridní dispatch v exec shimu:** binárky, které loader neumí (statické bez
   L2, cizí `e_machine`, cokoli z konfiguračního seznamu `lx-force-proot`),
   spustit pod skutečným proot se stejnými binds; glibc dynamické pod loaderem.
   Cizí architektura: pokud je v rootfs `qemu-<arch>-static`, použít ho (`-L`).
4. Fallback proměnná `LX_PROOT_BIN` (cesta ke skutečnému proot).

## Hotovo, když

- Typické volání proot z appky spuštěné přes loader dá interaktivní shell
  s binds, fake rootem a funkčním `apt`.
- Statická binárka bez L2 jde přes proot a vidí rootfs.
- A/B test `test-all.sh prootcompat`; `all` bez regrese.
- Návod na záměnu v `docs/nextgen/` (krátký `prechod-z-proot.md`).
