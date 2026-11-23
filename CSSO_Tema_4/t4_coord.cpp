/* t4_coord.c - PERSOANA A
 * t4_coord.exe <days_dir> <K> [--results d] [--session hex] [--delay-ms T] [--capacity N]
 *              [--experiment throughput] [--log yes|no] [--repetitions R]
 *
 * =====================================================================================
 *  IDEEA GENERALA
 * =====================================================================================
 *  Coordonatorul este "seful". El:
 *     1. creeaza memoria partajata si obiectele de sincronizare (sesiunea)
 *     2. porneste K muncitori (t4_worker.exe)
 *     3. pentru fiecare zi: citeste joburile din day_XX.tsv, le pune pe banda (ring),
 *        apoi asteapta sa fie gata TOATE joburile zilei
 *     4. scrie rezultatele zilei, trece la ziua urmatoare
 *     5. la final (sau la Ctrl+C / worker mort) opreste muncitorii controlat
 *
 *  MEMORIA PARTAJATA (T4_AREA):
 *     - header    : magic, versiune, capacitate, contoarele published / consumed, ziua curenta
 *     - ring[]    : banda cu 'capacity' locuri cu joburile care ASTEAPTA
 *     - results[] : cate un rand per job al zilei: stare (FREE/QUEUED/IN_PROGRESS/DONE) + rezultat
 *
 *  CONTOARELE (semafoare):
 *     empty = locuri LIBERE pe banda (incepe la capacity). Seful il ASTEAPTA inainte sa puna un job.
 *     full  = joburi PE banda (incepe la 0).               Seful il CRESTE dupa ce pune un job.
 *     done  = notificari "un job s-a terminat".            Seful il ASTEAPTA ca sa afle ca sunt rezultate noi.
 *     Mereu: empty + full = capacity.
 *
 *  CE ESTE O ASTEPTARE (nu e busy waiting):
 *     WaitForMultipleObjects pune procesul LA SOMN (0% CPU). Kernelul il trezeste cand un
 *     obiect din lista devine semnalat. Cine schimba obiectul (ReleaseSemaphore, SetEvent,
 *     sau moartea unui proces) trezeste automat procesul care asteapta; nimeni nu verifica in bucla.
 * =====================================================================================
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "t4_lib.h"

 /* Tot ce tine de o sesiune: handle-urile obiectelor, view-ul de memorie, procesele workerilor. */
typedef struct {
	wchar_t ses[40];
	HANDLE mem, mtx, empty, full, done, logm, stop;
	T4_AREA* z;
	HANDLE proc[T4_MAX_WORKERS];      /* hProcess al fiecarui worker: devine SEMNALAT cand workerul MOARE */
	DWORD  pid[T4_MAX_WORKERS];
	int K, started, dead;             /* dead = id-ul (1..K) workerului mort, 0 = nimeni */
} Sess;

static HANDLE g_stop;
static LARGE_INTEGER g_freq;

/* Handler Ctrl+C. Ruleaza pe un thread creat de sistem, deci face MINIMUM: ridica steagul
 * 'stop'. Firul principal (care doarme in WaitForMultipleObjects pe stop) se trezeste si curata. */
static BOOL WINAPI on_ctrl(DWORD t)
{
	if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) { SetEvent(g_stop); return TRUE; }
	return FALSE;
}

/* Create* cu un nume care EXISTA DEJA nu da eroare: intoarce handle la obiectul existent si
 * seteaza GetLastError() == ERROR_ALREADY_EXISTS (si ignora parametrii nostri!). Ar insemna ca
 * ne atasam la obiectul altcuiva (o sesiune veche sau un atacator care a creat numele primul,
 * "squatting"). Coordonatorul e singurul creator si refuza in acest caz. */
static HANDLE fresh(HANDLE h)
{
	if (!h) return NULL;
	if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(h); return NULL; }
	return h;
}

