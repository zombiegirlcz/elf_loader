/* Minimal static kernel-interpreter bridge for parrot rootfs binaries.
 *
 * The kernel runs this binary (via PT_INTERP) when exec'ing any parrot ELF
 * whose interpreter was rewritten to this path.  It then re-execs through the
 * real parrot ld.so, which loads our own loader:
 *
 *     parrot-ld.so --library-path <rootfs>/usr/lib/aarch64-linux-gnu \
 *         <loader> --ownall <target> <args...>
 *
 * Config file /data/adb/parrot_root:
 *     line 1: rootfs path   (default /data/adb/parrot)
 *     line 2: loader path   (default /system/bin/elf_loader)
 *
 * MUST NOT use libc: glibc's static startup crashes when a static binary is
 * used as an ELF interpreter, so this is built with -nostdlib and raw
 * aarch64 syscalls only.
 *
 * CRITICAL: do NOT use file-scope writable data (globals/static buffers).
 * The linker then emits a second LOAD segment for the .bss at a file offset
 * that lies beyond EOF, and any access to it raises SIGBUS.  All buffers live
 * on the stack.
 *
 * Build: aarch64-linux-gnu-gcc -nostdlib -static -O2 -s -fno-builtin \
 *            interp_wrapper.c interp_wrapper.S
 */
#define _GNU_SOURCE
#include <stddef.h>

typedef unsigned long ulong;
typedef long ssize_t;

#define SYS_read    63
#define SYS_write   64
#define SYS_openat  56
#define SYS_close   57
#define SYS_execve  221
#define SYS_exit    93

#define AT_FDCWD    (-100)
#define O_RDONLY    0

static long syscall(long n, long a, long b, long c, long d) {
    register long x0 __asm("x0") = a;
    register long x1 __asm("x1") = b;
    register long x2 __asm("x2") = c;
    register long x3 __asm("x3") = d;
    register long x8 __asm("x8") = n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                     : "memory");
    return x0;
}

static long my_open(const char *path) {
    return syscall(SYS_openat, AT_FDCWD, (long)path, O_RDONLY, 0);
}
static ssize_t my_read(int fd, void *buf, size_t n) {
    return syscall(SYS_read, fd, (long)buf, (long)n, 0);
}
static void my_close(int fd) { syscall(SYS_close, fd, 0, 0, 0); }

static ulong my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (ulong)(p - s);
}
static void my_write(int fd, const char *s) {
    syscall(SYS_write, fd, (long)s, (long)my_strlen(s), 0);
}
static void my_write_err(const char *s) { my_write(2, s); }

static void my_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++))
        ;
}
static void my_strcat(char *dst, const char *src) {
    while (*dst) dst++;
    my_strcpy(dst, src);
}
static int has_prefix(const char *s, const char *p) {
    while (*p) {
        if (*s != *p) return 0;
        s++;
        p++;
    }
    return 1;
}
static char *trim_end(char *s) {
    size_t l = my_strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == ' ' || s[l - 1] == '\r'))
        s[--l] = '\0';
    return s;
}

