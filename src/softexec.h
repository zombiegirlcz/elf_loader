/* src/softexec.h — prompt 11, varianta A: soft exec (exec bez execve(2))
 *
 * ELF_LOADER_SOFTEXEC=1 (default VYPNUTO). Misto re-execu loaderu
 * (`execve($LOADER, --ownall <cil> ...)`) vymeni guest program UVNITR
 * beziciho procesu:
 *
 *   1. obnovi "pristine" snapshot RW stavu guest glibc (porizeny v
 *      run_ownall tesne pred relokaci predchoziho exe),
 *   2. provede to, co jinak dela jadro pri execve: zavre O_CLOEXEC fd,
 *      resetuje dispozice signalu, sigaltstack a interval timery,
 *   3. nacte novy exe (elf_load/elf_relocate), preregistruje link mapy,
 *      postavi novy TLS + stack a skoci na entry.
 *
 * Usetri se tim: execve(2) + bionic linker64 nad loaderem + loader init
 * (seccomp compat, overrides) + nacteni glibc a deps + instalace hooku
 * (hooks_bti) + svc_patch. Zustane jen nacteni noveho exe, jeho relokace,
 * TLS a stack.
 *
 * POZOR — co soft exec NEDELA (proto je default vypnuty, viz
 * docs/nextgen/proot-nahrada.md, kapitola "Soft exec / zygote — vysledek"):
 *   - neni to execve: ladici nastroje nevidi exec event, /proc/<pid>/exe
 *     dal ukazuje na loader, setuid/setgid bity se neaplikuji (loader je
 *     stejne nikdy neaplikoval), PR_SET_DUMPABLE se nemeni;
 *   - stary obraz exe, stary guest stack, stary TLS region a mmapy
 *     predchoziho programu zustanou namapovane (RSS leak ~ jednotky MB na
 *     jeden soft exec). V typickem vzoru `fork -> exec` dela potomek prave
 *     jeden soft exec za svuj zivot, takze se to nekumuluje;
 *   - moduly dlopen-nute PREDCHOZIM programem zustanou v scope s neobnovenym
 *     RW stavem;
 *   - POSIX timery (timer_create) a robust futex list se neresetuji.
 *
 * Podminky pouziti (jinak fallback na skutecny re-exec):
 *   - cil je PIE (ET_DYN) aarch64 ELF s PT_INTERP a BEZ PT_TLS
 *     (PT_TLS exe musi lezet na TP+0x10, ten je uz zabrany predchozim exe;
 *      python3/gcc proto jdou klasickou cestou),
 *   - proces je jednovlaknovy,
 *   - neni to skript (#!) a neni aktivni L2 (vlastni seccomp okno pro exe).
 */
#ifndef ELF_LOADER_SOFTEXEC_H
#define ELF_LOADER_SOFTEXEC_H

extern uintptr_t g_tls_old_tp, g_tls_new_tp;
extern uintptr_t g_exe_base;

#define SX_MAX_REGIONS 96

typedef struct {
    void   *addr;
    size_t  len;
    void   *copy;
} sx_region_t;

static sx_region_t g_sx_reg[SX_MAX_REGIONS];
static int      g_sx_nreg;
static int      g_sx_ready;      /* snapshot je k dispozici (knob byl zapnuty) */
static void    *g_sx_brk;        /* privatni arena (parrot_brk_cur) pri capture */
static size_t   g_sx_pending;    /* pocet queued initu pri capture */
static size_t   g_sx_tlsmods;    /* pocet TLS modulu pri capture */
static uintptr_t g_sx_tlsnext;   /* g_tls_next pri capture */
static elf_scope_t *g_sx_scope;
static int      g_sx_trace;
static int      g_sx_l2;         /* elf_l2_enabled() v dobe capture (getenv chce host TP) */

/* Trasa jde na stderr i do souboru: pri zatuhnuti se stderr (roura ashellu)
 * nemusi dostat ven. Cesta = $ELF_LOADER_SOFTEXEC_TRACE, kdyz zacina '/'. */
static const char *g_sx_logfile;

