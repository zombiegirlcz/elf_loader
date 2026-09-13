/* exec_shim.c — LD_PRELOAD whitelist exec-redirect shim for elf_loader.
 *
 * Purpose (this pass only):
 *   Hooks execve()/execv()/execvp()/execvpe()/execveat() and
 *   posix_spawn()/posix_spawnp() via dlsym(RTLD_NEXT, ...). On every call it
 *   checks the target binary's basename (or full path) against a static
 *   plain-text whitelist. If it matches, the call is re-routed through
 *   elf_loader so the guest glibc binary is own-loaded instead of exec'ed
 *   directly (direct exec of a glibc binary fails on Android: PT_INTERP
 *   /lib/ld-linux-aarch64.so.1 does not exist on the host). If it does NOT
 *   match, the call goes through to the real function completely unmodified.
 *
 * Build (glibc, against the parrot rootfs — this .so is loaded into a *guest*
 * glibc process such as zsh, never into the bionic elf_loader itself):
 *   aarch64-linux-gnu-gcc -shared -fPIC -O2 -o exec_shim.so exec_shim.c
 *
 * Environment:
 *   SHIM_WHITELIST   path to whitelist file (default: <dir-of-this-.so>/exec_shim.whitelist)
 *   SHIM_EXEC_MODE   elf_loader mode to use (default: --ownall)
 *   ELF_LOADER       path to elf_loader binary (default: /system/bin/elf_loader)
 *   SHIM_DEBUG=1     print decisions to stderr
 *
 * Note on envp: we MUST strip LD_PRELOAD / ELF_LOADER_PRELOAD from the envp we
 * hand to elf_loader, otherwise the bionic linker64 (which starts elf_loader)
 * would try to preload this glibc .so and abort with
 * "CANNOT LINK EXECUTABLE ... libc.so.6 not found". Everything else in the
 * environment is preserved.
 *
 * Out of scope (later pass): fallback-on-failure, auto whitelist writing,
 * hashing. This pass is whitelist-only, fail-through on non-match.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>

extern char **environ;

typedef int (*execve_fn)(const char *, char *const[], char *const[]);
typedef int (*execv_fn)(const char *, char *const[]);
typedef int (*execvp_fn)(const char *, char *const[]);
typedef int (*execvpe_fn)(const char *, char *const[], char *const[]);
typedef int (*execveat_fn)(int, const char *, char *const[], char *const[], int);
typedef int (*posix_spawn_fn)(pid_t *, const char *,
                              const posix_spawn_file_actions_t *,
                              const posix_spawnattr_t *,
                              char *const[], char *const[]);

static execve_fn      real_execve;
static execv_fn       real_execv;
static execvp_fn      real_execvp;
static execvpe_fn     real_execvpe;
static execveat_fn    real_execveat;
static posix_spawn_fn real_posix_spawn;
/* posix_spawnp is re-routed through real_posix_spawn after PATH resolution. */
static posix_spawn_fn real_posix_spawnp;

/* ── whitelist state ─────────────────────────────────────────────────────── */
#define SHIM_MAX_NAMES 64
#define SHIM_NAME_LEN  128
static char g_names[SHIM_MAX_NAMES][SHIM_NAME_LEN];
static int  g_nnames = 0;
static int  g_inited = 0;

static const char *g_loader = "/system/bin/elf_loader";
static const char *g_mode   = "--ownall";
static const char *g_rootfs = NULL;

