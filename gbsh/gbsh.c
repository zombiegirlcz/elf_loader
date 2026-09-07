/* ═══════════════════════════════════════════════════════════════════════
 * gbsh — Ghost/Bionic Shell
 * ═══════════════════════════════════════════════════════════════════════
 * Nativní shell pro Android host (bionic, static-pie — žádné závislosti).
 *
 * Koncept:
 *   - builtiny běží přímo (cd/pwd/echo/export/alias/source/...)
 *   - HOST příkazy (/system/bin toybox) → fork+execvp
 *   - ROOTFS (parrot glibc) příkazy → fork+execve elf_loader --ownall
 *     (binárka se resolvne v $ROOTFS/{usr/bin,bin}; loader ji own-loadne)
 *
 * Config: ~/.gbshrc (nebo $GBSHRC) se provede při startu — stejný parser
 * jako interaktivní vstup (alias/export/PS1/...)
 *
 * Syntaxe: |  >  >>  <  &&  ||  ;  "quotes"  'quotes'  $VAR  ~/
 * ═══════════════════════════════════════════════════════════════════════ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mount.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/ioctl.h>

#define GBSH_VERSION "0.1"
#define MAX_LINE     8192
#define MAX_ARGS     128
#define MAX_PIPES    32
#define MAX_HISTORY  256
#define MAX_ALIAS    64

/* ─────────────────────────── globální stav ─────────────────────────── */

static char g_cwd[1024];
static char g_rootfs[512];
static char g_elfloader[600];      /* cesta k elf_loader binárce */
static int  g_running = 1;
static int  g_last_status = 0;

static char *g_history[MAX_HISTORY];
static int   g_hist_count = 0;

struct alias { char name[64]; char value[256]; };
static struct alias g_aliases[MAX_ALIAS];
static int g_alias_count = 0;

/* ═══ OBRÁCENÝ SVĚT (dual-world navigation) ═══
 * WORLD_ROOTFS: "/" = kořen distro (parrot) — příkazy běží přes ownall,
 *               cesty se fyzicky mapují pod $ROOTFS/
 * WORLD_HOST:   skutečný Android filesystem — příkazy běží nativně
 * cd .. z "/" (rootfs) tě překlopí na druhou stranu (host entry point),
 * cd $ROOTFS_SYMBOL (default "/parrot") v host světě tě vrátí dovnitř. */
#define WORLD_HOST  0
#define WORLD_ROOTFS 1
static int  g_world = WORLD_ROOTFS;
static int  g_dual_world = 0;
   /* --double-world / -dw */
static char g_vpath[1024];            /* virtuální cesta uvnitř rootfs ("/") */
static char g_host_entry[600];        /* kam ses dostane cd .. z "/" */

/* normalizuj vpath: zpracuj . / .. / opakování */
static void vpath_normalize(const char *in, char *out, size_t cap) {
    char stack[64][256];
    int sp = 0;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", in);
    char *save = NULL;
    for (char *seg = strtok_r(tmp, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) { if (sp > 0) sp--; continue; }
        if (sp < 64) snprintf(stack[sp++], 256, "%s", seg);
    }
    size_t o = 0;
    out[o++] = '/';
    for (int i = 0; i < sp && o + strlen(stack[i]) + 2 < cap; i++) {
        size_t l = strlen(stack[i]);
        memcpy(out + o, stack[i], l); o += l;
        if (i + 1 < sp) out[o++] = '/';
    }
    if (o > 1 && out[o-1] == '/') o--;
    out[o] = 0;
}

/* virtuální PWD podle světa */
static void get_vpwd(char *out, size_t cap) {
    if (g_world == WORLD_ROOTFS) snprintf(out, cap, "%s", g_vpath);
    else snprintf(out, cap, "%s", g_cwd);
}

/* ─────────────────────────── utility ─────────────────────────── */

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) { fprintf(stderr, "gbsh: OOM\n"); exit(1); }
    memcpy(p, s, n);
    return p;
}

static const char *env_or(const char *name, const char *def) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : def;
}

static int starts_with(const char *s, const char *pre) {
    return strncmp(s, pre, strlen(pre)) == 0;
}

/* ─────────────────────────── historie ─────────────────────────── */

static void hist_add(const char *line) {
    if (!line[0]) return;
    if (g_hist_count == MAX_HISTORY) {
        free(g_history[0]);
        memmove(&g_history[0], &g_history[1], sizeof(char *) * (MAX_HISTORY - 1));
        g_hist_count--;
    }
    g_history[g_hist_count++] = xstrdup(line);
}


#define MAX_FUNCS 64
struct shell_func {
    char name[64];
    char *body;
};
static struct shell_func g_funcs[MAX_FUNCS];
static int g_func_count = 0;

static char *g_positional_args[MAX_ARGS];
static int   g_positional_count = 0;

static const struct shell_func *func_lookup(const char *name) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, name) == 0)
            return &g_funcs[i];
    }
    return NULL;
}

static void func_set(const char *name, const char *body) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_funcs[i].name, name) == 0) {
            free(g_funcs[i].body);
            g_funcs[i].body = xstrdup(body);
            return;
        }
    }
    if (g_func_count < MAX_FUNCS) {
        snprintf(g_funcs[g_func_count].name, 64, "%s", name);
        g_funcs[g_func_count].body = xstrdup(body);
        g_func_count++;
    }
}

