/* t4_lib.c - PERSOANA B */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "t4_lib.h"

void t4_name(wchar_t* d, size_t n, const wchar_t* ses, const wchar_t* suf)
{
	swprintf(d, n, L"Local\\CSSO_T4_%ls_%ls", ses, suf);
}

BOOL t4_join(wchar_t* out, size_t n, const wchar_t* dir, const wchar_t* file)
{
	size_t l = wcslen(dir);
	int sep = (l > 0 && (dir[l - 1] == L'\\' || dir[l - 1] == L'/'));
	return swprintf(out, n, L"%ls%ls%ls", dir, sep ? L"" : L"\\", file) > 0;
}

BOOL t4_mkdirs(const wchar_t* dir)
{
	if (!dir || !*dir) return FALSE;
	wchar_t tmp[MAX_PATH];
	wcsncpy(tmp, dir, MAX_PATH - 1);
	tmp[MAX_PATH - 1] = 0;
	size_t len = wcslen(tmp);
	for (size_t i = 1; i < len; i++) {
		if (tmp[i] == L'\\' || tmp[i] == L'/') {
			wchar_t c = tmp[i];
			tmp[i] = 0;
			CreateDirectoryW(tmp, NULL);
			tmp[i] = c;
		}
	}
	if (CreateDirectoryW(tmp, NULL)) return TRUE;
	DWORD err = GetLastError();
	return err == ERROR_ALREADY_EXISTS || err == ERROR_SUCCESS;
}

/* ---------- Collatz ----------
 * Pentru un numar n: daca e par il impartim la 2, daca e impar facem 3n+1, pana ajungem la 1.
 * Numaram pasii. Ex: 6 -> 3 -> 10 -> 5 -> 16 -> 8 -> 4 -> 2 -> 1 = 8 pasi.
 * Returneaza 0 daca 3n+1 ar depasi 2^64-1 (overflow). */
static int collatz(uint64_t n, uint64_t* steps)
{
	uint64_t p = 0;
	while (n != 1) {
		if (n & 1) { if (n > (UINT64_MAX - 1) / 3) return 0; n = 3 * n + 1; }
		else n >>= 1;
		p++;
	}
	*steps = p;
	return 1;
}

int t4_collatz_range(uint64_t a, uint64_t b, uint64_t* n_max, uint64_t* max_steps)
{
	uint64_t best = 0, bestn = a, s;
	int first = 1;
	for (uint64_t n = a;; n++) {                 /* se opreste la n == b (b poate fi UINT64_MAX) */
		if (!collatz(n, &s)) { *n_max = n; *max_steps = 0; return T4_OVERFLOW; }
		if (first || s > best) { best = s; bestn = n; first = 0; }   /* > strict: cel mai mic n la egalitate */
		if (n == b) break;
	}
	*n_max = bestn; *max_steps = best;
	return T4_OK;
}

/* ---------- log ---------- */
static HANDLE g_lf = INVALID_HANDLE_VALUE, g_lm = NULL;
static T4_LOG_HOOK g_log_hook = NULL;
static void* g_log_hook_ctx = NULL;

void t4_set_log_hook(T4_LOG_HOOK hook, void* ctx)
{
	g_log_hook = hook;
	g_log_hook_ctx = ctx;
}

BOOL log_open(const wchar_t* path, HANDLE hLogMutex, BOOL truncate)
{
	if (truncate) {                              /* doar coordonatorul */
		HANDLE t = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (t == INVALID_HANDLE_VALUE) return FALSE;
		CloseHandle(t);
	}
	g_lf = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	g_lm = hLogMutex;
	return g_lf != INVALID_HANDLE_VALUE;
}

void log_close(void)
{
	if (g_lf != INVALID_HANDLE_VALUE) CloseHandle(g_lf);
	g_lf = INVALID_HANDLE_VALUE;
}

