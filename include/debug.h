#ifndef ELF_LOADER_DEBUG_H
#define ELF_LOADER_DEBUG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

/* Debug úrovně */
#define ELF_DEBUG_NONE      0
#define ELF_DEBUG_ERROR     1
#define ELF_DEBUG_WARN      2
#define ELF_DEBUG_INFO      3
#define ELF_DEBUG_DEBUG     4
#define ELF_DEBUG_VERBOSE   5
#define ELF_DEBUG_TRACE     6

/* Globální debug úroveň */
extern int g_elf_debug_level;

/* Inicializace debug systému */
void elf_debug_init(void);

/* Nastavení/get debug úrovně */
void elf_debug_set_level(int level);
int elf_debug_get_level(void);

/* Logovací funkce */
void elf_log_error(const char *fmt, ...);
void elf_log_warn(const char *fmt, ...);
void elf_log_info(const char *fmt, ...);
void elf_log_debug(const char *fmt, ...);
void elf_log_trace(const char *fmt, ...);

/* Makra pro snadné použití */
#define LOG_ERROR(...)  elf_log_error(__VA_ARGS__)
#define LOG_WARN(...)   elf_log_warn(__VA_ARGS__)
#define LOG_INFO(...)   elf_log_info(__VA_ARGS__)
#define LOG_DEBUG(...)  elf_log_debug(__VA_ARGS__)
#define LOG_TRACE(...)  elf_log_trace(__VA_ARGS__)

/* Podmíněné logování podle úrovně */
#define LOG_IF(level, fmt, ...) \
    do { if (g_elf_debug_level >= (level)) elf_log_##level(fmt, ##__VA_ARGS__); } while(0)

/* Dump paměti */
void elf_debug_dump_memory(const char *label, void *addr, size_t len);
void elf_debug_dump_hex(const char *label, const uint8_t *data, size_t len);

/* Dump ELF struktur */
void elf_debug_dump_header(const char *label, const void *ehdr);
void elf_debug_dump_phdr(const char *label, const void *phdr, int count);
void elf_debug_dump_shdr(const char *label, const void *shdr, int count);
void elf_debug_dump_symbols(const char *label, const void *symtab, size_t count);
void elf_debug_dump_relocations(const char *label, const void *rela, size_t count);

/* Dump stavu procesu */
void elf_debug_dump_maps(void);
void elf_debug_dump_regs(void);
void elf_debug_dump_stack(void *sp, size_t len);

/* Tracing volání funkcí */
void elf_trace_enter(const char *func);
void elf_trace_exit(const char *func, const char *result);
void elf_trace_call(const char *func, const char *fmt, ...);

/* Performance tracking */
void elf_perf_start(const char *name);
void elf_perf_end(const char *name);
void elf_perf_report(void);

/* Crash dump */
void elf_debug_dump_on_crash(void *pc, void *sp, int signal);

/* Conditional debug macros */
#ifdef ELF_DEBUG_ENABLED
    #define DBG_LOG(fmt, ...)  elf_log_debug(fmt, ##__VA_ARGS__)
    #define DBG_TRACE(fmt, ...) elf_log_trace(fmt, ##__VA_ARGS__)
    #define DBG_DUMP_MEM(label, addr, len) elf_debug_dump_memory(label, addr, len)
#else
    #define DBG_LOG(fmt, ...)  ((void)0)
    #define DBG_TRACE(fmt, ...) ((void)0)
    #define DBG_DUMP_MEM(label, addr, len) ((void)0)
#endif

#endif /* ELF_LOADER_DEBUG_H */