static int try_parse_func_header(const char *line, char *func_name, size_t name_cap, const char **body_start) {
    const char *p = line;
    while (isspace((unsigned char)*p)) p++;
    if (!*p || *p == '#') return 0;

    char name[64] = "";
    if (strncmp(p, "function ", 9) == 0) {
        p += 9;
        while (isspace((unsigned char)*p)) p++;
        size_t ni = 0;
        while ((isalnum((unsigned char)*p) || *p == '_' || *p == '-') && ni + 1 < sizeof name) {
            name[ni++] = *p++;
        }
        name[ni] = 0;
        while (isspace((unsigned char)*p)) p++;
        if (p[0] == '(' && p[1] == ')') { p += 2; while (isspace((unsigned char)*p)) p++; }
        if (*p == '{') {
            snprintf(func_name, name_cap, "%s", name);
            *body_start = p + 1;
            return 1;
        }
    } else {
        size_t ni = 0;
        const char *s = p;
        while ((isalnum((unsigned char)*s) || *s == '_' || *s == '-') && ni + 1 < sizeof name) {
            name[ni++] = *s++;
        }
        name[ni] = 0;
        if (ni > 0) {
            while (isspace((unsigned char)*s)) s++;
            if (s[0] == '(' && s[1] == ')') {
                s += 2;
                while (isspace((unsigned char)*s)) s++;
                if (*s == '{') {
                    snprintf(func_name, name_cap, "%s", name);
                    *body_start = s + 1;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* ─────────────────────────── alias ─────────────────────────── */

static const char *alias_lookup(const char *name) {
    for (int i = 0; i < g_alias_count; i++)
        if (strcmp(g_aliases[i].name, name) == 0)
            return g_aliases[i].value;
    return NULL;
}

static void alias_set(const char *name, const char *value) {
    for (int i = 0; i < g_alias_count; i++) {
        if (strcmp(g_aliases[i].name, name) == 0) {
            snprintf(g_aliases[i].value, sizeof(g_aliases[i].value), "%s", value);
            return;
        }
    }
    if (g_alias_count == MAX_ALIAS) {
        fprintf(stderr, "gbsh: alias table full\n");
        return;
    }
    snprintf(g_aliases[g_alias_count].name, 64, "%s", name);
    snprintf(g_aliases[g_alias_count].value, 256, "%s", value);
    g_alias_count++;
}

static void alias_remove(const char *name) {
    for (int i = 0; i < g_alias_count; i++) {
        if (strcmp(g_aliases[i].name, name) == 0) {
            memmove(&g_aliases[i], &g_aliases[i + 1],
                    sizeof(struct alias) * (g_alias_count - i - 1));
            g_alias_count--;
            return;
        }
    }
}

/* ─────────────────────────── $VAR expanze ─────────────────────────── */

/* do out (cap) zapíše line s expandovanými $VAR a ~/ */
static void expand_vars(const char *line, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; line[i] && o + 2 < cap; ) {
        if (line[i] == '\'' ) {           /* single quotes: no expansion */
            out[o++] = line[i++];
            while (line[i] && line[i] != '\'' && o + 2 < cap)
                out[o++] = line[i++];
            if (line[i]) out[o++] = line[i++];
            continue;
        }
        if (line[i] == '~' && (i == 0 || isspace((unsigned char)line[i-1]))) {
            const char *home = env_or("HOME", "/");
            size_t hl = strlen(home);
            if (o + hl + 2 >= cap) break;
            memcpy(out + o, home, hl); o += hl; i++;
            continue;
        }
        if (line[i] == '$') {
            if (isdigit((unsigned char)line[i+1])) {
                int idx = line[i+1] - '0';
                i += 2;
                if (idx == 0) {
                    const char *v = env_or("0", "gbsh");
                    size_t vl = strlen(v);
                    if (o + vl + 2 < cap) { memcpy(out + o, v, vl); o += vl; }
                } else if (idx <= g_positional_count && g_positional_args[idx - 1]) {
                    const char *v = g_positional_args[idx - 1];
                    size_t vl = strlen(v);
                    if (o + vl + 2 < cap) { memcpy(out + o, v, vl); o += vl; }
                }
                continue;
            }
            if (line[i+1] == '#') {
                i += 2;
                char num[16];
                snprintf(num, sizeof num, "%d", g_positional_count);
                size_t vl = strlen(num);
                if (o + vl + 2 < cap) { memcpy(out + o, num, vl); o += vl; }
                continue;
            }
            if (line[i+1] == '@' || line[i+1] == '*') {
                i += 2;
                for (int k = 0; k < g_positional_count; k++) {
                    if (!g_positional_args[k]) continue;
                    size_t vl = strlen(g_positional_args[k]);
                    if (o + vl + 2 < cap) {
                        memcpy(out + o, g_positional_args[k], vl);
                        o += vl;
                        if (k + 1 < g_positional_count && o + 2 < cap) out[o++] = ' ';
                    }
                }
                continue;
            }
        }
        if (line[i] == '$' && (isalpha((unsigned char)line[i+1]) || line[i+1]=='_')) {
            char name[128]; size_t ni = 0; i++;
            while ((isalnum((unsigned char)line[i]) || line[i]=='_') && ni+1 < sizeof name)
                name[ni++] = line[i++];
            name[ni] = 0;
            const char *v = getenv(name);
            if (!v) v = "";
            size_t vl = strlen(v);
            if (o + vl + 2 >= cap) break;
            memcpy(out + o, v, vl); o += vl;
            continue;
        }
        out[o++] = line[i++];
    }
    out[o] = 0;
}

/* ─────────────────────────── tokenizer ─────────────────────────── */

enum { T_WORD, T_PIPE, T_GT, T_GTGT, T_LT, T_ANDAND, T_OROR, T_SEMI, T_FDNUM };

struct token { int type; char text[512]; };

static int tokenize(const char *line, struct token *toks, int max_tok) {
    (void)max_tok;
    int nt = 0;
    size_t i = 0;
    while (line[i]) {
        while (isspace((unsigned char)line[i])) i++;
        if (!line[i]) break;
        if (nt >= max_tok - 1) break;
        char t = line[i];
        if (t == '|') {
            if (line[i+1] == '|') { toks[nt++]=(struct token){T_OROR, "||"}; i+=2; }
            else { toks[nt++]=(struct token){T_PIPE, "|"}; i++; }
            continue;
        }
        if (t == ';') { toks[nt++] = (struct token){T_SEMI, ";"}; i++; continue; }
        if (t == '>') {
            if (line[i+1] == '>') { toks[nt++]=(struct token){T_GTGT, ">>"}; i+=2; }
            else { toks[nt++]=(struct token){T_GT, ">"}; i++; }
            continue;
        }
        if (t == '<') { toks[nt++] = (struct token){T_LT, "<"}; i++; continue; }
        if (t == '&') {
            if (line[i+1] == '&') { toks[nt++]=(struct token){T_ANDAND,"&&"}; i+=2; continue; }
            /* lone & = background: zatím jako semi (foreground) */
            toks[nt++] = (struct token){T_SEMI, "&"}; i++; continue;
        }
        /* word — s uvozovkami */
        {
            char *w = toks[nt].text; size_t wi = 0;
            while (line[i] && !isspace((unsigned char)line[i])) {
                if (wi + 2 >= MAX_LINE) break;
                if (line[i] == '"') {
                    i++;
                    while (line[i] && line[i] != '"' && wi + 2 < MAX_LINE)
                        w[wi++] = line[i++];
                    if (line[i] == '"') i++;
                    continue;
                }
                if (line[i] == '\'') {
                    i++;
                    while (line[i] && line[i] != '\'' && wi + 2 < MAX_LINE)
                        w[wi++] = line[i++];
                    if (line[i] == '\'') i++;
                    continue;
                }
                /* operátory ukončují word */
                if (strchr("|;&<>", line[i])) {
                    if (wi == 0) break;
                    /* N> bez mezery (napr. 2>/dev/null) => fd marker */
                    int all_dig = 1;
                    for (size_t q = 0; q < wi; q++)
                        if (!isdigit((unsigned char)w[q])) { all_dig = 0; break; }
                    if (all_dig && line[i] == '>' && nt < max_tok - 2) {
                        w[wi] = 0;
                        toks[nt].type = T_FDNUM;
                        toks[nt].text[0] = '>';
                        toks[nt].text[1] = 0;
                        nt++;
                        break;
                    }
                    break;
                }
                w[wi++] = line[i++];
            }
            w[wi] = 0;
            if (wi > 0) toks[nt++].type = T_WORD;
        }
    }
    toks[nt].type = -1;
    return nt;
}

/* ─────────────────────────── resolve externích příkazů ─────────────────────────── */

enum { SRC_HOST, SRC_ROOTFS };

/* rozhodne kde binárka je; v ROOTFS světě má parrot VŽDY prioritu */
static int resolve_source(const char *cmd, char *rootfs_path, size_t rp_cap) {
    if (g_world == WORLD_ROOTFS) {
        if (!strchr(cmd, '/')) {
            static const char *rdirs[] = { "/usr/bin", "/bin", "/usr/sbin", "/sbin" };
            for (size_t i = 0; i < sizeof rdirs / sizeof rdirs[0]; i++) {
                snprintf(rootfs_path, rp_cap, "%s%s/%s", g_rootfs, rdirs[i], cmd);
                if (access(rootfs_path, X_OK) == 0) return SRC_ROOTFS;
            }
            /* cizí příkaz zkusíme taky hostem (nelžeme — není v distru) */
            return -1;
        }
        /* cesta: /X uvnitř rootfs = fyzicky $ROOTFS/X */
        if (cmd[0] == '/') {
            snprintf(rootfs_path, rp_cap, "%s%s", g_rootfs,
                     strcmp(cmd, "/") == 0 ? "" : cmd);
            if (access(rootfs_path, X_OK) == 0) return SRC_ROOTFS;
        } else {
            snprintf(rootfs_path, rp_cap, "%s", cmd); /* relativní už je fyzické */
            if (access(rootfs_path, X_OK) == 0) return SRC_ROOTFS;
        }
        return -1;
    }
    /* 1) absolutní/relativní cesta */
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0) return SRC_HOST;
        /* zkus v rootfs */
        if (cmd[0] == '/') {
            snprintf(rootfs_path, rp_cap, "%s%s", g_rootfs, cmd);
            if (access(rootfs_path, X_OK) == 0) return SRC_ROOTFS;
        }
        return SRC_HOST; /* necháme execvp vyhodit chybu */
    }

    /* 2) host PATH */
    {
        char pth[2048]; snprintf(pth, sizeof pth, "%s", env_or("PATH", "/system/bin"));
        char *save = NULL;
        for (char *d = strtok_r(pth, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
            char full[1200];
            snprintf(full, sizeof full, "%s/%s", d, cmd);
            if (access(full, X_OK) == 0) return SRC_HOST;
        }
    }

    /* 3) rootfs fallback jen v ROOTFS světě (host svět = čistý host) */
    if (g_world == WORLD_ROOTFS) {
        static const char *rdirs[] = { "/usr/bin", "/bin", "/usr/sbin", "/sbin" };
        for (size_t i = 0; i < sizeof rdirs / sizeof rdirs[0]; i++) {
            snprintf(rootfs_path, rp_cap, "%s%s/%s", g_rootfs, rdirs[i], cmd);
            if (access(rootfs_path, X_OK) == 0) return SRC_ROOTFS;
        }
    }
    return -1;
}

/* ─────────────────────────── builtins ─────────────────────────── */

static int enter_rootfs(const char *vpath) {
    char full[1200];
    const char *vp = (vpath && vpath[0]) ? vpath : "/";
    snprintf(full, sizeof full, "%s%s", g_rootfs, strcmp(vp, "/") == 0 ? "" : vp);
    if (chdir(full) != 0) return -1;
    g_world = WORLD_ROOTFS;
    vpath_normalize(vp, g_vpath, sizeof g_vpath);
    setenv("PWD", g_vpath, 1);
    return 0;
}

static int enter_host(const char *hpath) {
    if (chdir(hpath) != 0) return -1;
    g_world = WORLD_HOST;
    if (getcwd(g_cwd, sizeof g_cwd) == NULL) g_cwd[0] = 0;
    setenv("PWD", g_cwd, 1);
    return 0;
}

/* přepočet virtuální cesty v rootfs po relativním cd */
static int rootfs_relcd(const char *target, char *newvpath, size_t cap) {
    char combined[2048];
    if (target[0] == '/')
        snprintf(combined, sizeof combined, "%s", target);
    else
        snprintf(combined, sizeof combined, "%s/%s", g_vpath, target);
    vpath_normalize(combined, newvpath, cap);

    /* fyzická kontrola existence */
    char full[1200];
    snprintf(full, sizeof full, "%s%s", g_rootfs,
             strcmp(newvpath, "/") == 0 ? "" : newvpath);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    return chdir(full);
}

static int bi_cd(char **argv, int argc) {
    char targetbuf[MAX_LINE];
    const char *target;
    if (argc < 2) target = env_or("HOME", "/");
    else target = argv[1];

    /* ~ expanze */
    if (starts_with(target, "~")) {
        snprintf(targetbuf, sizeof targetbuf, "%s%s",
                 env_or("HOME", "/"), target + 1);
        target = targetbuf;
    }

    const char *sym = getenv("ROOTFS_SYMBOL");   /* volitelný alias */

    if (g_world == WORLD_ROOTFS) {
        /* fyzická cesta pod $ROOTFS prefixem = cesta dovnitř distra
           (např. cd $ROOTFS/usr/lib/...) → přepočet na virtuální vpath */
        size_t rflen = strlen(g_rootfs);
        if (strncmp(target, g_rootfs, rflen) == 0 &&
            (target[rflen] == 0 || target[rflen] == '/')) {
            const char *vp2 = target[rflen] ? target + rflen : "/";
            char nv[1024];
            vpath_normalize(vp2, nv, sizeof nv);
            char chk[1200];
            snprintf(chk, sizeof chk, "%s%s", g_rootfs,
                     strcmp(nv, "/") == 0 ? "" : nv);
            struct stat st;
            if (stat(chk, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (chdir(chk) != 0) {
                    fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
                    return 1;
                }
                snprintf(g_vpath, sizeof g_vpath, "%s", nv);
                setenv("PWD", g_vpath, 1);
                return 0;
            }
        }
        if (strcmp(target, "..") == 0 && strcmp(g_vpath, "/") == 0) {
            if (!g_dual_world) {
                /* single world: / je strop — zůstaneme */
                return 0;
            }
            /* ─── PŘEKLOP NA DRUHOU STRANU ───
             * host svět začíná ve FYZICKÉM RODIČI rootfs:
             *   $ROOTFS=/data/…/nh/distro/parrot  →  cd .. = /data/…/nh/distro
             * a odtud se dá chodit dál nahoru i dolů (do files, tmp, …) */
            char parent[1024];
            snprintf(parent, sizeof parent, "%s", g_rootfs);
            char *sl = strrchr(parent, '/');
            if (sl && sl != parent) *sl = 0;
            if (!sl || strcmp(parent, "") == 0) snprintf(parent, sizeof parent, "/");
            if (enter_host(parent) == 0) return 0;
            fprintf(stderr, "cd: cannot flip to host world\n");
            return 1;
        }
        char newvp[1024];
        if (rootfs_relcd(target, newvp, sizeof newvp) == 0) {
            snprintf(g_vpath, sizeof g_vpath, "%s", newvp);
            setenv("PWD", g_vpath, 1);
            return 0;
        }
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    /* HOST svět (reálný pouze v dual mode; jinak se tam nedostaneme).
     * Návrat dovnitř: cd $ROOTFS (fyzická cesta — univerzální, žádný
     * vymyšlený symbol). Jakákoli cesta uvnitř $ROOTFS prefixu tě přepne
     * do rootfs světa se správnou virtuální vpath. */
    if (strncmp(target, g_rootfs, strlen(g_rootfs)) == 0 &&
        (target[strlen(g_rootfs)] == 0 || target[strlen(g_rootfs)] == '/')) {
        const char *vpath = target + strlen(g_rootfs);
        if (enter_rootfs(vpath[0] ? vpath : "/") == 0) return 0;
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    if (!g_dual_world) {
        /* single world nemá host svět — cd funguje jen v rámci rootfs */
        char newvp[1024];
        snprintf(newvp, sizeof newvp, "%s", target);
        char nv[1024];
        vpath_normalize(newvp, nv, sizeof nv);
        char full[1200];
        snprintf(full, sizeof full, "%s%s", g_rootfs,
                 strcmp(nv, "/") == 0 ? "" : nv);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (chdir(full) == 0) {
                snprintf(g_vpath, sizeof g_vpath, "%s", nv);
                setenv("PWD", g_vpath, 1);
                return 0;
            }
        }
        fprintf(stderr, "cd: %s: %s\n", target,
                errno ? strerror(errno) : "Not a directory");
        return 1;
    }
    if (sym && strcmp(target, sym) == 0) {
        /* vstup do rootfs světa */
        if (enter_rootfs("/") == 0) return 0;
        fprintf(stderr, "cd: %s: %s\n", sym, strerror(errno));
        return 1;
    }
    if (enter_host(target) != 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    return 0;
}


static int check_autocd(char **argv, int argc) {
    if (argc != 1 || !argv[0] || !argv[0][0]) return -1;
    const char *target = argv[0];
    char targetbuf[MAX_LINE];
    if (starts_with(target, "~")) {
        snprintf(targetbuf, sizeof targetbuf, "%s%s", env_or("HOME", "/"), target + 1);
        target = targetbuf;
    }
    struct stat st;
    if (g_world == WORLD_ROOTFS) {
        char chk[2048];
        if (target[0] == '/') {
            snprintf(chk, sizeof chk, "%s%s", g_rootfs, strcmp(target, "/") == 0 ? "" : target);
        } else {
            char combined[2048];
            snprintf(combined, sizeof combined, "%s/%s", g_vpath, target);
            char nv[1024];
            vpath_normalize(combined, nv, sizeof nv);
            snprintf(chk, sizeof chk, "%s%s", g_rootfs, strcmp(nv, "/") == 0 ? "" : nv);
        }
        if (stat(chk, &st) == 0 && S_ISDIR(st.st_mode)) {
            return bi_cd(argv, argc);
        }
    } else {
        if (stat(target, &st) == 0 && S_ISDIR(st.st_mode)) {
            return bi_cd(argv, argc);
        }
    }
    return -1;
}

static int bi_pwd(char **argv, int argc) {
    (void)argv; (void)argc;
    char vp[1024];
    get_vpwd(vp, sizeof vp);
    printf("%s\n", vp);
    return 0;
}

static int bi_echo(char **argv, int argc) {
    int nflag = 0, start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { nflag = 1; start = 2; }
    for (int i = start; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc) putchar(' ');
    }
    if (!nflag) putchar('\n');
    return 0;
}

static int bi_export(char **argv, int argc) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = 0;
            setenv(argv[i], eq + 1, 1);
        } else {
            setenv(argv[i], "", 1);
        }
    }
    return 0;
}

static int bi_unset(char **argv, int argc) {
    for (int i = 1; i < argc; i++) unsetenv(argv[i]);
    return 0;
}

extern char **environ;

static int bi_env(char **argv, int argc) {
    (void)argv; (void)argc;
    for (char **e = environ; *e; e++)
        printf("%s\n", *e);
    return 0;
}

static int bi_exit(char **argv, int argc) {
    int code = argc > 1 ? atoi(argv[1]) : g_last_status;
    g_running = 0;
    return code;
}

static int bi_history(char **argv, int argc) {
    for (int i = 0; i < g_hist_count; i++)
        printf("%4d  %s\n", i + 1, g_history[i]);
    return 0;
}

static int bi_alias(char **argv, int argc) {
    if (argc == 1) {
        for (int i = 0; i < g_alias_count; i++)
            printf("alias %s='%s'\n", g_aliases[i].name, g_aliases[i].value);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = 0;
            char val[256];
            snprintf(val, sizeof val, "%s", eq + 1);
            /* odstraň obalující uvozovky */
            size_t l = strlen(val);
            if (l >= 2 && ((val[0]=='"' && val[l-1]=='"') || (val[0]=='\'' && val[l-1]=='\''))) {
                val[l-1] = 0;
                memmove(val, val + 1, l - 1);
            }
            alias_set(argv[i], val);
        } else {
            const char *v = alias_lookup(argv[i]);
            if (v) printf("alias %s='%s'\n", argv[i], v);
            else { fprintf(stderr, "alias: %s not found\n", argv[i]); return 1; }
        }
    }
    return 0;
}

static int bi_unalias(char **argv, int argc) {
    for (int i = 1; i < argc; i++) alias_remove(argv[i]);
    return 0;
}

static int bi_help(char **argv, int argc);

/* source / . — proveď soubor stejným parserem */
static int bi_source(char **argv, int argc);


int gbsh_eval_line(const char *raw);
static int word_is_builtin(const char *w);


struct command;
static int run_pipeline(struct command *cmds, int ncmd);
static int bi_type(char **argv, int argc);

struct command {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile, *outfile;
    int   append;
    int   out_fd;   /* cilovy fd pro output redirect (default 1; 2 = 2>) */
};

static int bi_command(char **argv, int argc) {
    if (argc < 2) return 0;
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "-V") == 0) {
        if (argc < 3) return 1;
        return bi_type(&argv[1], argc - 1);
    }
    struct command cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.out_fd = STDOUT_FILENO;
    cmd.argc = argc - 1;
    for (int i = 1; i < argc; i++) cmd.argv[i - 1] = argv[i];
    return run_pipeline(&cmd, 1);
}

