/* t4_engine.cpp - Coordinator execution engine */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t4_engine.h"

typedef struct {
	wchar_t ses[40];
	HANDLE mem, mtx, empty, full, done, logm, stop;
	T4_AREA* z;
	HANDLE proc[T4_MAX_WORKERS];
	DWORD  pid[T4_MAX_WORKERS];
	int K, started, dead;
} Sess;

static HANDLE g_stop = NULL;
static LARGE_INTEGER g_freq;

void t4_engine_request_stop(void)
{
	if (g_stop) {
		SetEvent(g_stop);
	}
}

static BOOL WINAPI on_ctrl(DWORD t)
{
	if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) {
		t4_engine_request_stop();
		return TRUE;
	}
	return FALSE;
}

static HANDLE fresh(HANDLE h)
{
	if (!h) return NULL;
	if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(h); return NULL; }
	return h;
}

static BOOL create_session(Sess* s, uint32_t cap)
{
	wchar_t n[96];
	t4_name(n, 96, s->ses, T4_SUFFIX_MEM);
	s->mem = fresh(CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(T4_AREA), n));
	t4_name(n, 96, s->ses, T4_SUFFIX_MTX);   s->mtx = fresh(CreateMutexW(NULL, FALSE, n));
	t4_name(n, 96, s->ses, T4_SUFFIX_EMPTY); s->empty = fresh(CreateSemaphoreW(NULL, (LONG)cap, (LONG)cap, n));
	t4_name(n, 96, s->ses, T4_SUFFIX_FULL);  s->full = fresh(CreateSemaphoreW(NULL, 0, (LONG)cap, n));
	t4_name(n, 96, s->ses, T4_SUFFIX_DONE);  s->done = fresh(CreateSemaphoreW(NULL, 0, 0x7FFFFFFF, n));
	t4_name(n, 96, s->ses, T4_SUFFIX_LOG);   s->logm = fresh(CreateMutexW(NULL, FALSE, n));
	t4_name(n, 96, s->ses, T4_SUFFIX_STOP);  s->stop = fresh(CreateEventW(NULL, TRUE, FALSE, n));
	return s->mem && s->mtx && s->empty && s->full && s->done && s->logm && s->stop;
}

static void close_session(Sess* s)
{
	if (s->z) UnmapViewOfFile(s->z);
	s->z = NULL;
	HANDLE* h[] = { &s->mem, &s->mtx, &s->empty, &s->full, &s->done, &s->logm, &s->stop };
	for (int i = 0; i < 7; i++) if (*h[i]) { CloseHandle(*h[i]); *h[i] = NULL; }
	for (int i = 0; i < s->started; i++) CloseHandle(s->proc[i]);
	s->started = 0;
}

static BOOL lock_mtx(HANDLE m)
{
	DWORD w = WaitForSingleObject(m, INFINITE);
	if (w == WAIT_OBJECT_0) return TRUE;
	if (w == WAIT_ABANDONED) ReleaseMutex(m);
	return FALSE;
}

static void stop_workers(Sess* s)
{
	if (s->stop) SetEvent(s->stop);
	if (s->started > 0 &&
		WaitForMultipleObjects((DWORD)s->started, s->proc, TRUE, 5000) == WAIT_TIMEOUT)
		for (int i = 0; i < s->started; i++)
			if (WaitForSingleObject(s->proc[i], 0) == WAIT_TIMEOUT) {
				TerminateProcess(s->proc[i], 4);
				WaitForSingleObject(s->proc[i], 1000);
			}
}

