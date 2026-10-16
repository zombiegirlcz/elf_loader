/* timing.c — ELF_LOADER_TIMING fázový profil exec.
 *
 * Zapnuto jen kdyz je v env ELF_LOADER_TIMING (a neni "0"). Default: vsechny
 * funkce jsou no-op vetve (jedno cache pole, zadny syscall).
 *
 * BEZPECNOST: mark() se muze volat pod bionickym i pod parrot (guest) TP a v
 * oknech, kde nesmi zaznit zadny bionic kod (malloc/stdio/errno). Proto jen
 * raw syscally: clock_gettime(113) a write(64) pres inline svc. Zadny getenv()
 * za behu - flag se nascachuje v main() / prvnim volani (bionicky TP).
 *
 * Vystup: jednoradkove [TIMING] <label> delta=<us>us total=<us>us na fd 2.
 * Format je stabilni pro parsovani bench skriptem.
 */
#define _GNU_SOURCE 1
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../include/elf_loader.h"

/* aarch64 syscall numbers (jen ty, ktere potrebujeme). */
#define TS_SYS_write           64
#define TS_SYS_clock_gettime  113

struct ts_timespec { long tv_sec; long tv_nsec; };

static long ts_raw_syscall2(long nr, long a0, long a1) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1)
                     : "memory", "cc");
    return x0;
}

/* -1 = nezjisteno, 0 = vypnuto, 1 = zapnuto */
static int g_ts_enabled = -1;
static long g_ts_first_ns  = 0;   /* prvni mark (CLOCK_MONOTONIC ns) */
static long g_ts_last_ns   = 0;   /* predchozi mark */

static long ts_now_ns(void) {
    struct ts_timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    long r = ts_raw_syscall2(TS_SYS_clock_gettime, 1 /*CLOCK_MONOTONIC*/,
                             (long)(uintptr_t)&ts);
    if (r < 0)
        return 0;
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

/* Rychly raw write na fd. Vraci pocet zapsanych bajtu nebo <0. */
static long ts_raw_write(int fd, const char *buf, long len) {
    register long x8 __asm__("x8") = TS_SYS_write;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(uintptr_t)buf;
    register long x2 __asm__("x2") = len;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

static int ts_append(char *b, int i, const char *s) {
    while (*s && i < 220) b[i++] = *s++;
    return i;
}

/* Desetinné cislo bez znamenka (jen pro >= 0), do bufferu. Vraci novy i. */
static int ts_append_u64(char *b, int i, unsigned long v) {
    char tmp[24];
    int n = 0;
    if (v == 0) { if (i < 220) b[i++] = '0'; return i; }
    while (v && n < 20) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && i < 220) b[i++] = tmp[--n];
    return i;
}

/* Nascachuj flag z env. Volat PRVNI (pod bionickym TP, kde getenv funguje).
 * Pokud se zavola pozdeji pod guest TP, getenv() vraci nesmysl - proto je
 * lepsi zavolat elf_timing_init() hned v main(). */
void elf_timing_init(void) {
    if (g_ts_enabled >= 0)
        return;
    const char *v = getenv("ELF_LOADER_TIMING");
    g_ts_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    if (g_ts_enabled) {
        g_ts_first_ns = ts_now_ns();
        g_ts_last_ns  = g_ts_first_ns;
    }
}

int elf_timing_enabled(void) {
    if (g_ts_enabled < 0)
        elf_timing_init();
    return g_ts_enabled;
}

/* Zaznamenej fazi. <label> musi byt staticky string (zadny format). */
void elf_timing_mark(const char *label) {
    if (!elf_timing_enabled())
        return;
    long now = ts_now_ns();
    if (!now)
        return;
    if (!g_ts_first_ns) {
        g_ts_first_ns = now;
        g_ts_last_ns  = now;
    }
    long delta_us = (now - g_ts_last_ns) / 1000;
    long total_us = (now - g_ts_first_ns) / 1000;
    g_ts_last_ns = now;

    char b[256];
    int i = 0;
    i = ts_append(b, i, "[TIMING] ");
    i = ts_append(b, i, label);
    i = ts_append(b, i, " delta=");
    if (delta_us < 0) { b[i++] = '-'; delta_us = -delta_us; }
    i = ts_append_u64(b, i, (unsigned long)delta_us);
    i = ts_append(b, i, "us total=");
    if (total_us < 0) { b[i++] = '-'; total_us = -total_us; }
    i = ts_append_u64(b, i, (unsigned long)total_us);
    i = ts_append(b, i, "us\n");
    if (i > 0)
        ts_raw_write(2, b, i);
}

/* Zaznamenej fazi s externim casem (napr. pres hranici, kde se musi pouzit
 * bionicky clock_gettime). cas je v mikrosekundach CLOCK_MONOTONIC. */
void elf_timing_mark_us(const char *label, unsigned long long us) {
    if (!elf_timing_enabled())
        return;
    long now_ns = (long)(us * 1000ULL);
    long delta_us = g_ts_last_ns ? (now_ns - g_ts_last_ns) / 1000 : 0;
    long total_us = g_ts_first_ns ? (now_ns - g_ts_first_ns) / 1000 : 0;
    if (g_ts_last_ns) g_ts_last_ns = now_ns;
    if (!g_ts_first_ns) { g_ts_first_ns = now_ns; g_ts_last_ns = now_ns; }

    char b[256];
    int i = 0;
    i = ts_append(b, i, "[TIMING] ");
    i = ts_append(b, i, label);
    i = ts_append(b, i, " delta=");
    if (delta_us < 0) { b[i++] = '-'; delta_us = -delta_us; }
    i = ts_append_u64(b, i, (unsigned long)delta_us);
    i = ts_append(b, i, "us total=");
    if (total_us < 0) { b[i++] = '-'; total_us = -total_us; }
    i = ts_append_u64(b, i, (unsigned long)total_us);
    i = ts_append(b, i, "us\n");
    if (i > 0)
        ts_raw_write(2, b, i);
}

/* Vrat CLOCK_MONOTONIC v mikrosekundach (pro mereni okolo externich bloku,
 * napr. v shimu pred execve). Bezpecne i pod bionickym TP. */
unsigned long long elf_timing_now_us(void) {
    long ns = ts_now_ns();
    return ns > 0 ? (unsigned long long)(ns / 1000) : 0ULL;
}