/* Creeaza cele 7 obiecte ale sesiunii. */
static BOOL create_session(Sess* s, uint32_t cap)
{
	wchar_t n[96];
	/* mem: memorie partajata sustinuta de paging file (INVALID_HANDLE_VALUE, NU NULL); porneste umpluta cu zero */
	t4_name(n, 96, s->ses, T4_SUFFIX_MEM);
	s->mem = fresh(CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(T4_AREA), n));
	/* mtx: "cheia de la tabla"; FALSE = nu e detinuta la creare */
	t4_name(n, 96, s->ses, T4_SUFFIX_MTX);   s->mtx = fresh(CreateMutexW(NULL, FALSE, n));
	/* empty: initial = capacity (toate locurile libere), maxim = capacity */
	t4_name(n, 96, s->ses, T4_SUFFIX_EMPTY); s->empty = fresh(CreateSemaphoreW(NULL, (LONG)cap, (LONG)cap, n));
	/* full: initial 0 (niciun job pe banda), maxim = capacity */
	t4_name(n, 96, s->ses, T4_SUFFIX_FULL);  s->full = fresh(CreateSemaphoreW(NULL, 0, (LONG)cap, n));
	/* done: contor de notificari, fara limita practica */
	t4_name(n, 96, s->ses, T4_SUFFIX_DONE);  s->done = fresh(CreateSemaphoreW(NULL, 0, 0x7FFFFFFF, n));
	/* log: cheia de la log.txt */
	t4_name(n, 96, s->ses, T4_SUFFIX_LOG);   s->logm = fresh(CreateMutexW(NULL, FALSE, n));
	/* stop: eveniment MANUAL-RESET: ramane semnalat dupa SetEvent si trezeste TOTI cei care asteapta,
	 * inclusiv pe cei care ajung mai tarziu (potrivit pentru "oprire") */
	t4_name(n, 96, s->ses, T4_SUFFIX_STOP);  s->stop = fresh(CreateEventW(NULL, TRUE, FALSE, n));
	return s->mem && s->mtx && s->empty && s->full && s->done && s->logm && s->stop;
}

/* Elibereaza totul: view-ul, handle-urile obiectelor si hProcess-urile workerilor. */
static void close_session(Sess* s)
{
	if (s->z) UnmapViewOfFile(s->z);
	s->z = NULL;
	HANDLE* h[] = { &s->mem, &s->mtx, &s->empty, &s->full, &s->done, &s->logm, &s->stop };
	for (int i = 0; i < 7; i++) if (*h[i]) { CloseHandle(*h[i]); *h[i] = NULL; }
	for (int i = 0; i < s->started; i++) CloseHandle(s->proc[i]);
	s->started = 0;
}

/* Ia "cheia" (mutexul) pentru memoria partajata. Daca altcineva o are, dormim pana o da inapoi.
 * WAIT_ABANDONED = fostul proprietar a murit tinand-o; datele pot fi corupte => eroare fatala. */
static BOOL lock_mtx(HANDLE m)
{
	DWORD w = WaitForSingleObject(m, INFINITE);
	if (w == WAIT_OBJECT_0) return TRUE;
	if (w == WAIT_ABANDONED) ReleaseMutex(m);
	return FALSE;
}

/* OPRIRE CONTROLATA a workerilor:
 *   1. ridica 'stop' (toti workerii adorm pe {stop, full}; se trezesc si ies singuri)
 *   2. asteapta pana la 5 secunde sa iasa TOTI (bAll = TRUE)
 *   3. doar daca vreunul nu s-a oprit, TerminateProcess pe SUPRAVIETUITORI (ultima solutie) */
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