static BOOL start_workers(Sess* s, const wchar_t* resdir, DWORD delay, int jobLog, const T4_COORD_CONFIG* cfg)
{
	wchar_t exe[MAX_PATH], cmd[2048], ds[40] = L"";
	if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return FALSE;
	wchar_t* bs = wcsrchr(exe, L'\\');
	wcscpy(bs ? bs + 1 : exe, L"t4_worker.exe");
	if (delay) swprintf(ds, 40, L" --delay-ms %lu", delay);
	for (int i = 1; i <= s->K; i++) {
		STARTUPINFOW si = { sizeof si };
		PROCESS_INFORMATION pi;
		swprintf(cmd, 2048, L"\"%ls\" %ls %d \"%ls\"%ls%ls", exe, s->ses, i, resdir, ds,
			jobLog ? L"" : L" --no-job-log");
		if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi))
			return FALSE;
		CloseHandle(pi.hThread);
		s->proc[s->started] = pi.hProcess;
		s->pid[s->started] = pi.dwProcessId;
		s->started++;
		log_line("COORD", "WORKER_STARTED", "worker=%d pid=%lu", i, pi.dwProcessId);
		if (cfg->on_worker_started) cfg->on_worker_started(cfg->cb_ctx, i, pi.dwProcessId);
	}
	return TRUE;
}

static uint32_t count_done(Sess* s, uint32_t n, BOOL* err)
{
	uint32_t c = 0;
	if (!lock_mtx(s->mtx)) { *err = TRUE; return 0; }
	for (uint32_t i = 0; i < n; i++) if (s->z->results[i].state == T4_DONE) c++;
	ReleaseMutex(s->mtx);
	return c;
}

static int wait3(Sess* s, HANDLE obj, const T4_COORD_CONFIG* cfg)
{
	HANDLE hs[2 + T4_MAX_WORKERS];
	hs[0] = s->stop;
	for (int i = 0; i < s->K; i++) hs[1 + i] = s->proc[i];
	hs[1 + s->K] = obj;
	DWORD w = WaitForMultipleObjects((DWORD)(2 + s->K), hs, FALSE, INFINITE);
	if (w == WAIT_OBJECT_0) return 1;
	if (w == WAIT_OBJECT_0 + 1 + (DWORD)s->K) return 0;
	if (w > WAIT_OBJECT_0 && w <= WAIT_OBJECT_0 + (DWORD)s->K) {
		int i = (int)(w - WAIT_OBJECT_0) - 1;
		s->dead = i + 1;
		log_line("COORD", "WORKER_DEAD", "worker=%d pid=%lu", i + 1, s->pid[i]);
		if (cfg->on_worker_dead) cfg->on_worker_dead(cfg->cb_ctx, i + 1, s->pid[i]);
	}
	return 3;
}

static int run_day(Sess* s, int day, const T4_INJOB* jobs, uint32_t n, double* secs, const T4_COORD_CONFIG* cfg)
{
	T4_AREA* z = s->z;
	BOOL err = FALSE;

	if (!lock_mtx(s->mtx)) return 3;
	memset(z->results, 0, sizeof z->results);
	z->header.current_day = (uint32_t)day;
	z->header.job_count = n;
	for (uint32_t i = 0; i < n; i++) { z->results[i].state = T4_FREE; z->results[i].id = jobs[i].id; }
	ReleaseMutex(s->mtx);
	if (!log_line("COORD", "DAY_START", "day=%d jobs=%u", day, n)) return 3;
	if (cfg->on_day_start) cfg->on_day_start(cfg->cb_ctx, day, n);

	LARGE_INTEGER t0, t1;
	QueryPerformanceCounter(&t0);

	for (uint32_t pub = 0; pub < n; ) {
		int r = wait3(s, s->empty, cfg);
		if (r) return r;
		if (!lock_mtx(s->mtx)) return 3;
		T4_JOB* slot = &z->ring[z->header.published % z->header.capacity];
		slot->day = (uint32_t)day; slot->id = jobs[pub].id; slot->index = pub;
		slot->a = jobs[pub].a; slot->b = jobs[pub].b;
		z->header.published++;
		z->results[pub].state = T4_QUEUED;
		ReleaseMutex(s->mtx);
		ReleaseSemaphore(s->full, 1, NULL);
		pub++;
	}

	for (;;) {
		uint32_t c = count_done(s, n, &err);
		if (err) return 3;
		if (cfg->on_progress) {
			uint32_t wdone[T4_MAX_WORKERS + 1] = { 0 };
			if (lock_mtx(s->mtx)) {
				for (uint32_t i = 0; i < n; i++) {
					if (z->results[i].state == T4_DONE) {
						uint32_t wid = z->results[i].worker;
						if (wid <= (uint32_t)s->K) wdone[wid]++;
					}
				}
				ReleaseMutex(s->mtx);
			}
			cfg->on_progress(cfg->cb_ctx, day, c, n, wdone);
		}
		if (c == n) break;
		int r = wait3(s, s->done, cfg);
		if (r) return r;
	}
	QueryPerformanceCounter(&t1);
	*secs = (double)(t1.QuadPart - t0.QuadPart) / (double)g_freq.QuadPart;
	return 0;
}

