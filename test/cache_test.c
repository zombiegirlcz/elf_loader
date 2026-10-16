/* cache_test.c — host-side unit test for src/cache.c (no device needed).
 *
 * Verifies: key stability, invalidation on mtime/size/BuildID change,
 * store->load round trip, atomic write leaves no .tmp, corrupt file = miss,
 * missing file = miss, and that the real loader ELF yields a BuildID key.
 * Build: cc -Wall -Iinclude -Isrc test/cache_test.c src/cache.c -o /tmp/ct
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <elf.h>
#include "cache.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else printf("ok: %s\n", msg); \
} while (0)

int main(int argc, char **argv) {
    char tmpdir[] = "/tmp/eltestXXXXXX";
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 2; }
    setenv("LX_LOG", tmpdir, 1);
    setenv("ROOTFS", "", 1);

    const char *cd = cache_dir();
    CHECK(cd && strstr(cd, tmpdir), "cache_dir uses LX_LOG");

    unsigned char img[512];
    memset(img, 0, sizeof img);
    img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[EI_CLASS] = ELFCLASS64;
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    eh->e_phoff = sizeof(Elf64_Ehdr);
    eh->e_phnum = 1;
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);
    ph->p_type = PT_NOTE;
    ph->p_offset = 128;
    ph->p_filesz = 32;
    unsigned char *n = img + 128;
    uint32_t nsz = 4, dsz = 8, typ = 3;
    memcpy(n, &nsz, 4); memcpy(n + 4, &dsz, 4); memcpy(n + 8, &typ, 4);
    memcpy(n + 12, "GNU", 4);
    for (int i = 0; i < 8; i++) n[16 + i] = (unsigned char)(0xa0 + i);

    char k1[CACHE_KEY_MAX], k2[CACHE_KEY_MAX], k3[CACHE_KEY_MAX], k4[CACHE_KEY_MAX];
    cache_build_key(img, sizeof img, 1000, k1, sizeof k1);
    cache_build_key(img, sizeof img, 1000, k2, sizeof k2);
    CHECK(strcmp(k1, k2) == 0, "key stable for same (BuildID,size,mtime)");
    CHECK(strstr(k1, "a0a1a2a3a4a5a6a7") != NULL, "key carries BuildID hex");

    cache_build_key(img, sizeof img, 2000, k3, sizeof k3);
    CHECK(strcmp(k1, k3) != 0, "key changes when mtime changes");

    cache_build_key(img, sizeof img - 1, 1000, k4, sizeof k4);
    CHECK(strcmp(k1, k4) != 0, "key changes when size changes");

    n[16] = 0xbb;
    cache_build_key(img, sizeof img, 1000, k4, sizeof k4);
    CHECK(strcmp(k1, k4) != 0, "key changes when BuildID changes");
    n[16] = 0xa0;

    cache_module_t cm;
    memset(&cm, 0, sizeof cm);
    cm.nr[0] = 99; cm.nr[1] = 293; cm.nr[2] = 435; cm.nr[3] = 151; cm.nr[4] = 152;
    uint32_t a1[] = { 0x1234, 0x5678 };
    uint32_t a2[] = { 0x9abc };
    cm.list[0].n = 2; cm.list[0].off = a1;
    cm.list[1].n = 1; cm.list[1].off = a2;
    CHECK(cache_store_module(k1, &cm) == 1, "store returns success");

    char tmppath[5000];
    snprintf(tmppath, sizeof tmppath, "%s/%s", cd, k1);
    struct stat st;
    CHECK(stat(tmppath, &st) == 0, "cache file exists after store");

    char cmd[5200];
    snprintf(cmd, sizeof cmd, "ls -1 %s | grep -c tmp", cd);
    FILE *p = popen(cmd, "r");
    int nt = -1; if (p) { if (fscanf(p, "%d", &nt) != 1) nt = -1; pclose(p); }
    CHECK(nt == 0, "atomic write leaves no .tmp file");

    cache_module_t *cm2 = cache_load_module(k1);
    CHECK(cm2 != NULL, "load after store returns a module");
    if (cm2) {
        CHECK(cm2->nr[0] == 99 && cm2->list[0].n == 2 &&
              cm2->list[0].off[0] == 0x1234 && cm2->list[0].off[1] == 0x5678,
              "round trip preserves nr/offsets (slot 0)");
        CHECK(cm2->list[1].n == 1 && cm2->list[1].off[0] == 0x9abc,
              "round trip preserves nr/offsets (slot 1)");
        cache_module_free(cm2);
    }

    int fd = open(tmppath, O_WRONLY);
    if (fd >= 0) { char bogus[8] = "XXXXXXX"; (void)!write(fd, bogus, 7); close(fd); }
    cm2 = cache_load_module(k1);
    CHECK(cm2 == NULL, "corrupt cache file -> silent miss");
    if (cm2) cache_module_free(cm2);

    cm2 = cache_load_module("doesnotexist_1_1_v1");
    CHECK(cm2 == NULL, "missing cache file -> silent miss");

    const char *self = argc > 1 ? argv[1] : "/root/elf_loader/elf_loader";
    FILE *f = fopen(self, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < (1 << 24)) {
            void *buf = malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                char kk[CACHE_KEY_MAX];
                cache_build_key(buf, (size_t)sz, 42, kk, sizeof kk);
                printf("info: key(%s) = %s\n", self, kk);
                CHECK(strstr(kk, "nobid") == NULL, "real ELF has GNU BuildID");
            }
            free(buf);
        }
        fclose(f);
    } else {
        printf("info: %s not found, skipping real-ELF key check\n", self);
    }

    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
    (void)!system(cmd);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}