static int bi_eval(char **argv, int argc) {
    if (argc < 2) return 0;
    char eval_buf[MAX_LINE * 4] = "";
    size_t pos = 0;
    for (int i = 1; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (pos + len + 2 < sizeof eval_buf) {
            memcpy(eval_buf + pos, argv[i], len);
            pos += len;
            if (i + 1 < argc) eval_buf[pos++] = ' ';
        }
    }
    eval_buf[pos] = 0;
    return gbsh_eval_line(eval_buf);
}

static int bi_noop(char **argv, int argc) {
    (void)argv; (void)argc;
    return 0;
}

static int bi_type(char **argv, int argc) {
    if (argc < 2) {
        fprintf(stderr, "type: missing argument\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *cmd = argv[i];
        if (word_is_builtin(cmd)) {
            printf("%s is a shell builtin\n", cmd);
            continue;
        }
        const struct shell_func *fn = func_lookup(cmd);
        if (fn) {
            printf("%s is a function\n%s ()\n{\n%s\n}\n", cmd, cmd, fn->body);
            continue;
        }
        const char *al = alias_lookup(cmd);
        if (al) {
            printf("%s is an alias for %s\n", cmd, al);
            continue;
        }
        char rfpath[1200];
        int src = resolve_source(cmd, rfpath, sizeof rfpath);
        if (src == SRC_ROOTFS) {
            printf("%s is %s\n", cmd, rfpath);
            continue;
        }
        if (src == SRC_HOST) {
            printf("%s is %s\n", cmd, cmd);
            continue;
        }
        fprintf(stderr, "type: %s: not found\n", cmd);
        status = 1;
    }
    return status;
}

static struct { const char *name; int (*fn)(char **, int); } builtins[] = {
    { "cd",       bi_cd },
    { "pwd",      bi_pwd },
    { "echo",     bi_echo },
    { "command",  bi_command },
    { "eval",     bi_eval },
    { "export",   bi_export },
    { "unset",    bi_unset },
    { "env",      bi_env },
    { "exit",     bi_exit },
    { "history",  bi_history },
    { "alias",    bi_alias },
    { "unalias",  bi_unalias },
    { "source",   bi_source },
    { ".",        bi_source },
    { "type",     bi_type },
    { "which",    bi_type },
    { "setopt",   bi_noop },
    { "unsetopt", bi_noop },
    { "bindkey",  bi_noop },
    { "zstyle",   bi_noop },
    { "compdef",  bi_noop },
    { "if",       bi_noop },
    { "then",     bi_noop },
    { "else",     bi_noop },
    { "elif",     bi_noop },
    { "fi",       bi_noop },
    { "for",      bi_noop },
    { "do",       bi_noop },
    { "done",     bi_noop },
    { "local",    bi_noop },
    { "help",     bi_help },
};

static int builtin_count = (int)(sizeof builtins / sizeof builtins[0]);

static int run_builtin(char **argv, int argc) {
    for (int i = 0; i < builtin_count; i++)
        if (strcmp(builtins[i].name, argv[0]) == 0)
            return builtins[i].fn(argv, argc);
    return -1; /* není builtin */
}

/* Kompletní nápověda — sdílená pro --help flag i help builtin */
static void print_usage(void) {
    printf("gbsh %s — nativní Android shell (bionic, bez závislostí)\n\n",
           GBSH_VERSION);
    printf("POUŽITÍ:\n");
    printf("  gbsh [FLAGY] [-c \"příkaz\" ]\n");
    printf("FLAGY:\n");
    printf("  -dw, --double-world  cd .. z rootfs / překlopí na fyzický Android,\n"
           "                       cd $ROOTFS se vrátí zpět\n");
    printf("  -c  \"příkaz\"         spustit příkaz a skončit s jeho statusem\n");
    printf("  --version            verze\n");
    printf("  --help               tato nápověda\n\n");
    printf("BUILTINY:");
    for (int i = 0; i < builtin_count; i++)
        printf(" %s", builtins[i].name);
    printf("\n\nSYNTAXE:\n");
    printf("  roura:      cmd1 | cmd2            sekvenční: a ; b   a && b   a || b\n");
    printf("  redirect:   cmd > f   cmd >> f   cmd < f   cmd N> f (fd form)\n");
    printf("  expanze:    $VAR  ${VAR}  ~/  ~  *  ?\n");
    printf("  uvozovky:   \"...\" expanduje, '...' literální\n\n");
    printf("PROSTŘEDÍ:\n");
    printf("  ROOTFS       cesta k distro rootfs (default: auto-detekce)\n");
    printf("  GBSHRC       config file (default ~/.gbshrc)\n");
    printf("  GBSH_PROMPT_MODE  '' | fancy | starship\n\n");
    printf("PŘÍKLADY:\n");
    printf("  gbsh                                  # interaktivní shell (own-loading)\n");
    printf("  gbsh -dw                              # + obrácený svět (cd .. z /)\n");
    printf("  gbsh -c \"uname -m | wc -c\"             # jeden příkaz, exit status propagován\n");

}

static int bi_help(char **argv, int argc) {
    (void)argv; (void)argc;
    print_usage();
    return 0;
}

/* ─────────────────────────── source ─────────────────────────── */

extern int gbsh_eval_line(const char *line);  /* definováno níže */

static int bi_source(char **argv, int argc) {
    if (argc < 2) {
        fprintf(stderr, "source: missing filename\n");
        return 1;
    }
    const char *target = argv[1];
    char targetbuf[MAX_LINE];
    if (starts_with(target, "~")) {
        snprintf(targetbuf, sizeof targetbuf, "%s%s", env_or("HOME", "/"), target + 1);
        target = targetbuf;
    }

    char openpath[1200];
    snprintf(openpath, sizeof openpath, "%s", target);

    if (g_world == WORLD_ROOTFS && target[0] == '/') {
        size_t rflen = strlen(g_rootfs);
        if (strncmp(target, g_rootfs, rflen) != 0) {
            snprintf(openpath, sizeof openpath, "%s%s", g_rootfs, strcmp(target, "/") == 0 ? "" : target);
        }
    }

    FILE *f = fopen(openpath, "r");
    if (!f && strcmp(openpath, target) != 0) {
        f = fopen(target, "r");
    }

    if (!f) {
        fprintf(stderr, "source: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    char line[MAX_LINE];
    int last = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (l && line[l-1] == '\n') line[l-1] = 0;
        last = gbsh_eval_line(line);
    }
    fclose(f);
    return last;
}

/* ─────────────────────────── externí příkazy ─────────────────────────── */


/* otevři soubor respektující svět: v rootfs světě se /X mapuje pod $ROOTFS */

static const char *get_exec_mode(const char *rootfs_path) {
    const char *env_mode = getenv("GBSH_EXEC_MODE");
    if (env_mode && env_mode[0]) {
        if (strcmp(env_mode, "ownall") == 0) return "--ownall";
        if (strcmp(env_mode, "run") == 0) return "--run";
        if (strcmp(env_mode, "own") == 0) return "--own";
        if (strcmp(env_mode, "shim") == 0) return "--shim";
    }
    return "--shim";
}

static int open_world(const char *path, int flags, int mode) {
    char full[1200];
    if (g_world == WORLD_ROOTFS && path[0] == '/') {
        snprintf(full, sizeof full, "%s%s", g_rootfs,
                 strcmp(path, "/") == 0 ? "" : path);
        path = full;
    }
    return open(path, flags, mode);
}

/* spustit parrot binárku přes elf_loader (ownall) */
static int exec_rootfs(const char *rootfs_path, char **argv) {
    const char *mode = get_exec_mode(rootfs_path);
    char *eargv[MAX_ARGS];
    int n = 0;
    eargv[n++] = (char *)g_elfloader;
    eargv[n++] = (char *)mode;
    eargv[n++] = (char *)rootfs_path;
    for (int i = 1; argv[i] && n < MAX_ARGS - 1; i++)
        eargv[n++] = argv[i];
    eargv[n] = NULL;
    execv(g_elfloader, eargv);
    fprintf(stderr, "gbsh: exec %s: %s\n", g_elfloader, strerror(errno));
    _exit(127);
}

static pid_t launch_external(char **argv, int src, const char *rootfs_path,
                             int stdin_fd, int stdout_src, int stdout_target) {
    pid_t pid = fork();
    if (pid < 0) { perror("gbsh: fork"); return -1; }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        /* PATH podle světa: rootfs děti vidí distro PATH, host děti systémovou */
        if (g_world == WORLD_ROOTFS) {
            setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
            /* elf_loader vlastní libc.so musí brát z /system (jinak GNU ld
               script z parrotu = bad ELF magic), parrot glibc libs až za tím
               pro INNER loader (libc.so.6 apod.) */
            {
                char ldp[1200];
                snprintf(ldp, sizeof ldp,
                         "/system/lib64:/system/lib:%s/usr/lib/aarch64-linux-gnu:%s/lib",
                         g_rootfs, g_rootfs);
                setenv("LD_LIBRARY_PATH", ldp, 1);
            }
        } else {
            setenv("PATH", "/system/bin:/system/xbin", 1);
        }
        if (stdin_fd != STDIN_FILENO)  { dup2(stdin_fd, STDIN_FILENO);  close(stdin_fd); }
        if (stdout_src >= 0 && stdout_src != stdout_target) {
            dup2(stdout_src, stdout_target);
            close(stdout_src);
        }
        if (stdout_src < 0 && stdout_target != STDOUT_FILENO)
            dup2(stdout_target, stdout_target); /* no-op guard */
        if (src == SRC_ROOTFS) exec_rootfs(rootfs_path, argv);
        execvp(argv[0], argv);
        fprintf(stderr, "gbsh: %s: %s\n", argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    return pid;
}

/* ─────────────────────────── pipeline / segmenty ─────────────────────────── */

/* jeden segment (= oddělený ; && ||): pole pipelines, každou tvoří commands */


static int wait_all(pid_t *pids, int count) {
    int status = 0;
    for (int i = 0; i < count; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        if (WIFEXITED(st)) status = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) status = 128 + WTERMSIG(st);
    }
    return status;
}

/* proveď jednu pipeline (commands spojené |) */

static int run_function(char **argv, int argc) {
    if (argc < 1 || !argv[0]) return -1;
    const struct shell_func *fn = func_lookup(argv[0]);
    if (!fn) return -1;

    char *old_args[MAX_ARGS];
    int old_count = g_positional_count;
    for (int i = 0; i < old_count; i++) old_args[i] = g_positional_args[i];

    g_positional_count = argc - 1;
    for (int i = 1; i < argc; i++) g_positional_args[i - 1] = argv[i];

    int status = gbsh_eval_line(fn->body);

    g_positional_count = old_count;
    for (int i = 0; i < old_count; i++) g_positional_args[i] = old_args[i];

    return status;
}

static int run_pipeline(struct command *cmds, int ncmd) {
    if (ncmd == 0) return g_last_status;

    /* čistě builtin nebo autocd bez pipe → spusť inline */
    if (ncmd == 1 && !cmds[0].infile && !cmds[0].outfile) {
        int bi = run_builtin(cmds[0].argv, cmds[0].argc);
        if (bi >= 0) return bi;
        int acd = check_autocd(cmds[0].argv, cmds[0].argc);
        if (acd >= 0) return acd;
        int fn = run_function(cmds[0].argv, cmds[0].argc);
        if (fn >= 0) return fn;
    }

    pid_t pids[MAX_PIPES];
    int prev_read = -1;
    int spawned = 0;

    for (int ci = 0; ci < ncmd; ci++) {
        struct command *c = &cmds[ci];
        int is_last = (ci == ncmd - 1);
        int pipefd[2] = { -1, -1 };

        if (!is_last && pipe(pipefd) < 0) { perror("pipe"); break; }

        /* builtin uprostřed/poslední pipeline → subprocess kvůli fd plumbing */
        int bi = (ncmd == 1) ? run_builtin(c->argv, c->argc) : -1;
        if (bi >= 0 && ncmd == 1) {
            /* builtin s redirecty: přepni fd v parentu, restore po akci */
            if (!c->infile && !c->outfile) return bi;
            int saved_out = -1, saved_in = -1;
            if (c->outfile) {
                int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
                int fd = open(c->outfile, flags, 0644);
                if (fd < 0) { perror(c->outfile); return 1; }
                saved_out = dup(STDOUT_FILENO);
                dup2(fd, c->out_fd);
                close(fd);
            }
            if (c->infile) {
                int fd = open(c->infile, O_RDONLY);
                if (fd < 0) { perror(c->infile); return 1; }
                saved_in = dup(STDIN_FILENO);
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            bi = run_builtin(c->argv, c->argc);
            fflush(stdout); fflush(stderr);
            if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
            if (saved_in >= 0)  { dup2(saved_in, STDIN_FILENO);  close(saved_in); }
            return bi;
        }

        char rfpath[1200];
        int src = -1;
        if (strstr(c->argv[0], "/")) {
            /* cesta — resolve podle existence */
            src = resolve_source(c->argv[0], rfpath, sizeof rfpath);
        } else {
            src = resolve_source(c->argv[0], rfpath, sizeof rfpath);
        }

        int in_fd = prev_read;
        int out_fd = -1;

        /* redirekce output */
        if (c->outfile) {
            int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
            out_fd = open_world(c->outfile, flags, 0644);
            if (out_fd < 0) { perror(c->outfile); return 1; }
            /* presmerovani na jiny fd nez 1 (napr. 2>): dup2 uvnitr child */
        } else if (!is_last) {
            out_fd = pipefd[1];
        }

        /* redirekce input */
        if (c->infile) {
            in_fd = open_world(c->infile, O_RDONLY, 0);
            if (in_fd < 0) { perror(c->infile); return 1; }
        }

        pid_t pid = launch_external(c->argv, src, rfpath, in_fd,
                                    out_fd, c->out_fd);
        /* poznamka: out_fd je docasny fd; dup2 na c->out_fd uvnitr child
           resi launch_external pres stdout_fd -> upraveno nize */
        if (in_fd > STDERR_FILENO)  close(in_fd);
        if (out_fd > STDERR_FILENO) close(out_fd);
        if (prev_read > STDERR_FILENO) close(prev_read);
        if (pipefd[1] > STDERR_FILENO) close(pipefd[1]);
        prev_read = pipefd[0];

        if (pid > 0) pids[spawned++] = pid;
        if (!is_last && pid <= 0) {
            if (prev_read > STDERR_FILENO) close(prev_read);
            break;
        }
    }
    if (prev_read > STDERR_FILENO) close(prev_read);
    return wait_all(pids, spawned);
}

/* ─────────────────────────── segmentace na ; && || ─────────────────────────── */

static int eval_tokens(struct token *toks, int nt) {
    /* rozděl na segmenty podle SEMI/ANDAND/OROR, aplikuj podmínky */
    int start = 0;
    int cond = 1; /* 1=spustit, 0=přeskočit (po && fail / || success) */
    int last_op = -1;

    for (int i = start; i <= nt; i++) {
        int is_end = (i == nt) || toks[i].type == T_SEMI ||
                     toks[i].type == T_ANDAND || toks[i].type == T_OROR;
        if (!is_end) continue;

        if (cond && i > start) {
            /* pipeline uvnitř [start, i) */
            struct command cmds[MAX_PIPES];
            memset(cmds, 0, sizeof cmds);
            for (int z = 0; z < MAX_PIPES; z++) cmds[z].out_fd = STDOUT_FILENO;
            int ncmd = 0, ai = 0;
            enum { S_ARG, S_IN, S_OUT } state = S_ARG;

            for (int j = start; j < i; j++) {
                struct token *tk = &toks[j];
                switch (tk->type) {
                case T_WORD: {
                    /* alias expanze prvního slova */
                    if (ai == 0 && state == S_ARG) {
                        const char *av = alias_lookup(tk->text);
                        if (av) {
                            struct token atoks[MAX_ARGS];
                            int an = tokenize(av, atoks, MAX_ARGS);
                            for (int k = 0; k < an && ai < MAX_ARGS - 1; k++) {
                                if (atoks[k].type == T_WORD)
                                    cmds[ncmd].argv[ai++] = xstrdup(atoks[k].text);
                            }
                            continue;
                        }
                    }
                    if (state == S_ARG && ai < MAX_ARGS - 1)
                        cmds[ncmd].argv[ai++] = xstrdup(tk->text);
                    else if (state == S_IN)   cmds[ncmd].infile  = xstrdup(tk->text);
                    else if (state == S_OUT)  cmds[ncmd].outfile = xstrdup(tk->text);
                    break;
                }
                case T_PIPE:
                    cmds[ncmd].argc = ai; ai = 0; state = S_ARG;
                    ncmd++;
                    break;
                case T_FDNUM:
                    /* N> — cilovy fd nasledujiciho output redirectu */
                    cmds[ncmd].out_fd = atoi(tk->text + 1);
                    break;
                case T_GT:
                    cmds[ncmd].append = 0;
                    state = S_OUT;
                    break;
                case T_GTGT:
                    cmds[ncmd].append = 1;
                    state = S_OUT;
                    break;
                case T_LT:  state = S_IN;  break;
                default: break;
                }
            }
            cmds[ncmd].argc = ai;
            ncmd++;

            g_last_status = run_pipeline(cmds, ncmd);

            /* úklid */
            for (int ci = 0; ci < ncmd; ci++) {
                for (int a = 0; a < cmds[ci].argc; a++) free(cmds[ci].argv[a]);
                free(cmds[ci].infile);
                free(cmds[ci].outfile);
            }
        }

        /* nastav podmínku pro další segment */
        if (i < nt) {
            if (toks[i].type == T_ANDAND)      cond = (g_last_status == 0);
            else if (toks[i].type == T_OROR)   cond = (g_last_status != 0);
            else                                cond = 1;
            start = i + 1;
        }
        (void)last_op;
    }
    return g_last_status;
}

/* ─────────────────────────── veřejný eval (pro source/rc) ─────────────────────────── */

int gbsh_eval_line(const char *raw) {
    char raw_copy[MAX_LINE];
    snprintf(raw_copy, sizeof raw_copy, "%s", raw);

    char *save = NULL;
    char *line = strtok_r(raw_copy, "\n", &save);
    int status = g_last_status;

    char in_func_name[64] = "";
    char func_body[MAX_LINE * 2] = "";
    int collecting_func = 0;

    while (line) {
        if (line[0]) {
            if (collecting_func) {
                const char *close_brace = strchr(line, '}');
                if (close_brace) {
                    size_t pre_len = close_brace - line;
                    strncat(func_body, line, pre_len);
                    func_set(in_func_name, func_body);
                    collecting_func = 0;
                    in_func_name[0] = 0;
                    func_body[0] = 0;
                } else {
                    strcat(func_body, line);
                    strcat(func_body, "\n");
                }
            } else {
                char fname[64];
                const char *bstart = NULL;
                if (try_parse_func_header(line, fname, sizeof fname, &bstart)) {
                    const char *close_brace = strchr(bstart, '}');
                    if (close_brace) {
                        char inline_body[MAX_LINE];
                        size_t blen = close_brace - bstart;
                        snprintf(inline_body, sizeof inline_body, "%.*s", (int)blen, bstart);
                        func_set(fname, inline_body);
                    } else {
                        snprintf(in_func_name, sizeof in_func_name, "%s", fname);
                        snprintf(func_body, sizeof func_body, "%s\n", bstart);
                        collecting_func = 1;
                    }
                } else {
                    char expanded[MAX_LINE];
                    expand_vars(line, expanded, sizeof expanded);
                    struct token toks[MAX_ARGS];
                    int nt = tokenize(expanded, toks, MAX_ARGS);
                    if (nt > 0) status = eval_tokens(toks, nt);
                }
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return status;
}


/* ════════════════════════ barevný výstup / line editor ═══════════════ */

#include <termios.h>

#define C_RESET  "\x1b[0m"
#define C_BOLD   "\x1b[1m"
#define C_DIM    "\x1b[2m"
#define C_GREEN  "\x1b[32m"
#define C_BGRN   "\x1b[1;32m"
#define C_YELLOW "\x1b[33m"
#define C_CYAN   "\x1b[36m"
#define C_MAGENTA "\x1b[35m"
#define C_RED    "\x1b[91m"
#define C_GRAY   "\x1b[90m"

static struct termios g_orig_termios;
static int g_raw_enabled = 0;
static int g_saved = 0;       /* uložena pozice startu promptu (\x1b[s / \x1b[u) */

static void raw_enable(void) {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0) return;
    g_orig_termios = t;
    t.c_lflag &= ~(ECHO | ICANON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) g_raw_enabled = 1;
}

static void raw_disable(void) {
    if (g_raw_enabled) { tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios); g_raw_enabled = 0; }
}

/* je slovo builtin? */
static int word_is_builtin(const char *w) {
    for (int i = 0; i < builtin_count; i++)
        if (strcmp(builtins[i].name, w) == 0) return 1;
    return 0;
}

/* vypsat buf[0..len) se syntax highlightem */
static void hl_emit(const char *buf, size_t len) {
    char out[MAX_LINE * 8];
    size_t o = 0;
#define EMITS(s) do { size_t l_ = strlen(s); if (o + l_ < sizeof out) { memcpy(out+o, s, l_); o += l_; } } while (0)
#define EMITC(c_) do { if (o + 2 < sizeof out) out[o++] = (c_); } while (0)

    int first_done = 0;
    size_t i = 0;
    while (i < len) {
        if (isspace((unsigned char)buf[i])) { EMITC(buf[i++]); continue; }

        /* komentar */
        if (buf[i] == '#' && (i == 0 || isspace((unsigned char)buf[i-1]))) {
            EMITS(C_GRAY);
            while (i < len) EMITC(buf[i++]);
            EMITS(C_RESET);
            break;
        }

        /* uvozovky */
        if (buf[i] == '"' || buf[i] == '\'') {
            char q = buf[i];
            EMITS(C_MAGENTA); EMITC(q); i++;
            while (i < len) {
                EMITC(buf[i]);
                if (buf[i] == q) { i++; break; }
                i++;
            }
            EMITS(C_RESET);
            continue;
        }

        /* operatory */
        if (strchr("|;&<>", buf[i])) {
            EMITS(C_YELLOW);
            while (i < len && strchr("|;&<>", buf[i])) EMITC(buf[i++]);
            EMITS(C_RESET);
            continue;
        }

        /* slovo */
        {
            size_t ws = i;
            int has_var = 0;
            while (i < len && !isspace((unsigned char)buf[i]) &&
                   !strchr("|;&<>", buf[i])) {
                if (buf[i] == '$') has_var = 1;
                i++;
            }
            size_t wl = i - ws;
            char w[256]; size_t cl = wl < sizeof w - 1 ? wl : sizeof w - 1;
            memcpy(w, buf + ws, cl); w[cl] = 0;

            const char *col = NULL;
            if (!first_done) {
                if (word_is_builtin(w)) col = C_BGRN;
                else if (wl > 2 && w[0] == '/' && access(w, X_OK) == 0) col = C_CYAN;
                else if (w[0] != '-' && w[0] != '.' && w[0] != '/' &&
                         access(w, X_OK) == 0) col = C_BGRN;
                first_done = 1;
            } else if (w[0] == '-') {
                col = C_CYAN;
            } else if (has_var) {
                col = C_RED;
            } else if (access(w, X_OK) == 0) {
                col = C_CYAN;
            }
            if (col) EMITS(col);
            for (size_t q = ws; q < i && o + 2 < sizeof out; q++) EMITC(buf[q]);
            if (col) EMITS(C_RESET);
        }
    }
    out[o] = 0;
#undef EMITS
#undef EMITC
    fputs(out, stdout);
}

/* ─────────────────────────── prompt ─────────────────────────── */

static void print_prompt_text(const char *ps1) {
    char vp[1024];
    get_vpwd(vp, sizeof vp);
    const char *show = vp;
    const char *home = (g_world == WORLD_ROOTFS) ? "" : env_or("HOME", "");
    char shortcwd[1100];
    if (home[0] && starts_with(show, home))
        snprintf(shortcwd, sizeof shortcwd, "~%s", show + strlen(home));
    else
        snprintf(shortcwd, sizeof shortcwd, "%s", show);

    /* dual mode: host svět označíme žlutým [host] prefixem */
    if (g_dual_world && g_world == WORLD_HOST)
        fputs("\x1b[33m[host]\x1b[0m ", stdout);

    for (const char *p = ps1; *p; p++) {
        /* interpretace escape sekvencí z gbshrc (\e \n \t \x1b ...) */
        if (p[0] == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'e': putchar('\033'); break;
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case '\\': putchar('\\'); break;
            case 'x': {
                int hv = 0; int nd = 0;
                while (nd < 2 && isxdigit((unsigned char)p[1])) {
                    char c = p[1]; p++;
                    int dv = isdigit((unsigned char)c) ? c - '0'
                            : (c | 32) - 'a' + 10;
                    hv = hv * 16 + dv; nd++;
                }
                putchar(hv ? hv : '\033');
                break;
            }
            default: putchar(*p); break;
            }
            continue;
        }
        if (p[0] == '%' && p[1]) {
            p++;
            switch (*p) {
            case 'u': fputs(env_or("USER", env_or("LOGNAME", "?")), stdout); break;
            case 'h': fputs(env_or("HOSTNAME", "android"), stdout); break;
            case '~': fputs(shortcwd, stdout); break;
            case '$': fputs(getuid() == 0 ? "#" : "$", stdout); break;
            default:  putchar(*p);
            }
        } else {
            putchar(*p);
        }
    }
    fflush(stdout);
}



/* ─────────────────────────── completion ─────────────────────────── */

/* doplň slovo; vrátí počet matchů, common prefix uloží do buf */
static int complete_word(char *buf, size_t *plen, size_t *pcur) {
    size_t len = *plen, cur = *pcur;
    /* najdi začátek aktuálního slova */
    size_t ws = cur;
    while (ws > 0 && !isspace((unsigned char)buf[ws-1])) ws--;
    char word[256];
    size_t wl = cur - ws;
    if (wl >= sizeof word) return 0;
    memcpy(word, buf + ws, wl); word[wl] = 0;

    char matches[MAX_ARGS][256];
    int nm = 0;

    int first_word = 1;
    for (size_t q = 0; q < ws; q++)
        if (!isspace((unsigned char)buf[q])) { first_word = 0; break; }

    if (!first_word || wl == 0 || word[0] == '/' || word[0] == '.') {
        /* souborová completion v CWD */
        char dir[1024]; const char *base = word;
        snprintf(dir, sizeof dir, ".");
        char *slash = strrchr(word, '/');
        if (slash) {
            size_t dl = slash - word;
            if (dl == 0) snprintf(dir, sizeof dir, "/");
            else { memcpy(dir, word, dl); dir[dl] = 0; }
            base = slash + 1;
        }
        DIR *d = opendir(dir);
        if (!d) return 0;
        struct dirent *de;
        size_t bl = strlen(base);
        while ((de = readdir(d)) && nm < MAX_ARGS) {
            if (strncmp(de->d_name, base, bl) == 0) {
                snprintf(matches[nm], 256, "%.*s%s",
                         (int)(slash ? slash - word + 1 : 0), word, de->d_name);
                nm++;
            }
        }
        closedir(d);
    } else {
        /* příkazová completion: builtins + host PATH + rootfs dirs */
        for (int i = 0; i < builtin_count && nm < MAX_ARGS; i++) {
            if (strcmp(builtins[i].name, word) == 0)
                snprintf(matches[nm++], 256, "%s", builtins[i].name);
        }
        const char *paths[] = {
            "/system/bin", "/system/xbin", "$ROOTFS/usr/bin", "$ROOTFS/bin"
        };
        for (size_t pi = 0; pi < sizeof paths / sizeof paths[0] && nm < MAX_ARGS; pi++) {
            char dir[600];
            if (starts_with(paths[pi], "$"))
                snprintf(dir, sizeof dir, "%s%s", g_rootfs, paths[pi] + 7);
            else
                snprintf(dir, sizeof dir, "%s", paths[pi]);
            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *de;
            size_t bl = strlen(word);
            while ((de = readdir(d)) && nm < MAX_ARGS) {
                char full[1200];
                snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
                if (strncmp(de->d_name, word, bl) == 0 &&
                    access(full, X_OK) == 0) {
                    snprintf(matches[nm++], 256, "%s", de->d_name);
                }
            }
            closedir(d);
        }
    }
    if (nm == 0) return 0;

    /* deduplikace */
    for (int i = 0; i < nm; i++)
        for (int j = i + 1; j < nm; j++)
            if (strcmp(matches[i], matches[j]) == 0) matches[j][0] = 0;

    /* common prefix */
    size_t cp = strlen(matches[0]);
    for (int i = 1; i < nm; i++) {
        if (matches[i][0] == 0) continue;
        size_t l = strlen(matches[i]);
        size_t k = 0;
        while (k < cp && k < l && matches[i][k] == matches[0][k]) k++;
        cp = k;
    }

    /* vlož common prefix místo slova */
    size_t pl = cp - wl;   /* kolik přidat */
    if (len + pl + 2 >= MAX_LINE) return nm;
    memmove(buf + cur + pl, buf + cur, len - cur);
    memcpy(buf + ws, matches[0], cp);
    cur = ws + cp; len += pl;
    *pcur = cur; *plen = len;

    /* jediný match → mezera; více → vypiš seznam */
    int uniq = 0;
    for (int i = 0; i < nm; i++) if (matches[i][0]) uniq++;
    if (uniq == 1) {
        memmove(buf + cur + 1, buf + cur, len - cur);
        buf[cur++] = ' '; len++;
    } else {
        fputc('\n', stdout);
        for (int i = 0; i < nm; i++)
            if (matches[i][0]) printf("%s  ", matches[i]);
        fputc('\n', stdout);
    }
    return nm;
}

/* ─────────────────────────── git branch pro prompt ─────────────────── */

static void get_git_branch(char *out, size_t cap) {
    out[0] = 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", g_cwd);
    for (;;) {
        char p[1200];
        snprintf(p, sizeof p, "%.900s/.git/HEAD", dir);
        FILE *f = fopen(p, "r");
        if (f) {
            char head[256] = "";
            if (fgets(head, sizeof head, f)) {
                head[strcspn(head, "\n")] = 0;
                const char *sl = strstr(head, "refs/heads/");
                snprintf(out, cap, "%s", sl ? sl + strlen("refs/heads/") : head);
            }
            fclose(f);
            return;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) { out[0] = 0; return; }
        *slash = 0;
    }
}

/* starship-style dvouřádkový prompt (nativní fallback) */
static void print_fancy_prompt(void) {
    char cwd[1024];
    const char *show = getcwd(cwd, sizeof cwd) ? cwd : "?";
    const char *home = env_or("HOME", "");
    char shortc[1100];
    if (home[0] && starts_with(show, home))
        snprintf(shortc, sizeof shortc, "~%s", show + strlen(home));
    else
        snprintf(shortc, sizeof shortc, "%s", show);

    char branch[128];
    get_git_branch(branch, sizeof branch);

    fputs("\x1b[1;34m", stdout);
    fputs(shortc, stdout);
    fputs("\x1b[0m", stdout);
    if (branch[0]) {
        fputs(" \x1b[1;32m\xe2\x8e\x87 ", stdout);   /* ⎇ */
        fputs(branch, stdout);
        fputs("\x1b[0m", stdout);
    }
    if (g_last_status != 0) {
        printf(" \x1b[1;91m\xe2\x9c\x97%d\x1b[0m", g_last_status);  /* ✗N */
    }
    fputs("\n\x1b[1;36m\xe2\x9d\xaf\x1b[0m ", stdout);          /* ❯ */
    fflush(stdout);
}

/* starship přes elf_loader (pokud funguje) — výstup do bufferu */
static int try_starship_prompt(void) {
    int fds[2];
    if (pipe(fds) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        freopen("/dev/null", "w", stderr);
        char path[600];
        snprintf(path, sizeof path, "%s/usr/bin/starship", g_rootfs);
        char *ea[MAX_ARGS];
        int n = 0;
        ea[n++] = (char *)g_elfloader;
        ea[n++] = (char *)"--ownall";
        ea[n++] = path;
        ea[n++] = (char *)"prompt";
        ea[n] = NULL;
        execv(g_elfloader, ea);
        _exit(127);
    }
    close(fds[1]);
    char tmp[2048];
    ssize_t n = read(fds[0], tmp, sizeof tmp - 1);
    close(fds[0]);
    int st; waitpid(pid, &st, 0);
    if (n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;
    tmp[n] = 0;
    fputs(tmp, stdout);
    return 0;
}

/* ─────────────────────────── line editor (raw mode) ─────────────────── */

static void print_prompt_raw(void);
static void print_prompt_text(const char *ps1);

/* šířka terminálu (sloupce) — přes ioctl, jinak $COLUMNS, jinak 80 */
static int get_term_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    const char *c = getenv("COLUMNS");
    if (c && atoi(c) > 0) return atoi(c);
    return 80;
}

/* viditelná šířka promptu (bez ANSI) — pro výpočet pozice kurzoru při zalomení */
static int prompt_display_width(void) {
    const char *ps1 = env_or("GBSH_PROMPT",
        "\x1b[1;32m%u@%h\x1b[0m:\x1b[36m%~\x1b[0m$ ");
    int w = 0;
    for (const char *p = ps1; *p; p++) {
        if (p[0] == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': w = 0; break;
            case 't': w += 8; break;
            case 'x': while (p[1] && isxdigit((unsigned char)p[1])) p++; break;
            default: break;
            }
            continue;
        }
        if (p[0] == '%' && p[1]) {
            p++;
            switch (*p) {
            case 'u': w += (int)strlen(env_or("USER", env_or("LOGNAME", "?"))); break;
            case 'h': w += (int)strlen(env_or("HOSTNAME", "android")); break;
            case '~': {
                char cwd[1024]; const char *show = getcwd(cwd, sizeof cwd) ? cwd : "?";
                const char *home = env_or("HOME", "");
                char sc[1100];
                if (home[0] && starts_with(show, home))
                    snprintf(sc, sizeof sc, "~%s", show + strlen(home));
                else snprintf(sc, sizeof sc, "%s", show);
                w += (int)strlen(sc);
                break;
            }
            case '$': w += 1; break;
            default: w += 1; break;
            }
            continue;
        }
        w += 1;
    }
    return w;
}

static void ed_render(const char *prompt, const char *buf, size_t len, size_t cur) {
    (void)prompt;
    int tw = get_term_cols();

    /* Vrátit kurzor na začátek promptu (uložená pozice) a smazat CELU
       oblast vstupu včetně zalomených řádků. To odstraňuje duplicity,
       které vznikaly, když \r\x1b[K smazalo jen jeden fyzický řádek. */
    if (g_saved) fputs("\x1b[u", stdout);   /* obnov start promptu */
    else         fputs("\r", stdout);         /* první překreslení: začátek řádku */
    fputs("\x1b[s", stdout);                  /* ulož start promptu pro příště */
    fputs("\x1b[J", stdout);                  /* smaž od startu promptu dolů */

    print_prompt_raw();
    hl_emit(buf, len);
    fputs("\x1b[0m", stdout);
    fputs("\x1b[K", stdout);                  /* umazat zbytek posledního řádku */

    if (cur < len) {
        /* přesuň kurzor zpět na cur s ohledem na zalomení řádku */
        int pw = prompt_display_width();
        int disp_cur = pw + (int)cur;
        int disp_end = pw + (int)len;
        int back_rows = (disp_end / tw) - (disp_cur / tw);
        int back_cols = (disp_end % tw) - (disp_cur % tw);
        if (back_cols < 0) { back_rows--; back_cols += tw; }
        if (back_rows > 0) printf("\x1b[%dA", back_rows);
        if (back_cols > 0) printf("\x1b[%dD", back_cols);
    }
    g_saved = 1;
    fflush(stdout);
}

/* interaktivní čtení s live syntax highlightem; NULL = EOF */
static char *read_line_interactive(void) {
    static char buf[MAX_LINE];
    size_t len = 0, cur = 0;
    int hist_idx = g_hist_count;
    int paste = 0;

    raw_enable();
    g_saved = 0;
    while (1) {
        ed_render("", buf, len, cur);
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) { raw_disable(); return NULL; }

        if (c == 27) {                          /* escape sequence */
            char e1, e2;
            if (read(STDIN_FILENO, &e1, 1) <= 0) continue;
            if (e1 != '[') continue;
            if (read(STDIN_FILENO, &e2, 1) <= 0) continue;
            if (e2 == '2') {                    /* bracketed paste 200~/201~ */
                char e3, e4, e5;
                if (read(STDIN_FILENO, &e3, 1) <= 0) continue;
                if (read(STDIN_FILENO, &e4, 1) <= 0) continue;
                if (read(STDIN_FILENO, &e5, 1) <= 0) continue;
                if (e5 == '~') {
                    if (e3 == '0' && e4 == '0') paste = 1;        /* 200~ start */
                    else if (e3 == '0' && e4 == '1') paste = 0;    /* 201~ end */
                }
                continue;
            }
            if (e2 == 'A') {                    /* Up: historie zpět */
                if (hist_idx > 0) {
                    hist_idx--;
                    snprintf(buf, MAX_LINE, "%s", g_history[hist_idx]);
                    len = strlen(buf); cur = len;
                }
                continue;
            }
            if (e2 == 'B') {                    /* Down */
                if (hist_idx < g_hist_count) {
                    hist_idx++;
                    if (hist_idx == g_hist_count) { len = cur = 0; }
                    else { snprintf(buf, MAX_LINE, "%s", g_history[hist_idx]); len = strlen(buf); cur = len; }
                }
                continue;
            }
            if (e2 == 'C') { if (cur < len) cur++; continue; }   /* Right */
            if (e2 == 'D') { if (cur > 0) cur--; continue; }     /* Left */
            if (e2 == 'H') { cur = 0; continue; }                /* Home */
            if (e2 == 'F') { cur = len; continue; }              /* End */
            char e3;
            if ((e2 == '1' || e2 == '4' || e2 == '3') && read(STDIN_FILENO, &e3, 1) > 0) {
                if (e2 == '1' && e3 == '~') { cur = 0; continue; }
                if (e2 == '4' && e3 == '~') { cur = len; continue; }
                if (e2 == '3' && e3 == '~') {                   /* Delete */
                    if (cur < len) { memmove(buf + cur, buf + cur + 1, len - cur - 1); len--; }
                    continue;
                }
            }
            continue;
        }

        if (paste) {                            /* vkládání: vše jako text */
            char in = c;
            if (in == '\r') in = '\n';
            if (in >= 32 && len + 1 < MAX_LINE) {
                memmove(buf + cur + 1, buf + cur, len - cur);
                buf[cur++] = in; len++;
            }
            continue;
        }

        if (c == '\r' || c == '\n') {          /* Enter */
            fputs("\n", stdout); fflush(stdout);
            raw_disable();
            buf[len] = 0;
            return xstrdup(buf);
        }
        if (c == 3) {                           /* Ctrl-C */
            fputs("^C\n", stdout); fflush(stdout);
            raw_disable();
            g_saved = 0;
            buf[0] = 0;
            return xstrdup("");
        }
        if (c == 4) {                           /* Ctrl-D na prázdném řádku */
            if (len == 0) { printf("exit\n"); raw_disable(); return NULL; }
            continue;
        }
        if (c == 12) {                          /* Ctrl-L clear */
            fputs("\x1b[2J\x1b[H", stdout);
            g_saved = 0;
            continue;
        }
        if (c == 1)  { cur = 0; continue; }     /* Ctrl-A: začátek řádku */
        if (c == 5)  { cur = len; continue; }   /* Ctrl-E: konec řádku */
        if (c == 11) { len = cur; continue; }   /* Ctrl-K: smazat do konce */
        if (c == 23) {                          /* Ctrl-W: smazat slovo */
            size_t p = cur;
            while (p > 0 && isspace((unsigned char)buf[p-1])) p--;
            while (p > 0 && !isspace((unsigned char)buf[p-1])) p--;
            memmove(buf + p, buf + cur, len - cur);
            len -= (cur - p); cur = p;
            continue;
        }
        if (c == '\t') {                        /* Tab: completion */
            int nmatches = complete_word(buf, &len, &cur);
            buf[len] = 0;
            (void)nmatches;
            continue;
        }
        if (c == 127 || c == 8) {               /* Backspace */
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur);
                cur--; len--;
            }
            continue;
        }
        if (c >= 32 && c != 127 && len + 1 < MAX_LINE) {   /* printable */
            memmove(buf + cur + 1, buf + cur, len - cur);
            buf[cur++] = c;
            len++;
        }
    }
}