/* Scrie O linie in log.txt, folosit de coordonator SI de workeri (procese diferite, acelasi fisier).
 * Cheia 'log' (mutex) asigura ca doua procese nu scriu in acelasi timp. Ora se citeste DUPA ce am
 * luat cheia: asa, ordinea din fisier = ordinea in timp. O singura linie = un singur WriteFile. */
BOOL log_line(const char* source, const char* event, const char* fmt, ...)
{
	char det[400], line[640];
	if (g_lf == INVALID_HANDLE_VALUE) return FALSE;
	if (fmt && *fmt) { va_list ap; va_start(ap, fmt); vsnprintf(det, sizeof det, fmt, ap); va_end(ap); }
	else strcpy(det, "-");
	DWORD w = WaitForSingleObject(g_lm, INFINITE);
	if (w != WAIT_OBJECT_0) { if (w == WAIT_ABANDONED) ReleaseMutex(g_lm); return FALSE; }
	SYSTEMTIME st; GetSystemTime(&st);           /* citit DUPA luarea mutexului => ordine cronologica */
	int n = snprintf(line, sizeof line, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\t%s\t%lu\t%s\t%s\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
		source, GetCurrentProcessId(), event, det);
	DWORD wr = 0;
	BOOL ok = WriteFile(g_lf, line, (DWORD)n, &wr, NULL);   /* un singur WriteFile pe linie */
	ReleaseMutex(g_lm);
	if (g_log_hook) {
		g_log_hook(line, g_log_hook_ctx);
	}
	return ok && wr == (DWORD)n;
}

/* ---------- parsare ---------- */
static int parse_u64(const char* s, size_t len, uint64_t* out)
{
	uint64_t v = 0;
	if (!len) return 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9') return 0;
		unsigned d = (unsigned)(s[i] - '0');
		if (v > (UINT64_MAX - d) / 10) return 0;
		v = v * 10 + d;
	}
	*out = v;
	return 1;
}

static int parse_line(char* l, uint32_t* id, uint64_t* a, uint64_t* b)
{
	char* t1 = strchr(l, '\t');
	char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
	uint64_t v;
	if (!t1 || !t2 || strchr(t2 + 1, '\t')) return 0;
	if (!parse_u64(l, (size_t)(t1 - l), &v) || v < 1 || v > 4294967295ull) return 0;
	*id = (uint32_t)v;
	if (!parse_u64(t1 + 1, (size_t)(t2 - t1 - 1), a)) return 0;
	if (!parse_u64(t2 + 1, strlen(t2 + 1), b)) return 0;
	return *a >= 1 && *a <= *b;
}

int t4_parse_day(const wchar_t* dir, int day, T4_INJOB* jobs, uint32_t* n, uint32_t* invalid)
{
	wchar_t f[MAX_PATH], full[MAX_PATH];
	swprintf(f, MAX_PATH, L"day_%02d.tsv", day);
	t4_join(full, MAX_PATH, dir, f);
	*n = 0; *invalid = 0;
	HANDLE h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();
		return e == ERROR_FILE_NOT_FOUND ? T4_PARSE_NOFILE : e == ERROR_PATH_NOT_FOUND ? T4_PARSE_NODIR : T4_PARSE_ERR;
	}
	LARGE_INTEGER sz;
	if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); return T4_PARSE_ERR; }
	DWORD len = (DWORD)sz.QuadPart, got = 0;
	char* buf = (char*)malloc((size_t)len + 1);
	if (!buf) { CloseHandle(h); return T4_PARSE_ERR; }
	BOOL ok = ReadFile(h, buf, len, &got, NULL);
	CloseHandle(h);
	if (!ok || got != len) { free(buf); return T4_PARSE_ERR; }
	buf[len] = 0;

	char* p = buf, * end = buf + len;
	uint32_t lineno = 0;
	while (p < end) {
		char* e = (char*)memchr(p, '\n', (size_t)(end - p));
		char* le = e ? e : end, * next = e ? e + 1 : end;
		lineno++;
		if (le > p && le[-1] == '\r') le--;
		*le = 0;
		if (lineno > 1 && le > p) {              /* linia 1 = antet; liniile goale se ignora */
			uint32_t id; uint64_t a, b;
			int valid = parse_line(p, &id, &a, &b) && *n < T4_MAX_JOBS;
			for (uint32_t i = 0; valid && i < *n; i++)
				if (jobs[i].id == id) valid = 0;
			if (valid) { jobs[*n].id = id; jobs[*n].a = a; jobs[*n].b = b; (*n)++; }
			else { (*invalid)++; log_line("COORD", "INVALID_LINE", "day=%d line=%u", day, lineno); }
		}
		p = next;
	}
	free(buf);
	return T4_PARSE_OK;
}