static void sx_log(const char *msg) {
    if (!g_sx_trace) return;
    static const char pfx[] = "[softexec] ";
    shim_raw_syscall6(64, 2, (long)(unsigned long)pfx, sizeof(pfx) - 1, 0, 0, 0);
    shim_raw_syscall6(64, 2, (long)(unsigned long)msg, shim_strlen(msg), 0, 0, 0);
    shim_raw_syscall6(64, 2, (long)(unsigned long)"\n", 1, 0, 0, 0);
    if (g_sx_logfile) {
        int fd = (int)shim_raw_syscall6(56, -100, (long)(unsigned long)g_sx_logfile,
                                        0x441 /*O_WRONLY|O_CREAT|O_APPEND*/, 0644, 0, 0);
        if (fd >= 0) {
            shim_raw_syscall6(64, fd, (long)(unsigned long)msg, shim_strlen(msg), 0, 0, 0);
            shim_raw_syscall6(64, fd, (long)(unsigned long)"\n", 1, 0, 0, 0);
            shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
        }
    }
}

static void sx_log_num(const char *msg, long v) {
    if (!g_sx_trace) return;
    char b[96]; int i = 0;
    while (*msg && i < 64) b[i++] = *msg++;
    b[i++] = '=';
    if (v < 0) { b[i++] = '-'; v = -v; }
    char t[24]; int ti = 0;
    if (!v) t[ti++] = '0';
    while (v > 0) { t[ti++] = (char)('0' + v % 10); v /= 10; }
    while (ti > 0) b[i++] = t[--ti];
    b[i] = 0;
    sx_log(b);
}

static int softexec_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("ELF_LOADER_SOFTEXEC");
        v = (e && e[0] && e[0] != '0') ? 1 : 0;
        const char *t = getenv("ELF_LOADER_SOFTEXEC_TRACE");
        g_sx_trace = (t != NULL);
        g_sx_logfile = (t && t[0] == '/') ? t : NULL;
    }
    return v;
}

/* ---- snapshot -------------------------------------------------------- */

static void sx_add_region(void *addr, size_t len) {
    if (g_sx_nreg >= SX_MAX_REGIONS || !addr || !len)
        return;
    void *c = malloc(len);
    if (!c)
        return;
    memcpy(c, addr, len);
    g_sx_reg[g_sx_nreg].addr = addr;
    g_sx_reg[g_sx_nreg].len  = len;
    g_sx_reg[g_sx_nreg].copy = c;
    g_sx_nreg++;
}

static void sx_add_module(elf_object_t *m) {
    if (!m || !m->phdr || !m->base_addr)
        return;
    /* mapovaci baze: nejnizsi p_vaddr PT_LOAD (u .so je 0, u ET_EXEC ne) */
    size_t mbv = (size_t)-1;
    for (int i = 0; i < m->phdr_count; i++)
        if (m->phdr[i].p_type == PT_LOAD && m->phdr[i].p_vaddr < mbv)
            mbv = m->phdr[i].p_vaddr;
    if (mbv == (size_t)-1)
        return;
    {   /* stejne zarovnani jako map_base_vaddr() v elf_loader.c */
        size_t ps = (size_t)sysconf(_SC_PAGESIZE);
        if (ps) mbv &= ~(ps - 1);
    }
    for (int i = 0; i < m->phdr_count; i++) {
        if (m->phdr[i].p_type != PT_LOAD || !(m->phdr[i].p_flags & PF_W))
            continue;
        char *a = (char *)m->base_addr + (m->phdr[i].p_vaddr - mbv);
        sx_add_region(a, m->phdr[i].p_memsz);
    }
}

/* Volano z run_ownall tesne PRED elf_relocate(exe): vsechny zavislosti uz
 * jsou nactene, zrelokovane a zahakovane, ale exe jeste nezasahl do glibc
 * (COPY relokace, queued inity, link mapa, TLS). Presne stav, do ktereho se
 * chce soft exec vratit. */
static void softexec_capture(elf_scope_t *scope) {
    if (!softexec_enabled() || g_sx_ready || !scope)
        return;
    g_sx_nreg = 0;
    for (size_t i = 0; i < scope->count; i++)
        sx_add_module(scope->mods[i]);
    void *g = NULL, *ro = NULL; size_t gn = 0, ron = 0;
    elf_sx_fake_blocks(&g, &gn, &ro, &ron);
    sx_add_region(g, gn);
    sx_add_region(ro, ron);
    g_sx_brk     = elf_sx_brk_get();
    g_sx_pending = elf_sx_pending_count();
    g_sx_tlsmods = elf_tls_module_count();
    g_sx_tlsnext = elf_tls_span();
    g_sx_scope   = scope;
    g_sx_l2      = elf_l2_enabled();
    g_sx_ready   = (g_sx_nreg > 0);
    if (g_sx_trace) {
        char b[64]; int i = 0; const char *p = "snapshot regions=";
        while (*p) b[i++] = *p++;
        int n = g_sx_nreg; char t[8]; int ti = 0;
        if (!n) t[ti++] = '0';
        while (n) { t[ti++] = (char)('0' + n % 10); n /= 10; }
        while (ti) b[i++] = t[--ti];
        b[i] = 0;
        sx_log(b);
    }
}

