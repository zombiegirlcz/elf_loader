#ifndef ELF_LOADER_IMGCACHE_H
#define ELF_LOADER_IMGCACHE_H

#include <stddef.h>
#include <stdint.h>

#include "../include/elf_loader.h"

/* Cache pripravenych obrazu modulu (prompt 10 cast A).
 *
 * Po relokaci maji RW/RELRO stranky modulu (GOT, .data, .data.rel.ro) uz
 * hotove absolutni adresy. Obraz je ulozi; pri dalsim exec se modul namapuje
 * na TOTEZ bazi a RW stranky se nactou z obrazu misto relokace.
 *
 * Klic obrazu: BuildID+size+mtime hlavni binarky, verze loaderu (FNV z
 * __DATE__/__TIME__ v cache.c), PAGE_SIZE a AT_HWCAP/AT_HWCAP2 (IFUNC
 * resolvery zavisi na CPU). Kazdy modul navic nese svuj vlastni cache klic,
 * ktery se pri restore porovnava - pri jakemkoli nesouhlasu se obraz zahodi
 * a jde se plnou relokaci.
 *
 * Pointery do loaderu: loader je PIE s ASLR, takze adresy shimu v ulozenem
 * GOT nejsou pri dalsim behu platne. Pri ukladani se kazde 8B zarovnane slovo
 * mirici do loaderu dohleda v override tabulce (jmeno shimu) a ulozi se jako
 * fixup se JMENEM; pri restore se prepise aktualni adresou. Slovo mirici do
 * loaderu, ktere v override tabulce NENI, je duvod obraz vubec nevytvorit -
 * radeji zadna cache nez tise spatny pointer.
 *
 * Zapnuto jen pres ELF_LOADER_IMGCACHE=1. Default vypnuto.
 *
 * Vse se vola v bionicke fazi loaderu (pred prepnutim na guest TP), takze
 * stdio/malloc/getenv jsou bezpecne.
 */

#define IMG_FORMAT_VERSION 1u

/* Zapnuta? (ELF_LOADER_IMGCACHE=1) */
int img_enabled(void);

/* Diagnostika (ELF_LOADER_IMG_TRACE=1). */
int img_trace(void);

/* Otevre obraz pro danou hlavni binarku (exe_key z cache_build_key).
 * Musi se zavolat pred nacitanim modulu. Bez obrazu = no-op. */
void img_open(const char *exe_key);

/* Preferovana baze pro modul s touto host cestou, nebo 0. */
uintptr_t img_preferred_base(const char *path);

/* Pokus o restore RW stranek modulu z obrazu. Vraci 1 = restore probehl
 * (volajici MUSI preskocit elf_relocate), 0 = jde se plnou relokaci. */
int img_restore_module(elf_object_t *m, const char *path);

/* 1 = adresa lezi v RELRO useku obnovenem z obrazu -> elf_relocate ji ma
 * preskocit (hotova hodnota). */
int img_relro_covers(const elf_object_t *m, const void *addr);

/* Zaznam modulu do obrazu (po uspesne relokaci). Zapis na disk dela
 * img_commit(). */
void img_record_module(elf_object_t *m, const char *path);

/* Zapise nasbirane zaznamy (atomicky, tmp + rename). */
void img_commit(const char *exe_key);

/* Statistika pro ELF_LOADER_TIMING/diagnostiku: kolik modulu se restoroval
 * a kolik se zaznamenalo. */
void img_stats(int *restored, int *recorded);

/* ELF_LOADER_IMG_VERIFY=1: vypise rozdily obraz vs. plna relokace. */
void img_verify_report(void);

#endif
