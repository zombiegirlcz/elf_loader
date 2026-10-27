/* src/relsymtool.h — `elf_loader relsym`: jednorazova konverze absolutnich
 * symlinku v ROOTFS na kanonicky relativni tvar.
 *
 * Proc: jadro vyhodnocuje absolutni symlink uvnitr ROOTFS proti HOST koreni,
 * takze /usr/share/nikto -> /var/lib/nikto miri na host /var/lib/nikto
 * (neexistuje), i kdyz cil v rootfs existuje. Invariant "linky v rootfs jsou
 * relativni v kanonickem tvaru" drzi za behu shim_symlink/shim_symlinkat
 * (src/relsym.h); tenhle nastroj dela to same pro linky, ktere uz existuji.
 *
 * Bezi v HOST kontextu loaderu (pred dispatchem do guesta), takze pouziva
 * bezne POSIX volani host libc - zadny shim, zadny guest TP.
 *
 *   elf_loader relsym [--root <cesta>] [--dry-run|--check]
 *                     [--log <soubor>] [--quiet]
 *   elf_loader relsym --revert <zurnal>
 *
 * Tentyz algoritmus jako tools/relsymlinks.sh (a stejne seznamy vyjimek),
 * jen bez zavislosti na GNU find -printf a bez forku na kazdy link. */
#ifndef ELF_LOADER_RELSYMTOOL_H
#define ELF_LOADER_RELSYMTOOL_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

static int rs_quiet = 0;
static long rs_total = 0, rs_conv = 0, rs_hostskip = 0, rs_rootfsskip = 0;
/* Prefix rootfs pro rozliseni "cil uz je hostove-absolutni pod ROOTFS". */
static const char *rs_pref = "";