/* ---------- iesiri ---------- */
typedef struct { uint32_t id, idx; } PAIR;
static int cmp_pair(const void* x, const void* y)
{
	uint32_t a = ((const PAIR*)x)->id, b = ((const PAIR*)y)->id;
	return a < b ? -1 : a > b;
}

static BOOL write_whole(const wchar_t* path, const char* data, size_t len)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return FALSE;
	DWORD wr = 0;
	BOOL ok = WriteFile(h, data, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
	CloseHandle(h);
	return ok;
}

BOOL t4_write_results(const wchar_t* dir, int day, const T4_INJOB* jobs, const T4_RESULT* res, uint32_t n)
{
	wchar_t f[MAX_PATH], full[MAX_PATH];
	swprintf(f, MAX_PATH, L"day_%02d_results.tsv", day);
	t4_join(full, MAX_PATH, dir, f);
	PAIR* pr = (PAIR*)malloc((n ? n : 1) * sizeof(PAIR));
	char* buf = (char*)malloc(128 + (size_t)n * 160);
	if (!pr || !buf) { free(pr); free(buf); return FALSE; }
	for (uint32_t i = 0; i < n; i++) { pr[i].id = jobs[i].id; pr[i].idx = i; }
	qsort(pr, n, sizeof(PAIR), cmp_pair);
	size_t pos = (size_t)sprintf(buf, "id\ta\tb\tn_max\tmax_steps\tworker\tduration_us\n");
	for (uint32_t k = 0; k < n; k++) {
		const T4_INJOB* j = &jobs[pr[k].idx];
		const T4_RESULT* r = &res[pr[k].idx];
		char steps[24];
		if (r->status == T4_OVERFLOW) strcpy(steps, "OVERFLOW");
		else sprintf(steps, "%llu", (unsigned long long)r->max_steps);
		pos += (size_t)sprintf(buf + pos, "%u\t%llu\t%llu\t%llu\t%s\t%u\t%llu\n", j->id,
			(unsigned long long)j->a, (unsigned long long)j->b, (unsigned long long)r->n_max,
			steps, r->worker, (unsigned long long)r->duration_us);
	}
	BOOL ok = write_whole(full, buf, pos);
	free(pr); free(buf);
	return ok;
}

BOOL t4_summary_init(const wchar_t* dir)
{
	wchar_t full[MAX_PATH];
	const char* h = "day\tjobs\tinvalid\tsum_max_steps\toverflows\tduration_ms\n";
	t4_join(full, MAX_PATH, dir, L"summary.tsv");
	return write_whole(full, h, strlen(h));
}

BOOL t4_summary_row(const wchar_t* dir, int day, uint32_t n, uint32_t invalid,
	const T4_RESULT* res, unsigned long long duration_ms)
{
	wchar_t full[MAX_PATH];
	unsigned long long sum = 0; uint32_t ov = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (res[i].status == T4_OVERFLOW) ov++; else sum += res[i].max_steps;
	}
	char line[160];
	int len = sprintf(line, "%d\t%u\t%u\t%llu\t%u\t%llu\n", day, n, invalid, sum, ov, duration_ms);
	t4_join(full, MAX_PATH, dir, L"summary.tsv");
	HANDLE h = CreateFileW(full, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return FALSE;
	DWORD wr = 0;
	BOOL ok = WriteFile(h, line, (DWORD)len, &wr, NULL);
	CloseHandle(h);
	return ok;
}