/* Porneste K workeri cu CreateProcessW. */
static BOOL start_workers(Sess* s, const wchar_t* resdir, DWORD delay, int jobLog)
{
	wchar_t exe[MAX_PATH], cmd[2048], ds[40] = L"";
	if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return FALSE;
	wchar_t* bs = wcsrchr(exe, L'\\');
	wcscpy(bs ? bs + 1 : exe, L"t4_worker.exe");     /* t4_worker.exe sta langa t4_coord.exe */
	if (delay) swprintf(ds, 40, L" --delay-ms %lu", delay);
	for (int i = 1; i <= s->K; i++) {
		STARTUPINFOW si = { sizeof si };
		PROCESS_INFORMATION pi;
		swprintf(cmd, 2048, L"\"%ls\" %ls %d \"%ls\"%ls%ls", exe, s->ses, i, resdir, ds,
			jobLog ? L"" : L" --no-job-log");
		/* CREATE_NEW_PROCESS_GROUP: Ctrl+C din consola nu ajunge la worker (se opreste doar prin 'stop') */
		if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi))
			return FALSE;
		CloseHandle(pi.hThread);          /* hThread nu ne trebuie */
		/* hProcess il PASTRAM: un handle de proces devine semnalat cand procesul se termina. Il punem
		 * in lista de asteptare => aflam IMEDIAT ca un worker a murit, fara sa intrebam periodic. */
		s->proc[s->started] = pi.hProcess;
		s->pid[s->started++] = pi.dwProcessId;
		log_line("COORD", "WORKER_STARTED", "worker=%d pid=%lu", i, pi.dwProcessId);
	}
	return TRUE;
}

/* Numara joburile DONE din tabel (sub mutex). Asa decidem ca o zi e gata: din DATE, nu din
 * numarul de notificari 'done' (care poate fi gresit daca un worker moare intre doua operatii). */
static uint32_t count_done(Sess* s, uint32_t n, BOOL* err)
{
	uint32_t c = 0;
	if (!lock_mtx(s->mtx)) { *err = TRUE; return 0; }
	for (uint32_t i = 0; i < n; i++) if (s->z->results[i].state == T4_DONE) c++;
	ReleaseMutex(s->mtx);
	return c;
}

/* ASTEPTAREA CENTRALA a coordonatorului. Adoarme pe {stop, worker1..workerK, obj} si se trezeste
 * cand ORICARE devine semnalat:
 *     index 0        : stop           -> Ctrl+C (singurul care il seteaza cat timp lucram e handler-ul)
 *     index 1..K     : un worker      -> un worker a MURIT (nu ar trebui sa se termine singur)
 *     index K+1      : obj (empty/done) -> ce asteptam de fapt (loc liber / job terminat)
 * Intoarce: 0 = obj semnalat, 1 = stop (Ctrl+C), 3 = worker mort sau eroare.
 * 'stop' e la indexul 0 ca sa aiba prioritate daca mai multe sunt semnalate deodata. */
static int wait3(Sess* s, HANDLE obj)
{
	HANDLE hs[2 + T4_MAX_WORKERS];
	hs[0] = s->stop;
	for (int i = 0; i < s->K; i++) hs[1 + i] = s->proc[i];
	hs[1 + s->K] = obj;
	DWORD w = WaitForMultipleObjects((DWORD)(2 + s->K), hs, FALSE, INFINITE);
	if (w == WAIT_OBJECT_0) return 1;
	if (w == WAIT_OBJECT_0 + 1 + (DWORD)s->K) return 0;
	if (w > WAIT_OBJECT_0 && w <= WAIT_OBJECT_0 + (DWORD)s->K) {
		int i = (int)(w - WAIT_OBJECT_0) - 1;     /* ce index din vector (WAIT_OBJECT_0 + i) */
		s->dead = i + 1;
		log_line("COORD", "WORKER_DEAD", "worker=%d pid=%lu", i + 1, s->pid[i]);
	}
	return 3;
}

/* PROCESEAZA O ZI. Intoarce: 0 = completa, 1 = Ctrl+C, 3 = fatal.
 * *secs = durata de la DAY_START pana cand toate joburile sunt DONE (masurata cu QPC). */
