/* stack_probe.c — rozhodující test stack-detection hypotézy.
 *
 * Vypíše:
 *   - pthread_getattr_np(pthread_self()) → stackaddr/stacksize
 *   - __libc_stack_end
 *   - zda /proc/self/maps obsahuje [stack]
 *   - adresu lokální proměnné (aproximace SP)
 *   - dl_iterate_phdr: pro každý DSO name/base a jeho PT_LOAD + PT_GNU_RELRO
 *     (V8 detekuje code range právě přes dl_iterate_phdr)
 *
 * Spouští se (a) pod elf_loaderem přes ashell -c, (b) pod prootem přímo.
 * Rozdíl = důkaz/vvyrácení stack-detection hypotézy.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <link.h>

extern void *__libc_stack_end;

static int cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    printf("DSO name='%s' base=%p phnum=%d\n",
           info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : "<main>",
           (void *)info->dlpi_addr, info->dlpi_phnum);
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD)
            printf("  LOAD vaddr=%#lx memsz=%#lx flags=%d\n",
                   (unsigned long)p->p_vaddr, (unsigned long)p->p_memsz,
                   (int)p->p_flags);
        else if (p->p_type == PT_GNU_RELRO)
            printf("  GNU_RELRO vaddr=%#lx memsz=%#lx\n",
                   (unsigned long)p->p_vaddr, (unsigned long)p->p_memsz);
    }
    return 0;
}

int main(void) {
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        void *addr = NULL;
        size_t sz = 0;
        pthread_attr_getstack(&a, &addr, &sz);
        printf("getattr stackaddr=%p size=%zu (0x%zx)\n", addr, sz, sz);
        pthread_attr_destroy(&a);
    } else {
        printf("pthread_getattr_np FAILED errno=%d\n", errno);
    }

    printf("__libc_stack_end=%p\n", __libc_stack_end);

    char probe;
    printf("&probe(stack approx)=%p\n", (void *)&probe);

    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[512];
        int found = 0;
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "[stack]")) { printf("MAPS: %s", line); found = 1; }
        }
        fclose(f);
        if (!found) printf("MAPS: no [stack] marker\n");
    } else {
        printf("MAPS: open failed errno=%d\n", errno);
    }

    printf("--- dl_iterate_phdr ---\n");
    dl_iterate_phdr(cb, NULL);
    printf("stack_probe done\n");
    return 0;
}