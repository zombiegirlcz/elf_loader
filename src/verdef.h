/* src/verdef.h — cteni DT_VERDEF primo ze SOUBORU (bez nacteneho modulu).
 *
 * Proc: podle "definuje tento exe verzi `BUN_*`?" se rozhoduje o zapnuti L2
 * (Bun dela fs operace raw syscally -> bez L2 nevidi rootfs). Rozhodnuti musi
 * padnout JESTE PRED elf_load(), protoze L2 rezervuje okno pro PIE exe driv,
 * nez se exe nacte. elf_has_verdef_prefix() v src/elf_loader.c umi totez, ale
 * az nad nactenym elf_object_t.
 *
 * Vklada se do src/main.c (potrebuje raw_open/raw_close/shim_raw_syscall6).
 * Host unit test: test/verdef_file_test.c (`make test-host`).
 */
#ifndef ELF_LOADER_VERDEF_H
#define ELF_LOADER_VERDEF_H

/* pread64 (nr 67) — nepohybuje file offsetem, takze se da parsovat ELF
 * tabulka bez lseek tance. TLS-safe (raw syscall). */
static ssize_t raw_pread(int fd, void *buf, size_t n, unsigned long off) {
    return (ssize_t)shim_raw_syscall6(67 /* pread64 */, fd, (long)buf,
                                      (long)n, (long)off, 0, 0);
}

/* vaddr -> offset v souboru podle tabulky PT_LOAD segmentu. 0 = nenalezeno. */
static unsigned long elf_file_v2o(const unsigned long *v, const unsigned long *o,
                                  const unsigned long *sz, int n,
                                  unsigned long va) {
    for (int i = 0; i < n; i++)
        if (va >= v[i] && va < v[i] + sz[i])
            return o[i] + (va - v[i]);
    return 0;
}

/* 1 = ELF v `path` DEFINUJE verzi symbolu s prefixem `pfx` (DT_VERDEF).
 * Souborova varianta elf_has_verdef_prefix() — na rozdil od ni nepotrebuje
 * nacteny modul, takze se da zavolat JESTE PRED elf_load(). To je nutne,
 * protoze podle odpovedi "je to Bun?" se rozhoduje o zapnuti L2, a L2 musi
 * rezervovat okno pro PIE exe driv, nez se exe nacte.
 * TLS-safe: jen raw_* syscally, zadny malloc, zadna libc funkce. */
static int elf_file_has_verdef_prefix(const char *path, const char *pfx) {
    if (!path || !path[0] || !pfx || !pfx[0]) return 0;
    int fd = raw_open(path, O_RDONLY);
    if (fd < 0) return 0;
    int found = 0;
    unsigned char h[64];
    if (raw_pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h) goto out;
    if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' ||
        h[4] != 2 /* ELFCLASS64 */)
        goto out;
    unsigned long phoff    = *(unsigned long *)(void *)(h + 32);
    unsigned short phentsz = *(unsigned short *)(void *)(h + 54);
    unsigned short phnum   = *(unsigned short *)(void *)(h + 56);
    if (phentsz < 56 || phnum == 0 || phnum > 256) goto out;

    unsigned long lv[16], lo[16], ls[16];
    int nld = 0;
    unsigned long dyn_off = 0, dyn_sz = 0;
    for (unsigned short i = 0; i < phnum; i++) {
        unsigned char ph[56];
        if (raw_pread(fd, ph, sizeof ph,
                      phoff + (unsigned long)i * phentsz) != (ssize_t)sizeof ph)
            goto out;
        unsigned int  t   = *(unsigned int *)(void *)ph;
        unsigned long off = *(unsigned long *)(void *)(ph + 8);
        unsigned long va  = *(unsigned long *)(void *)(ph + 16);
        unsigned long fsz = *(unsigned long *)(void *)(ph + 32);
        if (t == 2 /* PT_DYNAMIC */) { dyn_off = off; dyn_sz = fsz; }
        else if (t == 1 /* PT_LOAD */ && nld < 16 && fsz) {
            lv[nld] = va; lo[nld] = off; ls[nld] = fsz; nld++;
        }
    }
    if (!dyn_off || !dyn_sz || !nld) goto out;

    unsigned long vdef_va = 0, strtab_va = 0, vdefnum = 0;
    for (unsigned long k = 0; k < dyn_sz / 16 && k < 1024; k++) {
        unsigned char d[16];
        if (raw_pread(fd, d, sizeof d, dyn_off + k * 16) != (ssize_t)sizeof d)
            goto out;
        unsigned long tag = *(unsigned long *)(void *)d;
        unsigned long val = *(unsigned long *)(void *)(d + 8);
        if (tag == 0) break;                       /* DT_NULL */
        else if (tag == 5) strtab_va = val;        /* DT_STRTAB */
        else if (tag == 0x6ffffffcUL) vdef_va = val;    /* DT_VERDEF */
        else if (tag == 0x6ffffffdUL) vdefnum = val;    /* DT_VERDEFNUM */
    }
    if (!vdef_va || !strtab_va) goto out;
    unsigned long vdo = elf_file_v2o(lv, lo, ls, nld, vdef_va);
    unsigned long stro = elf_file_v2o(lv, lo, ls, nld, strtab_va);
    if (!vdo || !stro) goto out;

    size_t pl = 0;
    while (pfx[pl]) pl++;
    for (unsigned long k = 0; k < (vdefnum ? vdefnum : 64) && k < 64; k++) {
        unsigned char vd[20];   /* Elf64_Verdef: aux@12, next@16 */
        if (raw_pread(fd, vd, sizeof vd, vdo) != (ssize_t)sizeof vd) break;
        unsigned int vd_aux  = *(unsigned int *)(void *)(vd + 12);
        unsigned int vd_next = *(unsigned int *)(void *)(vd + 16);
        if (vd_aux) {
            unsigned char va[8];   /* Elf64_Verdaux: name@0 */
            if (raw_pread(fd, va, sizeof va, vdo + vd_aux) == (ssize_t)sizeof va) {
                unsigned int nm = *(unsigned int *)(void *)va;
                char nb[64];
                ssize_t r = raw_pread(fd, nb, sizeof nb - 1, stro + nm);
                if (r > 0) {
                    nb[r] = 0;
                    size_t i = 0;
                    while (i < pl && nb[i] == pfx[i]) i++;
                    if (i == pl) { found = 1; break; }
                }
            }
        }
        if (!vd_next) break;
        vdo += vd_next;
    }
out:
    raw_close(fd);
    return found;
}

#endif /* ELF_LOADER_VERDEF_H */