static void print_prompt_raw(void) {
    /* barevný default; GBSH_PROMPT může obsahovat vlastní ANSI */
    const char *ps1 = env_or("GBSH_PROMPT", "\x1b[1;32m%u@%h\x1b[0m:\x1b[36m%~\x1b[0m$ ");
    print_prompt_text(ps1);
}

static void print_prompt(void) {
    print_prompt_raw();
}

/* ─────────────────────────── init ─────────────────────────── */

static void detect_env(void) {
    /* ROOTFS musí přijít z ENV (univerzální — žádná hardcoded cesta k aplikaci).
       Nastaví ho launcher (elroot) nebo uživatel:
         export ROOTFS=/cesta/k/distro   (např. parrot rootfs) */
    const char *rf = getenv("ROOTFS");
    if (!rf || !rf[0]) {
        fprintf(stderr, "gbsh: ROOTFS není nastaven — export ROOTFS=/cesta/k/distro "
                        "(např. parrot rootfs) a spusť znovu\n");
        exit(1);
    }
    snprintf(g_rootfs, sizeof g_rootfs, "%s", rf);
    /* host entry point: kam se dostaneš cd .. z "/" rootfs světa */
    if (!getenv("STARSHIP_CONFIG")) {
        char cfg[1200];
        snprintf(cfg, sizeof cfg, "%s/.config/starship.toml", env_or("HOME", "/"));
        if (access(cfg, R_OK) == 0) {
            setenv("STARSHIP_CONFIG", cfg, 1);
        } else {
            snprintf(cfg, sizeof cfg, "%s/root/.config/starship.toml", g_rootfs);
            if (access(cfg, R_OK) == 0) {
                setenv("STARSHIP_CONFIG", cfg, 1);
            } else {
                snprintf(cfg, sizeof cfg, "%s/etc/starship.toml", g_rootfs);
                if (access(cfg, R_OK) == 0) {
                    setenv("STARSHIP_CONFIG", cfg, 1);
                }
            }
        }
    }
    snprintf(g_host_entry, sizeof g_host_entry, "%s",
             env_or("HOST_ENTRY", env_or("HOME", "/")));

    const char *elf = getenv("ELF_LOADER");
    if (elf && elf[0])
        snprintf(g_elfloader, sizeof g_elfloader, "%s", elf);
    else {
        /* výchozí: elf_loader leží vedle rootfs (../usr/bin), jinak systemová cesta */
        snprintf(g_elfloader, sizeof g_elfloader, "%s/../usr/bin/elf_loader", g_rootfs);
        if (access(g_elfloader, X_OK) != 0)
            snprintf(g_elfloader, sizeof g_elfloader, "/system/bin/elf_loader");
    }
}

