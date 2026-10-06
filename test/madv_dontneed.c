/* Regresni test: madvise(MADV_DONTNEED) na inicializovana data exe musi
 * stranky znovu nacist ze souboru (file-backed MAP_PRIVATE), ne vratit nuly.
 * Bun standalone (claude) to dela se svou .bun sekci -> pri anonymnim
 * mapovani segmentu "SyntaxError: Invalid character '\0'".
 * Ocekavany vystup: "MADV A". */
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

__attribute__((aligned(65536))) char big[3 * 65536] = {[0 ... 3 * 65536 - 1] = 'A'};

int main(void) {
    uintptr_t p = ((uintptr_t)big + 65535) & ~(uintptr_t)65535;
    if (madvise((void *)p, 65536, MADV_DONTNEED) != 0) {
        perror("madvise");
        return 2;
    }
    char c = *(volatile char *)(p + 100);
    printf("MADV %c\n", c ? c : '0');
    return c == 'A' ? 0 : 1;
}
