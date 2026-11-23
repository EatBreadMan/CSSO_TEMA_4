/* t4_coord.cpp - Command line coordinator for T4 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t4_engine.h"

int wmain(int argc, wchar_t** argv)
{
	if (argc < 3) {
		fwprintf(stderr, L"usage: t4_coord <days_dir> <num_workers> [options]\n");
		return 2;
	}

	T4_COORD_CONFIG cfg;
	memset(&cfg, 0, sizeof(cfg));

	wcsncpy(cfg.days_dir, argv[1], MAX_PATH - 1);
	cfg.K = _wtoi(argv[2]);
	cfg.capacity = T4_CAPACITY;
	cfg.repetitions = 5;
	cfg.job_log = 1;
	cfg.delay_ms = 0;
	cfg.experiment = 0;

	int haveRes = 0;
	for (int i = 3; i < argc; i++) {
		const wchar_t* o = argv[i];
		int more = i + 1 < argc;
		if (!wcscmp(o, L"--results") && more) {
			wcsncpy(cfg.results_dir, argv[++i], MAX_PATH - 1);
			haveRes = 1;
		}
		else if (!wcscmp(o, L"--session") && more) {
			wcsncpy(cfg.session, argv[++i], 39);
		}
		else if (!wcscmp(o, L"--delay-ms") && more) {
			cfg.delay_ms = wcstoul(argv[++i], NULL, 10);
		}
		else if (!wcscmp(o, L"--capacity") && more) {
			cfg.capacity = wcstoul(argv[++i], NULL, 10);
		}
		else if (!wcscmp(o, L"--repetitions") && more) {
			cfg.repetitions = wcstoul(argv[++i], NULL, 10);
		}
		else if (!wcscmp(o, L"--experiment") && more) {
			if (wcscmp(argv[++i], L"throughput")) return 2;
			cfg.experiment = 1;
		}
		else if (!wcscmp(o, L"--log") && more) {
			const wchar_t* v = argv[++i];
			if (!wcscmp(v, L"no")) cfg.job_log = 0;
			else if (wcscmp(v, L"yes")) return 2;
		}
		else {
			return 2;
		}
	}

	if (cfg.K < 1 || cfg.K >(int)T4_MAX_WORKERS) return 2;
	if (cfg.capacity < 1 || cfg.capacity > T4_CAPACITY || cfg.repetitions < 1) return 2;
	if (cfg.session[0]) {
		size_t l = wcslen(cfg.session);
		if (l > 32 || wcsspn(cfg.session, L"0123456789abcdefABCDEF") != l) return 2;
	}

	if (!haveRes) {
		wcscpy(cfg.results_dir, cfg.experiment ? L"C:\\Faculty\\CSSO\\T4\\Experiments" : L"C:\\Faculty\\CSSO\\T4\\Results");
	}

	return t4_engine_run(&cfg);
}