static void shim_debug(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void shim_debug(const char *fmt, ...) {
    if (!getenv("SHIM_DEBUG")) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static const char *basename_of(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static int is_whitelisted(const char *p) {
    if (!p || !p[0]) return 0;
    const char *base = basename_of(p);
    for (int i = 0; i < g_nnames; i++) {
        if (strcmp(g_names[i], base) == 0) return 1;
        /* allow a full-path whitelist entry too */
        if (strchr(g_names[i], '/') && strcmp(g_names[i], p) == 0) return 1;
    }
    return 0;
}

static void load_whitelist(void) {
    if (g_inited) return;
    g_inited = 1;

    const char *wf = getenv("SHIM_WHITELIST");
    char default_path[PATH_MAX];
    if (!wf || !wf[0]) {
        /* default: next to this .so */
        Dl_info info;
        if (dladdr((void *)&load_whitelist, &info) && info.dli_fname) {
            snprintf(default_path, sizeof default_path, "%s", info.dli_fname);
            char *slash = strrchr(default_path, '/');
            if (slash)
                snprintf(slash + 1, sizeof default_path - (size_t)(slash + 1 - default_path),
                         "exec_shim.whitelist");
            else
                snprintf(default_path, sizeof default_path, "exec_shim.whitelist");
        } else {
            snprintf(default_path, sizeof default_path, "exec_shim.whitelist");
        }
        wf = default_path;
    }

    FILE *f = fopen(wf, "r");
    if (!f) { shim_debug("[exec_shim] whitelist not found: %s\n", wf); return; }

    char line[256];
    while (fgets(line, sizeof line, f) && g_nnames < SHIM_MAX_NAMES) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        char *end = s + strlen(s);
        while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = 0;
        if (!*s) continue;
        snprintf(g_names[g_nnames++], SHIM_NAME_LEN, "%s", s);
    }
    fclose(f);
    shim_debug("[exec_shim] whitelist loaded (%d name(s)) from %s\n", g_nnames, wf);
}

__attribute__((constructor)) static void shim_ctor(void) {
    shim_debug("[exec_shim] constructor ran (LD_PRELOAD loaded into this process)\n");
}

static void ensure_init(void) {
    if (real_execve) return;
    real_execve   = (execve_fn)dlsym(RTLD_NEXT, "execve");
    real_execv    = (execv_fn)dlsym(RTLD_NEXT, "execv");
    real_execvp   = (execvp_fn)dlsym(RTLD_NEXT, "execvp");
    real_execvpe  = (execvpe_fn)dlsym(RTLD_NEXT, "execvpe");
    real_execveat = (execveat_fn)dlsym(RTLD_NEXT, "execveat");
    real_posix_spawn = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    real_posix_spawnp = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawnp");

    const char *l = getenv("ELF_LOADER");
    if (l && l[0]) g_loader = l;
    const char *m = getenv("SHIM_EXEC_MODE");
    if (m && m[0]) g_mode = m;
    g_rootfs = getenv("ROOTFS");

    load_whitelist();
    shim_debug("[exec_shim] init loader=%s mode=%s\n", g_loader, g_mode);
}

/* Resolve a guest path (as seen by the guest process) to a device path that
 * elf_loader can open. search_path=1 for execvp/posix_spawnp semantics. */
static int resolve_target(const char *p, char *out, size_t cap, int search_path) {
    size_t rl = g_rootfs ? strlen(g_rootfs) : 0;

    if (strchr(p, '/')) {
        if (p[0] == '/') {
            if (rl && strncmp(p, g_rootfs, rl) == 0 && (p[rl] == '/' || p[rl] == 0)) {
                snprintf(out, cap, "%s", p);          /* already device path */
            } else if (rl) {
                snprintf(out, cap, "%s%s", g_rootfs, p);   /* guest-absolute */
            } else {
                snprintf(out, cap, "%s", p);
            }
        } else {
            snprintf(out, cap, "%s", p);              /* relative: device-relative */
        }
        return 1;
    }

    /* bare name */
    if (search_path) {
        const char *path_env = getenv("PATH");
        char *pc = path_env ? strdup(path_env) : NULL;
        if (pc) {
            for (char *dir = strtok(pc, ":"); dir; dir = strtok(NULL, ":")) {
                char cand[PATH_MAX];
                if (dir[0] == '/') {
                    if (rl && strncmp(dir, g_rootfs, rl) != 0)
                        snprintf(cand, sizeof cand, "%s%s/%s", g_rootfs, dir, p);
                    else
                        snprintf(cand, sizeof cand, "%s/%s", dir, p);
                } else {
                    snprintf(cand, sizeof cand, "%s/%s", dir, p);
                }
                if (access(cand, X_OK) == 0) {
                    snprintf(out, cap, "%s", cand);
                    free(pc);
                    return 1;
                }
            }
            free(pc);
        }
    }

    /* fallback: assume $ROOTFS/usr/bin/<name> */
    if (rl) { snprintf(out, cap, "%s/usr/bin/%s", g_rootfs, p); return 1; }
    snprintf(out, cap, "%s", p);
    return 1;
}

/* Build [ELF_LOADER, MODE, resolved, argv[1]...]. Caller frees (except after
 * execve, which replaces the image). */
static char **build_loader_argv(const char *resolved, char *const argv[]) {
    int argc = 0;
    while (argv && argv[argc]) argc++;
    char **na = malloc((size_t)(argc + 3) * sizeof(char *));
    if (!na) return NULL;
    int n = 0;
    na[n++] = (char *)g_loader;
    na[n++] = (char *)g_mode;
    na[n++] = (char *)resolved;
    for (int i = 1; i < argc; i++) na[n++] = argv[i];
    na[n] = NULL;
    return na;
}

/* Copy envp but drop the preload machinery so the bionic elf_loader (and any
 * exec'd glibc child) does not try to load this glibc .so itself. */
static char **env_without_preload(char *const envp[]) {
    char *const *e = envp ? envp : (char *const *)environ;
    int n = 0;
    for (int i = 0; e[i]; i++) n++;

    char **out = malloc((size_t)(n + 1) * sizeof(char *));
    if (!out) return (char **)e;   /* OOM: keep original (best effort) */

    int o = 0;
    for (int i = 0; e[i]; i++) {
        if (strncmp(e[i], "LD_PRELOAD=", 11) == 0) continue;
        if (strncmp(e[i], "ELF_LOADER_PRELOAD=", 19) == 0) continue;
        out[o++] = e[i];
    }
    out[o] = NULL;
    return out;
}

static int redirect_exec(const char *path, char *const argv[], char *const envp[],
                         int search_path) {
    char resolved[PATH_MAX];
    if (!is_whitelisted(path) || !resolve_target(path, resolved, sizeof resolved, search_path))
        return 0;   /* 0 = not redirected */

    char **na = build_loader_argv(resolved, argv);
    if (!na) return -1;
    char **ne = env_without_preload(envp);

    shim_debug("[exec_shim] redirect %s -> %s %s %s\n",
               path, g_loader, g_mode, resolved);

    /* execve replaces the image on success; on failure return errno semantics. */
    real_execve(g_loader, na, ne);
    int saved = errno;
    free(na);
    free(ne);
    errno = saved;
    return -1;   /* -1 = redirected and failed */
}

/* ── exec family ─────────────────────────────────────────────────────────── */

int execve(const char *path, char *const argv[], char *const envp[]) {
    ensure_init();
    if (path && is_whitelisted(path)) {
        int r = redirect_exec(path, argv, envp, 0);
        if (r == -1) return -1;
    }
    return real_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    ensure_init();
    if (file && is_whitelisted(file)) {
        int r = redirect_exec(file, argv, environ, 1);
        if (r == -1) return -1;
    }
    return real_execvp(file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    ensure_init();
    if (file && is_whitelisted(file)) {
        int r = redirect_exec(file, argv, envp, 1);
        if (r == -1) return -1;
    }
    return real_execvpe(file, argv, envp);
}

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

int execveat(int dirfd, const char *path, char *const argv[], char *const envp[], int flags) {
    ensure_init();
    if (path && is_whitelisted(path)) {
        int r = redirect_exec(path, argv, envp, 0);
        if (r == -1) return -1;
    }
    return real_execveat(dirfd, path, argv, envp, flags);
}

/* ── posix_spawn family ──────────────────────────────────────────────────── */

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    ensure_init();
    if (!(path && is_whitelisted(path)))
        return real_posix_spawn(pid, path, fa, attr, argv, envp);

    char resolved[PATH_MAX];
    if (!resolve_target(path, resolved, sizeof resolved, 0))
        return real_posix_spawn(pid, path, fa, attr, argv, envp);

    char **na = build_loader_argv(resolved, argv);
    if (!na) return real_posix_spawn(pid, path, fa, attr, argv, envp);
    char **ne = env_without_preload(envp);

    shim_debug("[exec_shim] posix_spawn redirect %s -> %s %s %s\n",
               path, g_loader, g_mode, resolved);

    int r = real_posix_spawn(pid, g_loader, fa, attr, na, ne);
    free(na);
    free(ne);
    return r;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
    ensure_init();
    if (!(file && is_whitelisted(file)))
        return real_posix_spawnp(pid, file, fa, attr, argv, envp);
    /* posix_spawnp resolves via PATH like execvp; re-use that path logic then
     * call real posix_spawn (not spawnp) with the resolved loader target. */
    char resolved[PATH_MAX];
    if (!resolve_target(file, resolved, sizeof resolved, 1))
        return real_posix_spawn(pid, file, fa, attr, argv, envp);

    char **na = build_loader_argv(resolved, argv);
    if (!na) return real_posix_spawn(pid, file, fa, attr, argv, envp);
    char **ne = env_without_preload(envp);

    shim_debug("[exec_shim] posix_spawnp redirect %s -> %s %s %s\n",
               file, g_loader, g_mode, resolved);

    int r = real_posix_spawn(pid, g_loader, fa, attr, na, ne);
    free(na);
    free(ne);
    return r;
}
