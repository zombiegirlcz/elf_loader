#ifndef ELF_LOADER_CACHE_H
#define ELF_LOADER_CACHE_H

#include <stddef.h>
#include <stdint.h>

/* BuildID-keyed cache pro vysledky skenu/hook-lookupu v exec ceste.
 *
 * Cache soubor =
 *   <cache_dir>/<buildid hex or "nobid">_<size>_<mtime_ns>_v<ver>-<loader tag>
 * `loader tag` = FNV-1a z __DATE__/__TIME__ loaderu, takze po jeho prekladu
 * se stara cache (vysledky skenu stareho loaderu) nikdy nepouzije.
 * Kazdy modul ma vlastni soubor. Pri zmene binarky (jiny BuildID/size/mtime)
 * vznikne jiny klic -> stara cache se nikdy nepouzije.
 *
 * Vsechny funkce se volaji v loaderove bionicke fazi (pred prepnutim na guest
 * TP), takze stdio/malloc/getenv jsou bezpecne. Zadny kod pod guest TP je
 * odsud nevolá. */

/* Format cache souboru. Pri zmene layoutu bumpni.
 * v2: slot nese priznak `scanned`, takze se cachuje i NEGATIVNI vysledek
 *     skenu (modul pro dane syscall cislo zadny svc-site nema). Bez toho
 *     se moduly bez nalezu (libm, libz, libexpat...) skenovaly pri kazdem
 *     exec znovu - viz postup.md 2026-10-20. */
#define CACHE_FORMAT_VERSION 2u

/* Pocet syscall cisel, ktera elf_patch_syscall_sites prebiji (99,151,152,
 * 293,435) a jejichz svc-site offsety se do cache ukladaji. */
#define CACHE_NR_COUNT 5
#define CACHE_MAX_SITES 64
#define CACHE_KEY_MAX  192

/* Vysledek skenu pro jedno syscall cislo: link-time vaddr instrukci `svc #0`
 * patri­cich sekvenci `movz x8,#nr ; ... ; svc #0`.
 *
 * `scanned` = sken pro tohle cislo uz probehl a `n`/`off` jsou jeho UPLNY
 * vysledek (vcetne n == 0). Jen tak lze preskocit sken i u modulu, ktere
 * zadny svc-site nemaji - a tech je vetsina. */
typedef struct {
    uint32_t n;
    uint32_t scanned;
    uint32_t *off;   /* malloc'd pole n link-time vaddr */
} cache_nr_t;

/* Cela cache jednoho modulu: pevne CACHE_NR_COUNT cisel. */
typedef struct {
    long      nr[CACHE_NR_COUNT];
    cache_nr_t list[CACHE_NR_COUNT];
} cache_module_t;

/* Vypocita cache klic pro ELF obraz v pameti (file_map velikosti fsize,
 * mtime_ns = mtime v ns). out musi mit >= CACHE_KEY_MAX B. Kdyz ELF nema
 * GNU BuildID, pouzije se "nobid" (klic stale obsahuje size+mtime). */
void cache_build_key(const void *file_map, size_t fsize, long long mtime_ns,
                     char *out, size_t outsz);

/* Nacte cache modulu pro dany klic. Vraci malloc'd cache_module_t nebo NULL
 * (miss / jakakoli chyba - nikdy neprerusi exec). */
cache_module_t *cache_load_module(const char *key);

/* Ulozi cache modulu atomicky (<key>.tmp + rename). Vraci 1 pri uspechu. */
int cache_store_module(const char *key, const cache_module_t *cm);

/* Uvolni cache_module_t vcetne off poli. */
void cache_module_free(cache_module_t *cm);

/* Cache adresar (existujici, vytvoreny mkdir -p jednou urovni):
 *   $LX_LOG/cache  |  $ROOTFS/root/elf_loader/cache  |  /tmp/elf_loader-cache
 * Vraci staticky buffer nebo NULL. */
const char *cache_dir(void);

/* Diagnostika zapnuta pres ELF_LOADER_CACHE_TRACE=1. */
int cache_trace(void);

#endif