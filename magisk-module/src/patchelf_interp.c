/* Rewrite the PT_INTERP of aarch64 ELF binaries whose interpreter is
 * "/lib/ld-linux-aarch64.so.1" to a short writable path so the kernel can
 * exec them on Android (whose root filesystem is read-only / verity).
 *
 * The replacement string must fit in the existing PT_INTERP segment, so it
 * must be no longer than the original.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -s patchelf_interp.c -o ...
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>

#define OLD_INTERP "/lib/ld-linux-aarch64.so.1"
#define NEW_INTERP "/data/local/tmp/ldl"

static int patch_one(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    Elf64_Ehdr eh;
    if (read(fd, &eh, sizeof eh) != sizeof eh ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_type == ET_REL) {
        close(fd);
        return 0;
    }
    int ret = 0;
    if (lseek(fd, (off_t)eh.e_phoff, SEEK_SET) < 0)
        goto out;
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        if (read(fd, &ph, sizeof ph) != sizeof ph)
            break;
        if (ph.p_type != PT_INTERP)
            continue;
        if (ph.p_filesz == 0 || ph.p_offset > (off_t)1 << 40)
            break;
        char *buf = malloc(ph.p_filesz);
        if (!buf)
            break;
        off_t saved = lseek(fd, 0, SEEK_CUR);
        if (lseek(fd, (off_t)ph.p_offset, SEEK_SET) < 0) {
            free(buf);
            goto out;
        }
        if (read(fd, buf, ph.p_filesz) != (ssize_t)ph.p_filesz) {
            free(buf);
            lseek(fd, saved, SEEK_SET);
            break;
        }
        if (strcmp(buf, OLD_INTERP) == 0 && strlen(NEW_INTERP) + 1 <= ph.p_filesz) {
            if (lseek(fd, (off_t)ph.p_offset, SEEK_SET) < 0) {
                free(buf);
                goto out;
            }
            if (write(fd, NEW_INTERP, strlen(NEW_INTERP) + 1) ==
                (ssize_t)strlen(NEW_INTERP) + 1) {
                printf("patched %s\n", path);
                ret = 0;
            }
        }
        free(buf);
        lseek(fd, saved, SEEK_SET);
        break;
    }
out:
    close(fd);
    return ret;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file...>\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++)
        patch_one(argv[i]);
    return 0;
}