static void load_rc(void) {
    const char *rc = getenv("GBSHRC");
    char path[1200] = "";
    FILE *f = NULL;

    if (rc && rc[0]) {
        snprintf(path, sizeof path, "%s", rc);
        f = fopen(path, "r");
    } else {
        const char *candidates[4];
        char c0[1200], c1[1200], c2[1200], c3[1200];
        snprintf(c0, sizeof c0, "%s/.gbshrc", env_or("HOME", "/"));
        snprintf(c1, sizeof c1, "%s/root/.gbshrc", g_rootfs);
        snprintf(c2, sizeof c2, "%s/etc/gbshrc", g_rootfs);
        snprintf(c3, sizeof c3, "%s/etc/zsh/zshrc", g_rootfs);
        candidates[0] = c0;
        candidates[1] = c1;
        candidates[2] = c2;
        candidates[3] = c3;

        for (int i = 0; i < 4; i++) {
            f = fopen(candidates[i], "r");
            if (f) {
                snprintf(path, sizeof path, "%s", candidates[i]);
                setenv("GBSHRC", path, 0);
                break;
            }
        }
    }

    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (l && line[l-1] == '\n') line[l-1] = 0;
        if (line[0] == '#' || !line[0]) continue;
        gbsh_eval_line(line);
    }
    fclose(f);
}

