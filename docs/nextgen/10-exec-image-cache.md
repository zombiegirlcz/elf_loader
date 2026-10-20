# Prompt 10 — cache připravených obrazů (RELRO-sharing styl) + líné vázání

> **STAV: HOTOVO / UZAVŘENO (2026-10-20).** Obě části implementovány,
> obě default vypnuté — po promptu 02 už nemají co ušetřit (relokace je
> ~1 ms z 15–30 ms exec). Viz
> [00-README.md — Co je hotové](00-README.md#co-je-hotové-stav-2026-10-20),
> `AGENTS.md` sekce 10 a `postup.md` 2026-10-20 (4)–(7).

Pracuješ v `/root/elf_loader`. Přečti `docs/nextgen/00-README.md`, `AGENTS.md`
(sekce 2 — IFUNC, TLS, brk arena; sekce 9 — Bun a file-backed mapování) a
`docs/nextgen/proot-nahrada.md` kapitoly 5.5, 5.6. **Předpoklad:** prompty 01
a 02; pokračuj jen pokud profil ukazuje, že relokace/lookupy jsou pořád velký
podíl exec.

## Část A — cache obrazů

- Po relokaci glibc + deps na **deterministické adresy** uložit jejich RW/RELRO
  stránky (GOT, `.data` po relokaci, `.data.rel.ro`) do
  `$LX_LOG/cache/img/<hash buildids+bází>.img` + manifest.
- Další exec: namapovat moduly na stejné báze a RW části `mmap
  MAP_PRIVATE|MAP_FIXED` z obrazu místo relokace.
- Klíč: BuildID + velikost + mtime všech modulů, verze loaderu, báze, page size,
  přítomné hooky/patche (jinak by obraz obsahoval staré pointery na shimy!).
- Pointery do loaderu (shimy v GOT) — loader je PIE s ASLR, takže buď loader
  mapovat na pevnou bázi, nebo GOT položky shimů po načtení obrazu přepsat
  (seznam z cache). Rozhodni a zdůvodni.
- ASLR: rotovat bázi s generací cache (např. denně / při boot id změně).
- IFUNC výsledky závisí na CPU (hwcaps) → součást klíče.
- Neplatný obraz = fallback na plnou relokaci.

## Část B — líné vázání `JUMP_SLOT`

- PLT položky nevázat při startu; GOT[1]/GOT[2] nastavit na resolver trampolínu
  (jako `_dl_runtime_resolve` v ld.so), resolver za běhu najde symbol, zapíše
  GOT a skočí. Trampolína běží pod guest TP → bez bionic TLS.
- Respektovat `DF_BIND_NOW`/`DF_1_NOW`, `LD_BIND_NOW`, IFUNC (resolvovat
  eager — historie bugu `memcpy`), TLS deskriptory (eager).
- Za proměnnou `ELF_LOADER_LAZY=1`, default až po ověření.

## Hotovo, když

- Benchmark: 1000× `/bin/true`, `node -e 0`, `python3 -c 0`, `clang --version`
  před/po (do `postup.md`).
- A/B testy `test-all.sh imgcache` (změna libc → obraz se zneplatní, výsledek
  shodný), `test-all.sh lazybind`; `all` bez regrese s oběma zapnutými.