static void rs_say(const char *fmt, ...) {
    if (rs_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

/* Cile, ktere MUSI zustat absolutni: host mountpointy a virtualni FS.
 * Presna kopie seznamu ze shim_excluded() v src/main.c - kdyby se oba
 * seznamy rozesly, konverze by prepsala neco, co loader pak preklada jinak. */
static int rs_is_host_target(const char *t) {
    static const char *excl[] = {
        "/proc", "/dev", "/sys", "/system", "/data", "/apex", "/linkerconfig",
        "/metadata", "/sdcard", "/storage", "/vendor", "/odm", "/product",
        "/persist", "/cache", "/config", "/debug_ramdisk", NULL
    };
    for (int i = 0; excl[i]; i++) {
        size_t l = strlen(excl[i]);
        if (strncmp(t, excl[i], l) == 0 && (t[l] == '/' || t[l] == 0))
            return 1;
    }
    return 0;
}

/* Adresare, do kterych vubec nelezeme (virtualni FS + host bind mounty).
 *
 * ".l2s" je interni adresar L2S (link2symlink): obsahuje samotne CILE
 * prevedenych odkazu, ne odkazy, ktere by se mely konvertovat. Navic tam
 * byva i stovky tisic polozek (v tomto rootfs 273 834) - pruchod by trval
 * minuty a nic nezmenil. */
static int rs_prune_name(const char *name) {
    static const char *prune[] = {
        "proc", "sys", "dev", "data", "system", "apex", "vendor",
        "linkerconfig", "storage", "sdcard", "metadata", "product", "odm",
        "persist", "cache", "config", "debug_ramdisk", ".l2s", NULL
    };
    for (int i = 0; prune[i]; i++)
        if (strcmp(name, prune[i]) == 0) return 1;
    return 0;
}

/* Hloubka adresare od guest korene: "/usr/share" -> 2, "/" -> 0. */
static int rs_depth(const char *guest_dir) {
    int d = 0;
    const char *p = guest_dir;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        d++;
        while (*p && *p != '/') p++;
    }
    return d;
}

/* Vymeni symlink host_link za novy s cilem 'target' (unlink + symlink).
 * Vraci 0 = OK, -1 = chyba (errno). */
static int rs_retarget(const char *host_link, const char *target) {
    if (unlink(host_link) != 0 && errno != ENOENT) return -1;
    if (symlink(target, host_link) != 0) return -1;
    return 0;
}

/* Zpracuje jeden symlink. mode: 0 = dry-run, 1 = convert, 2 = check. */
static void rs_handle_link(const char *host_link, const char *guest_link,
                           const char *target, int mode, FILE *log) {
    if (target[0] != '/') return;              /* relativni = uz hotovo */
    rs_total++;

    /* Cil uz je hostove-absolutni pod ROOTFS (typicky proot/L2S link2symlink:
     * /root/... -> /data/.../parrot/.l2s/.l2s.<hash>0001). Takovy link jadro
     * vyhodnoti spravne i ted (miri na realny inode), takze ho NEMENIME -
     * konverze by jen prepsala stovky tisic polozek v pnpm store a .git
     * objektech bez jakehokoli efektu. */
    if (rs_pref[0]) {
        size_t pl = strlen(rs_pref);
        if (strncmp(target, rs_pref, pl) == 0 &&
            (target[pl] == '/' || target[pl] == 0)) {
            rs_rootfsskip++;
            return;
        }
    }

    if (rs_is_host_target(target)) {
        rs_hostskip++;
        return;
    }

    /* hloubka adresare, ve kterem link lezi */
    char dir[8192];
    snprintf(dir, sizeof dir, "%s", guest_link);
    char *slash = strrchr(dir, '/');
    if (slash == dir) dir[1] = 0;              /* link v koreni -> "/" */
    else if (slash) *slash = 0;
    else dir[0] = 0;

    int depth = rs_depth(dir[0] ? dir : "/");

    char rel[8192];
    size_t o = 0;
    for (int i = 0; i < depth; i++) {
        if (o + 4 >= sizeof rel) return;
        rel[o++] = '.'; rel[o++] = '.'; rel[o++] = '/';
    }
    const char *rest = target;
    while (*rest == '/') rest++;
    if (o + strlen(rest) + 2 >= sizeof rel) return;
    if (*rest) {
        strcpy(rel + o, rest);
        o += strlen(rest);
    } else if (o) {
        o--;                                   /* cil = koren -> "../.." bez '/' */
    }
    rel[o] = 0;
    if (!rel[0]) strcpy(rel, ".");

    switch (mode) {
    case 0:
        rs_say("%s: %s -> %s", guest_link, target, rel);
        rs_conv++;
        break;
    case 1:
        if (rs_retarget(host_link, rel) != 0) {
            fprintf(stderr, "relsym: %s: %s\n", host_link, strerror(errno));
            break;
        }
        if (log) fprintf(log, "%s\t%s\t%s\n", host_link, target, rel);
        rs_say("%s: %s -> %s", guest_link, target, rel);
        rs_conv++;
        break;
    default:
        rs_conv++;
        break;
    }
}

/* Rekurzivni pruchod. pref = prefix rootfs ("" kdyz je rootfs "/"),
 * guest_dir = cesta od guest korene ("/", "/usr", ...). */
static void rs_walk(const char *pref, const char *guest_dir, int mode, FILE *log) {
    char host_dir[8192];
    if (guest_dir[0] == '/' && guest_dir[1] == 0)
        snprintf(host_dir, sizeof host_dir, "%s/", pref[0] ? pref : "");
    else
        snprintf(host_dir, sizeof host_dir, "%s%s", pref, guest_dir);

    DIR *d = opendir(host_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char host_path[8192];
        if (snprintf(host_path, sizeof host_path, "%s%s%s", host_dir,
                     host_dir[strlen(host_dir) - 1] == '/' ? "" : "/",
                     de->d_name) >= (int)sizeof host_path)
            continue;

        char guest_path[8192];
        if (guest_dir[0] == '/' && guest_dir[1] == 0)
            snprintf(guest_path, sizeof guest_path, "/%s", de->d_name);
        else
            snprintf(guest_path, sizeof guest_path, "%s/%s", guest_dir, de->d_name);

        struct stat st;
        if (lstat(host_path, &st) != 0) continue;

        if (S_ISLNK(st.st_mode)) {
            char target[8192];
            ssize_t n = readlink(host_path, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = 0;
            rs_handle_link(host_path, guest_path, target, mode, log);
        } else if (S_ISDIR(st.st_mode)) {
            if (rs_prune_name(de->d_name)) continue;
            rs_walk(pref, guest_path, mode, log);
        }
    }
    closedir(d);
}

/* --revert <zurnal>: vrati puvodni (absolutni) cile podle TSV zurnalu. */
static int rs_revert(const char *logpath) {
    FILE *f = fopen(logpath, "r");
    if (!f) {
        fprintf(stderr, "relsym: zurnal '%s': %s\n", logpath, strerror(errno));
        return 2;
    }
    char line[16384];
    long n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!line[0]) continue;
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t');
        if (t2) *t2 = 0;
        const char *link = line, *orig = t1 + 1;
        struct stat st;
        if (lstat(link, &st) != 0 || !S_ISLNK(st.st_mode)) {
            fprintf(stderr, "relsym: preskoceno (uz neni symlink): %s\n", link);
            bad++;
            continue;
        }
        if (rs_retarget(link, orig) != 0) {
            fprintf(stderr, "relsym: %s: %s\n", link, strerror(errno));
            bad++;
            continue;
        }
        n++;
    }
    fclose(f);
    printf("vraceno %ld linku podle %s%s%ld\n", n, logpath,
           bad ? ", chyb " : "", bad ? bad : 0);
    return bad ? 1 : 0;
}

