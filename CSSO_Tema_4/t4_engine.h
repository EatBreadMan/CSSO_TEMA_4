/* t4_engine.h - Coordinator execution engine (shared by t4_coord CLI and t4_gui) */
#ifndef T4_ENGINE_H
#define T4_ENGINE_H

#include <windows.h>
#include <stdint.h>
#include "t4_common.h"
#include "t4_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	wchar_t days_dir[MAX_PATH];
	wchar_t results_dir[MAX_PATH];
	wchar_t session[40];
	int K;
	uint32_t capacity;
	DWORD delay_ms;
	int experiment;
	int job_log;
	uint32_t repetitions;

	/* External handle to receive stop event */
	HANDLE* out_hStop;

	/* Callbacks for GUI / monitoring (can be NULL) */
	void* cb_ctx;
	void (*on_worker_started)(void* ctx, int worker_id, DWORD pid);
	void (*on_day_start)(void* ctx, int day, uint32_t total_jobs);
	void (*on_progress)(void* ctx, int day, uint32_t done_jobs, uint32_t total_jobs, const uint32_t* worker_done_counts);
	void (*on_day_done)(void* ctx, int day, uint32_t jobs, unsigned long long duration_ms);
	void (*on_worker_dead)(void* ctx, int worker_id, DWORD pid);
	void (*on_log_line)(void* ctx, const char* line);
	void (*on_finished)(void* ctx, int exit_code);
} T4_COORD_CONFIG;

/* Request engine to signal stop (thread-safe) */
void t4_engine_request_stop(void);

/* Runs coordinator session. Returns exit code: 0 = OK, 1 = Stop/Ctrl+C, 2 = bad input, 3 = fatal error */
int t4_engine_run(const T4_COORD_CONFIG* cfg);

#ifdef __cplusplus
}
#endif

#endif
