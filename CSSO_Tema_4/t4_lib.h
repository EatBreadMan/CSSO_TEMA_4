/* t4_lib.h - PERSOANA B: modul comun (nume obiecte, Collatz, log, parsare input, fisiere de iesire) */
#ifndef T4_LIB_H
#define T4_LIB_H
#include "t4_common.h"

typedef struct { uint32_t id; uint64_t a, b; } T4_INJOB;
enum { T4_PARSE_OK = 0, T4_PARSE_NOFILE = 1, T4_PARSE_NODIR = 2, T4_PARSE_ERR = 3 };

void t4_name(wchar_t* d, size_t n, const wchar_t* ses, const wchar_t* suf);
BOOL t4_join(wchar_t* out, size_t n, const wchar_t* dir, const wchar_t* file);
BOOL t4_mkdirs(const wchar_t* dir);

/* T4_OK sau T4_OVERFLOW (atunci *n_max = primul n care depaseste) */
int  t4_collatz_range(uint64_t a, uint64_t b, uint64_t* n_max, uint64_t* max_steps);

/* log: mutexul 'log' este al apelantului (nu il inchide log_close). FALSE = eroare/abandoned => exit 3 */
typedef void (*T4_LOG_HOOK)(const char* line, void* ctx);
void t4_set_log_hook(T4_LOG_HOOK hook, void* ctx);
BOOL log_open(const wchar_t* path, HANDLE hLogMutex, BOOL truncate);
void log_close(void);
BOOL log_line(const char* source, const char* event, const char* fmt, ...);

/* parseaza day_XX.tsv; logheaza INVALID_LINE; jobs[] are loc pentru T4_MAX_JOBS */
int  t4_parse_day(const wchar_t* dir, int day, T4_INJOB* jobs, uint32_t* n, uint32_t* invalid);
BOOL t4_write_results(const wchar_t* dir, int day, const T4_INJOB* jobs, const T4_RESULT* res, uint32_t n);
BOOL t4_summary_init(const wchar_t* dir);
BOOL t4_summary_row(const wchar_t* dir, int day, uint32_t n, uint32_t invalid,
	const T4_RESULT* res, unsigned long long duration_ms);
#endif