static void softexec_restore(void) {
    for (int i = 0; i < g_sx_nreg; i++)
        memcpy(g_sx_reg[i].addr, g_sx_reg[i].copy, g_sx_reg[i].len);
    elf_sx_brk_set(g_sx_brk);
    elf_sx_pending_truncate(g_sx_pending);
    elf_sx_tls_truncate(g_sx_tlsmods, g_sx_tlsnext);
}

/* ---- "co dela execve": fd, signaly, timery --------------------------- */

struct sx_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

static void sx_close_cloexec(void) {
    int dfd = (int)shim_raw_syscall6(56 /*openat*/, -100,
                                     (long)(unsigned long)"/proc/self/fd",
                                     O_RDONLY | O_DIRECTORY, 0, 0, 0);
    if (dfd < 0)
        return;
    int victims[256];
    int nv = 0;
    char buf[4096];
    for (;;) {
        long n = shim_raw_syscall6(61 /*getdents64*/, dfd,
                                   (long)(unsigned long)buf, sizeof buf, 0, 0, 0);
        if (n <= 0)
            break;
        for (long off = 0; off < n && nv < (int)(sizeof victims / sizeof victims[0]);) {
            struct sx_dirent64 *de = (struct sx_dirent64 *)(buf + off);
            off += de->d_reclen;
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            int fd = 0;
            for (const char *q = de->d_name; *q >= '0' && *q <= '9'; q++)
                fd = fd * 10 + (*q - '0');
            if (fd == dfd || fd < 0)
                continue;
            long fl = shim_raw_syscall6(25 /*fcntl*/, fd, 1 /*F_GETFD*/, 0, 0, 0, 0);
            if (fl >= 0 && (fl & 1 /*FD_CLOEXEC*/))
                victims[nv++] = fd;
        }
    }
    shim_raw_syscall6(57 /*close*/, dfd, 0, 0, 0, 0, 0);
    for (int i = 0; i < nv; i++)
        shim_raw_syscall6(57, victims[i], 0, 0, 0, 0, 0);
}

/* aarch64 kernel sigaction: { handler, flags, mask } (bez sa_restorer). */
struct sx_ksigaction {
    void          *handler;
    unsigned long  flags;
    unsigned long  mask;
};

static void sx_reset_signals(void) {
    sx_log("sig-enter");
    for (int s = 1; s <= 64; s++) {
        if (s == SIGKILL || s == SIGSTOP)
            continue;
        /* Synchronni signaly drzi loader (fault handler + SIGSYS pro L2/Go);
         * elf_run_final je stejne hned reinstaluje, takze je nechavame byt. */
        if (s == SIGSEGV || s == SIGBUS || s == SIGILL || s == SIGFPE ||
            s == SIGTRAP || s == SIGSYS)
            continue;
        struct sx_ksigaction old;
        shim_memset(&old, 0, sizeof old);
        if (shim_raw_syscall6(134 /*rt_sigaction*/, s, 0,
                              (long)(unsigned long)&old, 8, 0, 0) != 0)
            continue;
        if (old.handler == (void *)0 || old.handler == (void *)1)  /* DFL/IGN */
            continue;
        sx_log_num("sig-reset", s);
        struct sx_ksigaction act;
        shim_memset(&act, 0, sizeof act);
        shim_raw_syscall6(134, s, (long)(unsigned long)&act, 0, 8, 0, 0);
        sx_log_num("sig-reset-ok", s);
    }
    sx_log("sig-loop-end");
    struct { void *sp; int flags; int pad; size_t size; } ss;
    shim_memset(&ss, 0, sizeof ss);
    ss.flags = 2;   /* SS_DISABLE */
    shim_raw_syscall6(132 /*sigaltstack*/, (long)(unsigned long)&ss, 0, 0, 0, 0, 0);
    sx_log("sig-altstack-done");
    struct { long sec, usec, isec, iusec; } itv;
    shim_memset(&itv, 0, sizeof itv);
    for (int w = 0; w < 3; w++)
        shim_raw_syscall6(103 /*setitimer*/, w, (long)(unsigned long)&itv, 0, 0, 0, 0);
}

/* ---- pre-check cile (raw, bez host TP) -------------------------------- */