static void rs_usage(void) {
    fputs(
        "Usage: elf_loader relsym [options]\n"
        "       elf_loader relsym --revert <zurnal>\n"
        "\n"
        "Prevede absolutni symlinky v rootfs na kanonicky relativni tvar\n"
        "(presne hloubka x \"../\" + cesta od guest korene), aby je jadro\n"
        "uvnitr rootfs vyhodnotilo spravne misto proti host koreni.\n"
        "\n"
        "Options:\n"
        "  --root <cesta>   rootfs (default $ROOTFS / $R)\n"
        "  --dry-run        jen vypsat, co by se zmenilo\n"
        "  --check          jen spocitat, co zbyva\n"
        "  --log <soubor>   zurnal pro --revert (default $LX_LOG/relsymlinks-*.tsv)\n"
        "  --quiet          nevypisovat jednotlive linky\n"
        "  --revert <f>     vratit puvodni absolutni cile podle zurnalu\n"
        "  --help           tato napoveda\n"
        "\n"
        "Cile na host mountpointy (/proc, /dev, /system, /data, ...) zustavaji\n"
        "absolutni - stejny seznam jako shim_excluded() v loaderu.\n",
        stdout);
}

/* Vstupni bod podprikazu. Vola se z main() PRED dispatchem do guesta,
 * takze bezi na host libc (zadny shim, zadny guest TP). */
static int elf_relsym_main(int argc, char **argv) {
    const char *root = getenv("ROOTFS");
    if (!root || !root[0]) root = getenv("R");
    const char *logpath = NULL;
    int mode = 1;                              /* 0 dry, 1 convert, 2 check */
    const char *revert = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--dry-run") == 0) mode = 0;
        else if (strcmp(a, "--check") == 0) mode = 2;
        else if (strcmp(a, "--quiet") == 0) rs_quiet = 1;
        else if (strcmp(a, "--root") == 0 && i + 1 < argc) root = argv[++i];
        else if (strcmp(a, "--log") == 0 && i + 1 < argc) logpath = argv[++i];
        else if (strcmp(a, "--revert") == 0 && i + 1 < argc) revert = argv[++i];
        else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { rs_usage(); return 0; }
        else {
            fprintf(stderr, "relsym: nezname option '%s'\n", a);
            rs_usage();
            return 2;
        }
    }

    if (revert) return rs_revert(revert);

    if (!root || !root[0]) {
        fprintf(stderr, "relsym: chybi --root (nebo $ROOTFS/$R)\n");
        return 2;
    }

    /* prefix: "" kdyz je rootfs "/" (beh uvnitr rootfs), jinak cesta bez
     * koncoveho lomitka */
    char pref[8192];
    if (strcmp(root, "/") == 0) pref[0] = 0;
    else {
        snprintf(pref, sizeof pref, "%s", root);
        size_t pl = strlen(pref);
        while (pl > 1 && pref[pl - 1] == '/') pref[--pl] = 0;
    }

    struct stat st;
    if (stat(pref[0] ? pref : "/", &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "relsym: rootfs neexistuje: %s\n", pref[0] ? pref : "/");
        return 2;
    }

    char deflog[8192];
    FILE *log = NULL;
    if (mode == 1) {
        if (!logpath) {
            const char *lxlog = getenv("LX_LOG");
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            snprintf(deflog, sizeof deflog, "%s/relsymlinks-%04d%02d%02d_%02d%02d%02d.tsv",
                     (lxlog && lxlog[0]) ? lxlog : "/tmp",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            logpath = deflog;
        }
        log = fopen(logpath, "w");
        if (!log) {
            fprintf(stderr, "relsym: zurnal '%s': %s\n", logpath, strerror(errno));
            return 2;
        }
    }

    rs_pref = pref;   /* pro skip cilu pod ROOTFS v rs_handle_link */
    rs_walk(pref, "/", mode, log);
    if (log) fclose(log);

    switch (mode) {
    case 0:
        printf("--dry-run: prevedlo by se %ld z %ld absolutnich "
               "(host cilu %ld, pod-rootfs cilu %ld)\n",
               rs_conv, rs_total, rs_hostskip, rs_rootfsskip);
        break;
    case 2:
        printf("absolutnich linku: %ld (prevoditelnych %ld, host cilu %ld, "
               "pod-rootfs cilu %ld)\n",
               rs_total, rs_conv, rs_hostskip, rs_rootfsskip);
        break;
    default:
        printf("prevedeno %ld z %ld absolutnich (host cilu %ld, pod-rootfs cilu "
               "%ld); zurnal: %s\n",
               rs_conv, rs_total, rs_hostskip, rs_rootfsskip, logpath);
        break;
    }
    return 0;
}

#endif /* ELF_LOADER_RELSYMTOOL_H */