static char *fmt_ulong(char *buf, ulong v) {
    char tmp[16];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

static int find_debug_flag(char **env) {
    for (int i = 0; env[i]; i++)
        if (has_prefix(env[i], "INTERP_WRAPPER_DEBUG"))
            return 1;
    return 0;
}

int entry(int argc, char **argv) __attribute__((used, noinline));
int entry(int argc, char **argv) {
    char rootfs[4096];
    char loader[4096];
    char libdir[4608];
    char ldso[4704];
    char cfg[4096];
    char envbuf[65536];
    char *env_new[256];
    char *argv_new[512];
    char numbuf[16];

    if (argc < 1) {
        my_write_err("ld-linux-aarch64.so.1: no target executable\n");
        return 126;
    }
    /* The kernel invokes the interpreter with the ORIGINAL argv, so
     * argv[0] is just the program name.  The real path is AT_EXECFN in
     * the auxiliary vector (right after the envp on the stack).  */
    char **env = &argv[argc + 1]; /* argv[]..., NULL, env[]... */
    char **envp_end = env;
    while (*envp_end) envp_end++;
    char *execfn = NULL;
    for (unsigned long *av = (unsigned long *)(envp_end + 1);
         av[0] != 0; av += 2)
        if (av[0] == 31) { /* AT_EXECFN */
            execfn = (char *)av[1];
            break;
        }
    char *target = (execfn && execfn[0]) ? execfn : argv[0];

    my_strcpy(rootfs, "/data/adb/parrot");
    my_strcpy(loader, "/system/bin/elf_loader");
    int fd = my_open("/data/adb/parrot_root");
    if (fd >= 0) {
        ssize_t n = my_read(fd, cfg, sizeof cfg - 1);
        my_close(fd);
        if (n > 0) {
            cfg[n] = '\0';
            char *nl = cfg;
            char *eol = cfg;
            while (*eol && *eol != '\n') eol++;
            if (eol > nl) {
                *eol = '\0';
                trim_end(nl);
                my_strcpy(rootfs, nl);
                nl = eol + 1;
                if (*nl) {
                    eol = nl;
                    while (*eol && *eol != '\n') eol++;
                    *eol = '\0';
                    trim_end(nl);
                    my_strcpy(loader, nl);
                }
            }
        }
    }
    if (rootfs[0] == '\0')
        my_strcpy(rootfs, "/data/adb/parrot");
    if (loader[0] == '\0')
        my_strcpy(loader, "/system/bin/elf_loader");

    my_strcpy(libdir, rootfs);
    my_strcat(libdir, "/usr/lib/aarch64-linux-gnu");
    my_strcpy(ldso, libdir);
    my_strcat(ldso, "/ld-linux-aarch64.so.1");

    /* build new argv */
    if ((size_t)argc + 6 > sizeof argv_new / sizeof argv_new[0])
        return 127;
    size_t i = 0;
    argv_new[i++] = ldso;
    argv_new[i++] = "--library-path";
    argv_new[i++] = libdir;
    argv_new[i++] = loader;
    argv_new[i++] = "--ownall";
    argv_new[i++] = target; /* the main program the kernel exec'd */
    for (int k = 1; k < argc; k++)
        argv_new[i++] = argv[k];
    argv_new[i] = NULL;

    /* build new env: copy original, drop LD_LIBRARY_PATH, add ours, and
     * prepend the rootfs bin dirs to PATH so exec'd tools stay in parrot */
    char *ep = envbuf;
    size_t ei = 0;
    env_new[ei++] = NULL; /* slot 0: LD_LIBRARY_PATH below */
    env_new[ei++] = NULL; /* slot 1: PATH below */
    for (int k = 0; env[k] && k < 256; k++) {
        if (has_prefix(env[k], "LD_LIBRARY_PATH=") ||
            has_prefix(env[k], "PATH="))
            continue;
        size_t len = my_strlen(env[k]) + 1;
        if (ep + len > envbuf + sizeof envbuf)
            break;
        my_strcpy(ep, env[k]);
        env_new[ei++] = ep;
        ep += len;
        if (ei >= sizeof env_new / sizeof env_new[0] - 2)
            break;
    }
    /* insert LD_LIBRARY_PATH as env_new[0] */
    my_strcpy(ep, "LD_LIBRARY_PATH=");
    my_strcat(ep, libdir);
    env_new[0] = ep;
    ep += my_strlen(ep) + 1;
    /* insert PATH as env_new[1]: rootfs bins first, keep the rest */
    my_strcpy(ep, "PATH=");
    my_strcat(ep, rootfs);
    my_strcat(ep, "/usr/bin:");
    my_strcat(ep, rootfs);
    my_strcat(ep, "/bin:");
    for (int k = 0; env[k]; k++) {
        if (has_prefix(env[k], "PATH=")) {
            my_strcat(ep, env[k] + 5);
            break;
        }
    }
    env_new[1] = ep;
    ep += my_strlen(ep) + 1;
    ei += 2;
    if (ei < sizeof env_new / sizeof env_new[0])
        env_new[ei] = NULL;

    /* debug: dump the re-exec argv and stop */
    for (int d = 0; env[d]; d++)
        if (has_prefix(env[d], "INTERP_WRAPPER_DEBUG")) {
            for (size_t a = 0; argv_new[a]; a++) {
                my_write_err("[iw] argv[");
                my_write_err(fmt_ulong(numbuf, a));
                my_write_err("]=");
                my_write_err(argv_new[a]);
                my_write_err("\n");
            }
            return 0;
        }

    long r = syscall(SYS_execve, (long)ldso, (long)argv_new, (long)env_new, 0);
    my_write_err("ld-linux-aarch64.so.1: execve failed: errno=");
    my_write_err(fmt_ulong(numbuf, -r));
    my_write_err("\n");
    return 127;
}