static int sx_single_threaded(void) {
    int fd = (int)shim_raw_syscall6(56, -100,
                                    (long)(unsigned long)"/proc/self/status",
                                    O_RDONLY, 0, 0, 0);
    if (fd < 0)
        return 0;
    char b[2048];
    long n = shim_raw_syscall6(63 /*read*/, fd, (long)(unsigned long)b,
                               sizeof b - 1, 0, 0, 0);
    shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    if (n <= 0)
        return 0;
    b[n] = 0;
    for (long i = 0; i + 9 < n; i++) {
        if (b[i] == 'T' && shim_strncmp(b + i, "Threads:", 8) == 0) {
            long j = i + 8;
            while (b[j] == ' ' || b[j] == '\t') j++;
            return b[j] == '1' && (b[j + 1] == '\n' || b[j + 1] == 0);
        }
    }
    return 0;
}

/* PIE aarch64 ELF s PT_INTERP a bez PT_TLS. */
static int sx_target_ok(const char *path) {
    int fd = (int)shim_raw_syscall6(56, -100, (long)(unsigned long)path,
                                    O_RDONLY, 0, 0, 0);
    if (fd < 0)
        return 0;
    int ok = 0;
    Elf64_Ehdr eh;
    long n = shim_raw_syscall6(67 /*pread64*/, fd, (long)(unsigned long)&eh,
                               sizeof eh, 0, 0, 0);
    if (n == (long)sizeof eh &&
        eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' &&
        eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F' &&
        eh.e_ident[4] == 2 /*ELFCLASS64*/ &&
        eh.e_type == ET_DYN && eh.e_machine == EM_AARCH64 &&
        eh.e_phnum > 0 && eh.e_phnum <= 64 &&
        eh.e_phentsize == sizeof(Elf64_Phdr)) {
        Elf64_Phdr ph[64];
        size_t want = (size_t)eh.e_phnum * sizeof(Elf64_Phdr);
        long m = shim_raw_syscall6(67, fd, (long)(unsigned long)ph,
                                   (long)want, (long)eh.e_phoff, 0, 0);
        if (m == (long)want) {
            int has_interp = 0, has_tls = 0;
            for (int i = 0; i < eh.e_phnum; i++) {
                if (ph[i].p_type == PT_INTERP) has_interp = 1;
                if (ph[i].p_type == PT_TLS)    has_tls = 1;
            }
            ok = has_interp && !has_tls;
        }
    }
    shim_raw_syscall6(57, fd, 0, 0, 0, 0, 0);
    return ok;
}

/* ---- vlastni soft exec ------------------------------------------------ */

/* Kopie argv/envp mimo guest pamet (stary stack/heap po obnove snapshotu
 * nikdo negarantuje). Alokuje se z bionic heapu loaderu. */
static char **sx_dup_vec(char *const v[], int *count) {
    int n = 0;
    while (v && v[n]) n++;
    char **out = (char **)malloc((size_t)(n + 1) * sizeof(char *));
    if (!out) return NULL;
    for (int i = 0; i < n; i++) {
        size_t l = shim_strlen(v[i]) + 1;
        char *s = (char *)malloc(l);
        if (!s) { out[i] = NULL; n = i; break; }
        memcpy(s, v[i], l);
        out[i] = s;
    }
    out[n] = NULL;
    if (count) *count = n;
    return out;
}

/* Vraci 0 = soft exec se nepouzil (volajici pokracuje re-execem).
 * Pri uspechu se nevraci (skoci do noveho programu). */