static int run_day(Sess* s, int day, const T4_INJOB* jobs, uint32_t n, double* secs)
{
	T4_AREA* z = s->z;
	BOOL err = FALSE;

	/* --- Pregatirea zilei: tabelul de rezultate (sub mutex) --- */
	if (!lock_mtx(s->mtx)) return 3;
	memset(z->results, 0, sizeof z->results);
	z->header.current_day = (uint32_t)day;
	z->header.job_count = n;
	for (uint32_t i = 0; i < n; i++) { z->results[i].state = T4_FREE; z->results[i].id = jobs[i].id; }
	ReleaseMutex(s->mtx);
	if (!log_line("COORD", "DAY_START", "day=%d jobs=%u", day, n)) return 3;
	LARGE_INTEGER t0, t1;
	QueryPerformanceCounter(&t0);

	/* --- BUCLA 1: PUBLISH --- punem pe banda cate un job, pe rand.
	 * Pentru fiecare job:
	 *   (a) asteptam un loc LIBER (semaforul 'empty'). Daca banda e plina (empty = 0), dormim 0% CPU
	 *       pana cand un worker ia un job si creste 'empty'.
	 *   (b) abia APOI luam mutexul (INTAI SEMAFORUL, APOI MUTEXUL!). Daca am dormi pe 'empty' TINAND
	 *       cheia, workerii n-ar mai putea sa ia joburi (au nevoie de cheie) si n-ar elibera locuri
	 *       => deadlock.
	 *   (c) scriem jobul in ring[published % capacity] (banda e circulara; 'published' creste mereu).
	 *   (d) marcam results[pub] = QUEUED
	 *   (e) dam cheia inapoi, apoi crestem 'full' => trezim un worker care dormea fiindca banda era goala. */
	for (uint32_t pub = 0; pub < n; ) {
		int r = wait3(s, s->empty);
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

	/* --- BUCLA 2: FINISH --- asteptam sa fie gata toate joburile zilei.
	 * La fiecare trezire numaram joburile DONE din tabel. Notificarea 'done' doar TREZESTE
	 * coordonatorul; ce e adevarat se vede in tabel. Verificam si INAINTE de primul wait:
	 * o zi fara joburi valide (n = 0) e deja completa, iar notificarile venite in bucla 1 stau
	 * deja in semafor si nu se pierd. */
	for (;;) {
		uint32_t c = count_done(s, n, &err);
		if (err) return 3;
		if (c == n) break;
		int r = wait3(s, s->done);
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

int wmain(int argc, wchar_t** argv)
{
	/* ---------- 1. Argumentele (R1) ---------- */
	if (argc < 3) { fwprintf(stderr, L"usage: t4_coord <days_dir> <K> [options]\n"); return 2; }
	const wchar_t* days = argv[1];
	int K = _wtoi(argv[2]);
	wchar_t resdir[MAX_PATH] = L"", sesArg[40] = L"";
	DWORD delay = 0, cap = T4_CAPACITY, reps = 5;
	int experiment = 0, jobLog = 1, haveRes = 0;
	if (K < 1 || K >(int)T4_MAX_WORKERS) return 2;
	for (int i = 3; i < argc; i++) {
		const wchar_t* o = argv[i];
		int more = i + 1 < argc;
		if (!wcscmp(o, L"--results") && more) { wcsncpy(resdir, argv[++i], MAX_PATH - 1); haveRes = 1; }
		else if (!wcscmp(o, L"--session") && more) { wcsncpy(sesArg, argv[++i], 39); }
		else if (!wcscmp(o, L"--delay-ms") && more)  delay = wcstoul(argv[++i], NULL, 10);
		else if (!wcscmp(o, L"--capacity") && more)  cap = wcstoul(argv[++i], NULL, 10);
		else if (!wcscmp(o, L"--repetitions") && more) reps = wcstoul(argv[++i], NULL, 10);
		else if (!wcscmp(o, L"--experiment") && more) { if (wcscmp(argv[++i], L"throughput")) return 2; experiment = 1; }
		else if (!wcscmp(o, L"--log") && more) {
			const wchar_t* v = argv[++i];
			if (!wcscmp(v, L"no")) jobLog = 0; else if (wcscmp(v, L"yes")) return 2;
		}
		else return 2;
	}
	if (cap < 1 || cap > T4_CAPACITY || reps < 1) return 2;
	if (sesArg[0]) {
		size_t l = wcslen(sesArg);
		if (l > 32 || wcsspn(sesArg, L"0123456789abcdefABCDEF") != l) return 2;
	}
	if (!haveRes) wcscpy(resdir, experiment ? L"C:\\Faculty\\CSSO\\T4\\Experiments" : L"C:\\Faculty\\CSSO\\T4\\Results");

	/* handles_before pentru experiment: numarul de handle-uri INAINTE sa cream ceva */
	DWORD h0 = 0, h1 = 0;
	GetProcessHandleCount(GetCurrentProcess(), &h0);
	QueryPerformanceFrequency(&g_freq);

	{
		wchar_t d1[MAX_PATH]; t4_join(d1, MAX_PATH, days, L"day_01.tsv");   /* fara day_01 / director => exit 2 */
		if (GetFileAttributesW(d1) == INVALID_FILE_ATTRIBUTES) return 2;
	}
	if (!t4_mkdirs(resdir)) return 3;

	Sess s; memset(&s, 0, sizeof s);
	s.K = K;
	/* ID-ul sesiunii = PID-ul coordonatorului in hex (implicit): face numele obiectelor unice pe sesiune */
	if (sesArg[0]) wcscpy(s.ses, sesArg); else swprintf(s.ses, 40, L"%08lx", GetCurrentProcessId());
	T4_INJOB* jobs = (T4_INJOB*)malloc(T4_MAX_JOBS * sizeof(T4_INJOB));
	T4_RESULT* loc = (T4_RESULT*)malloc(T4_MAX_JOBS * sizeof(T4_RESULT));
	int rc = 3; const char* reason = NULL;
	uint32_t passes = experiment ? reps + 1 : 1, donePasses = 0;
	double pSecs[64]; uint32_t pJobs[64];
	if (passes > 64 || !jobs || !loc) { free(jobs); free(loc); return 2; }
	char sesA[40]; snprintf(sesA, sizeof sesA, "%ls", s.ses);

	/* ---------- 2. Cream sesiunea ---------- */
	if (!create_session(&s, cap)) goto cleanup;             /* ERROR_ALREADY_EXISTS => exit 3 */
	g_stop = s.stop;
	s.z = (T4_AREA*)MapViewOfFile(s.mem, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(T4_AREA));
	if (!s.z) goto cleanup;

	/* ---------- 3. Completam header-ul INAINTE sa pornim workerii ----------
	 * Cine porneste primul trebuie sa initializeze; solutia simpla: doar coordonatorul scrie header-ul,
	 * iar workerii doar il VALIDEAZA. Altfel un worker ar putea citi un header pe jumatate scris. */
	if (!lock_mtx(s.mtx)) goto cleanup;
	s.z->header.magic = T4_MAGIC; s.z->header.version = T4_VERSION;
	s.z->header.header_size = sizeof(T4_HEADER); s.z->header.total_size = sizeof(T4_AREA);
	s.z->header.capacity = cap; s.z->header.max_jobs = T4_MAX_JOBS; s.z->header.initialized = 1;
	ReleaseMutex(s.mtx);
	{
		wchar_t lp[MAX_PATH]; t4_join(lp, MAX_PATH, resdir, L"log.txt");
		if (!log_open(lp, s.logm, TRUE)) goto cleanup;
	}
	if (!experiment && !t4_summary_init(resdir)) goto cleanup;
	SetConsoleCtrlHandler(on_ctrl, TRUE);
	log_line("COORD", "SESSION_START", "session=%s workers=%d capacity=%lu", sesA, K, cap);

	/* ---------- 4. Pornim muncitorii ---------- */
	if (!start_workers(&s, resdir, delay, jobLog)) goto cleanup;

	/* ---------- 5. Procesam zilele ----------
	 * In modul normal: o singura trecere. In modul experiment: R+1 treceri (prima = warm-up). */
	rc = 0;
	for (uint32_t pass = 0; pass < passes && rc == 0; pass++) {
		double tot = 0; uint32_t totJobs = 0;
		for (int day = 1; rc == 0; day++) {
			uint32_t n, inv;
			int pr = t4_parse_day(days, day, jobs, &n, &inv);
			if (pr == T4_PARSE_NOFILE) break;                /* nu mai exista day_XX.tsv => gata */
			if (pr != T4_PARSE_OK) { rc = 3; break; }
			double secs = 0;
			int r = run_day(&s, day, jobs, n, &secs);
			if (r == 1) { rc = 1; reason = "ctrl-c"; break; }
			if (r == 3) { rc = 3; reason = s.dead ? "worker-dead" : "error"; break; }
			unsigned long long ms = (unsigned long long)(secs * 1000.0 + 0.5);
			log_line("COORD", "DAY_DONE", "day=%d jobs=%u duration_ms=%llu", day, n, ms);
			/* Copiem tabelul de rezultate SUB mutex (citire din memoria partajata), apoi scriem fisierele
			 * din copia locala, fara sa mai tinem cheia. */
			if (!lock_mtx(s.mtx)) { rc = 3; reason = "error"; break; }
			memcpy(loc, s.z->results, n * sizeof(T4_RESULT));
			ReleaseMutex(s.mtx);
			if (!t4_write_results(resdir, day, jobs, loc, n) ||
				(!experiment && !t4_summary_row(resdir, day, n, inv, loc, ms))) {
				rc = 3; reason = "error"; break;
			}
			tot += secs; totJobs += n;
		}
		if (rc == 0) { pSecs[pass] = tot; pJobs[pass] = totJobs; donePasses++; }
	}

	/* ---------- 6. Oprire si curatenie ----------
	 * Aceeasi cale pentru succes, Ctrl+C, worker mort sau orice eroare ('goto cleanup'). */
cleanup:
	SetConsoleCtrlHandler(on_ctrl, FALSE);
	if (reason) log_line("COORD", "STOP", "reason=%s", reason);
	stop_workers(&s);                                        /* SetEvent(stop) + 5 s + TerminateProcess supravietuitori */
	log_line("COORD", "SESSION_END", "code=%d", rc);         /* ultima linie din log */
	log_close();
	close_session(&s);
	free(jobs); free(loc);

	if (experiment) {                                        /* randurile se scriu DUPA handles_after */
		GetProcessHandleCount(GetCurrentProcess(), &h1);
		wchar_t ep[MAX_PATH]; t4_join(ep, MAX_PATH, resdir, L"experiments.tsv");
		if (GetFileAttributesW(ep) == INVALID_FILE_ATTRIBUTES)
			append_exp_row(ep, "run\texperiment\tparameters\trepetition\tvalue\tunit\terror_code\thandles_before\thandles_after\tdetails\n");
		char line[512];
		for (uint32_t p = 0; p < donePasses; p++) {
			snprintf(line, sizeof line, "%s\tE1\tworkers=%d;capacity=%lu;log=%s\t%u\t%.2f\tjobs/s\t0\t%lu\t%lu\tsession=%s jobs=%u duration_ms=%.0f\n",
				sesA, K, cap, jobLog ? "yes" : "no", p, pJobs[p] / pSecs[p], h0, h1, sesA, pJobs[p], pSecs[p] * 1000);
			append_exp_row(ep, line);
		}
		if (rc != 0) {
			snprintf(line, sizeof line, "%s\tE1\tworkers=%d;capacity=%lu;log=%s\t0\t0\tjobs/s\t%d\t%lu\t%lu\tfailed\n",
				sesA, K, cap, jobLog ? "yes" : "no", rc, h0, h1);
			append_exp_row(ep, line);
		}
	}
	return rc;
}