static BOOL append_exp_row(const wchar_t* path, const char* line)
{
	HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return FALSE;
	DWORD wr;
	BOOL ok = WriteFile(h, line, (DWORD)strlen(line), &wr, NULL);
	CloseHandle(h);
	return ok;
}

static void log_hook_forward(const char* line, void* ctx)
{
	const T4_COORD_CONFIG* cfg = (const T4_COORD_CONFIG*)ctx;
	if (cfg && cfg->on_log_line) {
		cfg->on_log_line(cfg->cb_ctx, line);
	}
}

int t4_engine_run(const T4_COORD_CONFIG* cfg)
{
	if (!cfg || cfg->K < 1 || cfg->K >(int)T4_MAX_WORKERS) return 2;
	if (cfg->capacity < 1 || cfg->capacity > T4_CAPACITY || cfg->repetitions < 1) return 2;

	QueryPerformanceFrequency(&g_freq);

	{
		wchar_t d1[MAX_PATH]; t4_join(d1, MAX_PATH, cfg->days_dir, L"day_01.tsv");
		if (GetFileAttributesW(d1) == INVALID_FILE_ATTRIBUTES) return 2;
	}
	if (!t4_mkdirs(cfg->results_dir)) return 3;

	DWORD h0 = 0, h1 = 0;
	GetProcessHandleCount(GetCurrentProcess(), &h0);

	Sess s; memset(&s, 0, sizeof s);
	s.K = cfg->K;
	if (cfg->session[0]) wcscpy(s.ses, cfg->session);
	else swprintf(s.ses, 40, L"%08lx", GetCurrentProcessId());

	T4_INJOB* jobs = (T4_INJOB*)malloc(T4_MAX_JOBS * sizeof(T4_INJOB));
	T4_RESULT* loc = (T4_RESULT*)malloc(T4_MAX_JOBS * sizeof(T4_RESULT));
	int rc = 3; const char* reason = NULL;
	uint32_t passes = cfg->experiment ? cfg->repetitions + 1 : 1, donePasses = 0;
	double pSecs[64]; uint32_t pJobs[64];
	if (passes > 64 || !jobs || !loc) { free(jobs); free(loc); return 2; }
	char sesA[40]; snprintf(sesA, sizeof sesA, "%ls", s.ses);

	if (!create_session(&s, cfg->capacity)) goto cleanup;
	g_stop = s.stop;
	if (cfg->out_hStop) *cfg->out_hStop = s.stop;

	s.z = (T4_AREA*)MapViewOfFile(s.mem, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(T4_AREA));
	if (!s.z) goto cleanup;

	if (!lock_mtx(s.mtx)) goto cleanup;
	s.z->header.magic = T4_MAGIC; s.z->header.version = T4_VERSION;
	s.z->header.header_size = sizeof(T4_HEADER); s.z->header.total_size = sizeof(T4_AREA);
	s.z->header.capacity = cfg->capacity; s.z->header.max_jobs = T4_MAX_JOBS; s.z->header.initialized = 1;
	ReleaseMutex(s.mtx);

	{
		wchar_t lp[MAX_PATH]; t4_join(lp, MAX_PATH, cfg->results_dir, L"log.txt");
		if (!log_open(lp, s.logm, TRUE)) goto cleanup;
	}
	if (cfg->on_log_line) {
		t4_set_log_hook(log_hook_forward, (void*)cfg);
	}
	if (!cfg->experiment && !t4_summary_init(cfg->results_dir)) goto cleanup;

	/* Only register console Ctrl+C if no GUI context */
	if (!cfg->cb_ctx) {
		SetConsoleCtrlHandler(on_ctrl, TRUE);
	}

	log_line("COORD", "SESSION_START", "session=%s workers=%d capacity=%lu", sesA, cfg->K, cfg->capacity);

	if (!start_workers(&s, cfg->results_dir, cfg->delay_ms, cfg->job_log, cfg)) goto cleanup;

	rc = 0;
	for (uint32_t pass = 0; pass < passes && rc == 0; pass++) {
		double tot = 0; uint32_t totJobs = 0;
		for (int day = 1; rc == 0; day++) {
			uint32_t n, inv;
			int pr = t4_parse_day(cfg->days_dir, day, jobs, &n, &inv);
			if (pr == T4_PARSE_NOFILE) break;
			if (pr != T4_PARSE_OK) { rc = 3; break; }
			double secs = 0;
			int r = run_day(&s, day, jobs, n, &secs, cfg);
			if (r == 1) { rc = 1; reason = "ctrl-c"; break; }
			if (r == 3) { rc = 3; reason = s.dead ? "worker-dead" : "error"; break; }
			unsigned long long ms = (unsigned long long)(secs * 1000.0 + 0.5);
			log_line("COORD", "DAY_DONE", "day=%d jobs=%u duration_ms=%llu", day, n, ms);
			if (cfg->on_day_done) cfg->on_day_done(cfg->cb_ctx, day, n, ms);

			if (!lock_mtx(s.mtx)) { rc = 3; reason = "error"; break; }
			memcpy(loc, s.z->results, n * sizeof(T4_RESULT));
			ReleaseMutex(s.mtx);

			if (!t4_write_results(cfg->results_dir, day, jobs, loc, n) ||
				(!cfg->experiment && !t4_summary_row(cfg->results_dir, day, n, inv, loc, ms))) {
				rc = 3; reason = "error"; break;
			}
			tot += secs; totJobs += n;
		}
		if (rc == 0) { pSecs[pass] = tot; pJobs[pass] = totJobs; donePasses++; }
	}

cleanup:
	if (!cfg->cb_ctx) {
		SetConsoleCtrlHandler(on_ctrl, FALSE);
	}
	if (reason) log_line("COORD", "STOP", "reason=%s", reason);
	stop_workers(&s);
	log_line("COORD", "SESSION_END", "code=%d", rc);
	t4_set_log_hook(NULL, NULL);
	log_close();
	close_session(&s);
	free(jobs); free(loc);
	g_stop = NULL;
	if (cfg->out_hStop) *cfg->out_hStop = NULL;

	if (cfg->experiment) {
		GetProcessHandleCount(GetCurrentProcess(), &h1);
		wchar_t ep[MAX_PATH]; t4_join(ep, MAX_PATH, cfg->results_dir, L"experiments.tsv");
		if (GetFileAttributesW(ep) == INVALID_FILE_ATTRIBUTES)
			append_exp_row(ep, "run\texperiment\tparameters\trepetition\tvalue\tunit\terror_code\thandles_before\thandles_after\tdetails\n");
		char line[512];
		for (uint32_t p = 0; p < donePasses; p++) {
			snprintf(line, sizeof line, "%s\tE1\tworkers=%d;capacity=%lu;log=%s\t%u\t%.2f\tjobs/s\t0\t%lu\t%lu\tsession=%s jobs=%u duration_ms=%.0f\n",
				sesA, cfg->K, cfg->capacity, cfg->job_log ? "yes" : "no", p, pJobs[p] / pSecs[p], h0, h1, sesA, pJobs[p], pSecs[p] * 1000);
			append_exp_row(ep, line);
		}
		if (rc != 0) {
			snprintf(line, sizeof line, "%s\tE1\tworkers=%d;capacity=%lu;log=%s\t0\t0\tjobs/s\t%d\t%lu\t%lu\tfailed\n",
				sesA, cfg->K, cfg->capacity, cfg->job_log ? "yes" : "no", rc, h0, h1);
			append_exp_row(ep, line);
		}
	}

	if (cfg->on_finished) cfg->on_finished(cfg->cb_ctx, rc);
	return rc;
}