static void sigint_handler(int sig) {
    (void)sig;
    /* nový prompt řádek */
    write(STDOUT_FILENO, "\n", 1);
}


/* výchozí barvy (pokud je uživatel nepřepsal v ~/.gbshrc)
   — gbsh spouští parrot ls/grep přes ownall, které barvy umí,
     jen chybí --color alias jako v běžném shellu */
static void set_default_aliases(void) {
    if (!alias_lookup("ls"))    alias_set("ls",   "ls --color=auto");
    if (!alias_lookup("grep"))  alias_set("grep", "grep --color=auto");
    if (!alias_lookup("fgrep")) alias_set("fgrep","fgrep --color=auto");
    if (!alias_lookup("egrep")) alias_set("egrep","egrep --color=auto");
    if (!alias_lookup("diff"))  alias_set("diff", "diff --color=auto");
}

int main(int argc, char **argv) {
    const char *cmd_string = NULL;
    /* CLI flags: --double-world/-dw, -c <command>, --version */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--double-world") == 0 ||
            strcmp(argv[i], "-dw") == 0)
            g_dual_world = 1;
        else if (strcmp(argv[i], "--shim") == 0 || strcmp(argv[i], "-s") == 0)
            setenv("GBSH_EXEC_MODE", "shim", 1);
        else if (strcmp(argv[i], "--ownall") == 0 || strcmp(argv[i], "-n") == 0)
            setenv("GBSH_EXEC_MODE", "ownall", 1);
        else if (strcmp(argv[i], "--run") == 0)
            setenv("GBSH_EXEC_MODE", "run", 1);
        else if (strcmp(argv[i], "--own") == 0)
            setenv("GBSH_EXEC_MODE", "own", 1);

        else if (strcmp(argv[i], "--version") == 0) {
            printf("gbsh %s\n", GBSH_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "gbsh: -c: chybí argument (příkaz)\n");
                return 2;
            }
            cmd_string = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] &&
                   strcmp(argv[i], "-c") != 0 &&
                   !(strcmp(argv[i], "-dw") == 0 ||
                     strcmp(argv[i], "--double-world") == 0)) {
            fprintf(stderr, "gbsh: unknown option: %s\n"
                    "usage: gbsh [--double-world|-dw] [-c command]\n", argv[i]);
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sigint_handler);
    signal(SIGQUIT, SIG_IGN);

    detect_env();
    if (getcwd(g_cwd, sizeof g_cwd) == NULL) g_cwd[0] = 0;
    setenv("SHELL", "gbsh", 1);
    /* startujeme uvnitř distro světa: / == rootfs
       (--double-world umožní cd .. z "/" překlopit na host)
       */
    if (enter_rootfs("/") != 0)
        fprintf(stderr, "gbsh: varování: rootfs %s nedostupné, startuji na hostu\n", g_rootfs);
    {
        char vn[32]; snprintf(vn, sizeof vn, "%s", GBSH_VERSION);
        setenv("GBSH_VERSION", vn, 0);
    }

    load_rc();
    set_default_aliases();

    /* gbsh -c <command>: spustit příkaz a skončit s jeho statusem.
       Rc se načítá (na rozdíl od bashe), aby měl příkaz k dispozici
       env/funkce/aliasy definované v ~/.gbshrc. */
    if (cmd_string) {
        gbsh_eval_line(cmd_string);
        return g_last_status;
    }

    const char *pmode = env_or("GBSH_PROMPT_MODE", "");
    int use_starship = strcmp(pmode, "starship") == 0;
    int use_fancy    = strcmp(pmode, "fancy") == 0;

    if (isatty(STDIN_FILENO)) {
        /* interaktivní režim: live syntax highlighting + historie */
        while (g_running) {
            if (use_starship)      { if (try_starship_prompt() != 0) print_fancy_prompt(); }
            else if (use_fancy)    print_fancy_prompt();
            else                   print_prompt_raw();
            char *line = read_line_interactive();
            if (!line) { printf("exit\n"); break; }
            hist_add(line);
            gbsh_eval_line(line);
            free(line);
        }
    } else {
        /* non-tty (pipe/skript) — žádné barvy ani raw mode */
        char *line = NULL;
        size_t cap = 0;
        while (g_running) {
            ssize_t n = getline(&line, &cap, stdin);
            if (n < 0) break;
            if (n > 0 && line[n-1] == '\n') line[n-1] = 0;
            hist_add(line);
            gbsh_eval_line(line);
        }
        free(line);
    }
    return g_last_status;
}
