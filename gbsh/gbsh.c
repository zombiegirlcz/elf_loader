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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

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

/* rozhodne kde binárka je: rootfs (parrot) má prioritu jen pokud host nemá */
static int resolve_source(const char *cmd, char *rootfs_path, size_t rp_cap) {
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

    /* 3) rootfs bin, usr/bin */
    static const char *rdirs[] = { "/usr/bin", "/bin", "/usr/sbin", "/sbin" };
    for (size_t i = 0; i < sizeof rdirs / sizeof rdirs[0]; i++) {
        snprintf(rootfs_path, rp_cap, "%s%s/%s", g_rootfs, rdirs[i], cmd);
        if (access(rootfs_path, X_OK) == 0) return SRC_ROOTFS;
    }
    return -1;
}

/* ─────────────────────────── builtins ─────────────────────────── */

static int bi_cd(char **argv, int argc) {
    const char *target;
    if (argc < 2) target = env_or("HOME", "/");
    else target = argv[1];
    if (chdir(target) != 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    if (getcwd(g_cwd, sizeof g_cwd) == NULL)
        g_cwd[0] = 0;
    return 0;
}

static int bi_pwd(char **argv, int argc) {
    (void)argv; (void)argc;
    printf("%s\n", getcwd(g_cwd, sizeof g_cwd) ? g_cwd : "?");
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

static struct { const char *name; int (*fn)(char **, int); } builtins[] = {
    { "cd",      bi_cd },
    { "pwd",     bi_pwd },
    { "echo",    bi_echo },
    { "export",  bi_export },
    { "unset",   bi_unset },
    { "env",     bi_env },
    { "exit",    bi_exit },
    { "history", bi_history },
    { "alias",   bi_alias },
    { "unalias", bi_unalias },
    { "source",  bi_source },
    { ".",       bi_source },
    { "help",    bi_help },
};

static int builtin_count = (int)(sizeof builtins / sizeof builtins[0]);

static int run_builtin(char **argv, int argc) {
    for (int i = 0; i < builtin_count; i++)
        if (strcmp(builtins[i].name, argv[0]) == 0)
            return builtins[i].fn(argv, argc);
    return -1; /* není builtin */
}

static int bi_help(char **argv, int argc) {
    (void)argv; (void)argc;
    printf("gbsh %s — native android shell\n\n", GBSH_VERSION);
    printf("builtins:");
    for (int i = 0; i < builtin_count; i++) printf(" %s", builtins[i].name);
    printf("\n\nsyntax: cmd | cmd > f  >> f  < f  &&  ||  ;\n");
    printf("rootfs (parrot) prikazy se spousti pres elf_loader automaticky.\n");
    printf("config: ~/.gbshrc\n");
    return 0;
}

/* ─────────────────────────── source ─────────────────────────── */

extern int gbsh_eval_line(const char *line);  /* definováno níže */

static int bi_source(char **argv, int argc) {
    if (argc < 2) {
        fprintf(stderr, "source: missing filename\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "r");
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

/* spustit parrot binárku přes elf_loader (ownall) */
static int exec_rootfs(const char *rootfs_path, char **argv) {
    char *eargv[MAX_ARGS];
    int n = 0;
    eargv[n++] = (char *)g_elfloader;
    eargv[n++] = (char *)"--ownall";
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
struct command {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile, *outfile;
    int   append;
    int   out_fd;   /* cilovy fd pro output redirect (default 1; 2 = 2>) */
};

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
static int run_pipeline(struct command *cmds, int ncmd) {
    if (ncmd == 0) return g_last_status;

    /* čistě builtin bez pipe → spusť inline (aby cd/export měly efekt) */
    if (ncmd == 1 && !cmds[0].infile && !cmds[0].outfile) {
        int bi = run_builtin(cmds[0].argv, cmds[0].argc);
        if (bi >= 0) return bi;
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
            out_fd = open(c->outfile, flags, 0644);
            if (out_fd < 0) { perror(c->outfile); return 1; }
            /* presmerovani na jiny fd nez 1 (napr. 2>): dup2 uvnitr child */
        } else if (!is_last) {
            out_fd = pipefd[1];
        }

        /* redirekce input */
        if (c->infile) {
            in_fd = open(c->infile, O_RDONLY);
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
    char expanded[MAX_LINE];
    expand_vars(raw, expanded, sizeof expanded);

    struct token toks[MAX_ARGS];
    int nt = tokenize(expanded, toks, MAX_ARGS);
    if (nt == 0) return g_last_status;
    return eval_tokens(toks, nt);
}

/* ─────────────────────────── prompt ─────────────────────────── */

static void print_prompt(void) {
    const char *ps1 = env_or("GBSH_PROMPT", "%u@%h:%~ $ ");
    char cwd[1024];
    const char *show = getcwd(cwd, sizeof cwd) ? cwd : "?";
    const char *home = env_or("HOME", "");
    char shortcwd[1100];
    if (home[0] && starts_with(show, home))
        snprintf(shortcwd, sizeof shortcwd, "~%s", show + strlen(home));
    else
        snprintf(shortcwd, sizeof shortcwd, "%s", show);

    for (const char *p = ps1; *p; p++) {
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

/* ─────────────────────────── init ─────────────────────────── */

static void detect_env(void) {
    const char *rf = getenv("ROOTFS");
    if (!rf || !rf[0]) rf = "/data/user/0/com.linux_core/files/nh/distro/parrot";
    snprintf(g_rootfs, sizeof g_rootfs, "%s", rf);

    const char *elf = getenv("ELF_LOADER");
    if (elf && elf[0])
        snprintf(g_elfloader, sizeof g_elfloader, "%s", elf);
    else {
        const char *home = env_or("HOME", "/data/data/com.linux_core/files");
        snprintf(g_elfloader, sizeof g_elfloader, "%s/usr/bin/elf_loader", home);
        if (access(g_elfloader, X_OK) != 0)
            snprintf(g_elfloader, sizeof g_elfloader, "/system/bin/elf_loader");
    }
}

static void load_rc(void) {
    const char *rc = getenv("GBSHRC");
    char path[600];
    if (rc && rc[0]) {
        snprintf(path, sizeof path, "%s", rc);
    } else {
        snprintf(path, sizeof path, "%s/.gbshrc", env_or("HOME", "/"));
    }
    FILE *f = fopen(path, "r");
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

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sigint_handler);
    signal(SIGQUIT, SIG_IGN);

    detect_env();
    if (getcwd(g_cwd, sizeof g_cwd) == NULL) g_cwd[0] = 0;
    setenv("SHELL", "gbsh", 1);
    {
        char vn[32]; snprintf(vn, sizeof vn, "%s", GBSH_VERSION);
        setenv("GBSH_VERSION", vn, 0);
    }

    load_rc();

    char *line = NULL;
    size_t cap = 0;

    while (g_running) {
        print_prompt();
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {                       /* EOF (Ctrl-D) */
            printf("exit\n");
            break;
        }
        if (n > 0 && line[n-1] == '\n') line[n-1] = 0;
        hist_add(line);
        gbsh_eval_line(line);
    }
    free(line);
    return g_last_status;
}