static int softexec_try(const char *path, char *const argv[], char *const envp[]) {
    if (!g_sx_ready || !g_sx_scope || !path || !path[0])
        return 0;
    if (g_sx_l2)                    /* L2 ma vlastni seccomp okno pro exe */
        return 0;
    if (!sx_target_ok(path) || !sx_single_threaded())
        return 0;

    /* Od tohoto mista bezime jako LOADER: bionic TP + zablokovane async
     * signaly (stejny vzor jako dl_enter_host v elf_loader.c). */
    uintptr_t cur;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(cur));
    if (!g_tls_old_tp || cur == g_tls_old_tp)
        return 0;                   /* nejsme pod guest TP -> nestandardni stav */
    /* Stejne jako dl_block_async(): blokuj vse krome synchronnich signalu,
     * aby async signal nedosel do guest handleru pod bionickym TP. */
    unsigned long oldmask = 0;
    unsigned long allmask = ~((1UL << (SIGSEGV - 1)) | (1UL << (SIGBUS - 1)) |
                              (1UL << (SIGILL - 1))  | (1UL << (SIGFPE - 1)) |
                              (1UL << (SIGTRAP - 1)) | (1UL << (SIGSYS - 1)) |
                              (1UL << (SIGABRT - 1)));
    shim_raw_syscall6(135 /*rt_sigprocmask*/, SIG_BLOCK,
                      (long)(unsigned long)&allmask,
                      (long)(unsigned long)&oldmask, 8, 0, 0);
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(g_tls_old_tp));

    sx_log(path);

    int argc = 0;
    char **nargv = sx_dup_vec(argv, &argc);
    char **nenvp = sx_dup_vec(envp, NULL);
    if (!nargv || !nenvp || argc <= 0) {
        /* navrat ke guestu - jeste jsme nic nerozbili */
        __asm__ volatile("msr tpidr_el0, %0" : : "r"(cur));
        shim_raw_syscall6(135, SIG_SETMASK, (long)(unsigned long)&oldmask, 0, 8, 0, 0);
        return 0;
    }

    /* ---- bod, odkud neni navratu ---- */
    g_loader_active = 1;
    sx_log("restore-begin");
    softexec_restore();
    sx_log("restore-done");
    if (!getenv("ELF_LOADER_SOFTEXEC_SKIP_FD"))
        sx_close_cloexec();
    sx_log("fds-done");
    if (!getenv("ELF_LOADER_SOFTEXEC_SKIP_SIG"))
        sx_reset_signals();
    sx_log("signals-done");

    elf_scope_t *scope = g_sx_scope;
    elf_init_argc = argc;
    elf_init_argv = nargv;
    elf_init_envp = nenvp;
    shim_strcpy(g_guest_exe_path, sizeof(g_guest_exe_path), path);

    size_t mods_before = scope->count;
    sx_log("load-begin");
    /* run_ownall po nacteni exe nuluje elf_own_scope; bez obnoveni by novy
     * exe nemel scope a vsechny importy (stdout, program_invocation_*) by
     * zustaly nerelokovane (GOT = 0 -> skok na pc=0 v __libc_init_first). */
    elf_own_deps = 1;
    elf_own_scope = scope;
    elf_object_t *obj = elf_load(path);
    elf_own_scope = NULL;
    sx_log("load-done");
    if (!obj || elf_relocate(obj) != 0) {
        static const char m[] = "[softexec] FATAL: nelze nacist cil\n";
        shim_raw_syscall6(64, 2, (long)(unsigned long)m, sizeof(m) - 1, 0, 0, 0);
        shim_raw_syscall6(94 /*exit_group*/, 127, 0, 0, 0, 0, 0);
    }
    sx_log("reloc-done");
    elf_queue_module_inits(obj);
    g_exe_base = (uintptr_t)obj->base_addr;
    scope->exe = obj;
    ldso_install_exe_linkmap(obj, path);
    ldso_install_module_list(scope->mods, scope->count);
    sx_log("linkmap-done");

    /* Nove nactene moduly (nova zavislost cile) jeste nemaji prepsane
     * svc #0 ani L1 stuby; u uz nactenych je flag svc_patched/l1_patched. */
    if (!getenv("ELF_LOADER_NO_PATCH")) {
        for (size_t i = mods_before; i < scope->count; i++)
            elf_patch_syscall_sites_all(scope->mods[i]);
        elf_patch_syscall_sites_all(obj);
    }
    {
        const char *l1 = getenv("ELF_LOADER_L1");
        if (l1 && l1[0] && l1[0] != '0') {
            for (size_t i = mods_before; i < scope->count; i++)
                elf_l1_patch_module(scope->mods[i]);
            elf_l1_patch_module(obj);
        }
    }

    sx_log("patch-done");
    if (!getenv("ELF_LOADER_NO_PROCVIRT")) {
        const char *bn = path;
        for (const char *q = path; *q; q++) if (*q == '/') bn = q + 1;
        char comm[16];
        snprintf(comm, sizeof comm, "%s", bn);
        prctl(PR_SET_NAME, comm, 0, 0, 0);
        procvirt_rewrite_argarea(g_karg_start, g_karg_end, argc, nargv);
    }

    sx_log("procvirt-done");
    g_loader_active = 0;
    elf_setup_own_tls(obj, scope);
    sx_log("tls-done");
    /* execve zachovava masku blokovanych signalu volajiciho */
    shim_raw_syscall6(135, SIG_SETMASK, (long)(unsigned long)&oldmask, 0, 8, 0, 0);
    sx_log("run");
    elf_run(obj, argc, nargv, nenvp);
    shim_raw_syscall6(94, 127, 0, 0, 0, 0, 0);
    return 0;
}

#endif /* ELF_LOADER_SOFTEXEC_H */
