/* t4_worker.c - PERSOANA B
 * t4_worker.exe <session> <worker_id> <results_dir> [--delay-ms T] [--no-job-log]
 *
 * =====================================================================================
 *  IDEEA GENERALA (citeste asta intai)
 * =====================================================================================
 *  Workerul este un "muncitor". Face mereu acelasi lucru intr-o bucla:
 *     1. asteapta sa apara un job pe banda (coada ring din memoria partajata)
 *     2. ia jobul de pe banda
 *     3. calculeaza (Collatz pe intervalul [a, b])
 *     4. scrie rezultatul in tabelul results[] din memoria partajata
 *  Se opreste cand coordonatorul ridica evenimentul "stop".
 *
 *  MEMORIA PARTAJATA are doua zone importante:
 *     - ring[]    : banda cu N locuri; aici stau joburile care ASTEAPTA sa fie luate
 *     - results[] : tabelul cu un rand per job al zilei (stare + rezultat)
 *  Locul de pe banda se elibereaza imediat ce un job e luat, de aceea rezultatul
 *  NU se scrie acolo, ci in results[job.index].
 *
 *  OBIECTELE KERNEL (Windows) cu care se coordoneaza procesele:
 *     mtx   = "cheia de la tabla": doar un proces la un moment dat citeste/scrie memoria partajata
 *     empty = contor: cate locuri LIBERE sunt pe banda     (workerul il CRESTE dupa ce ia un job)
 *     full  = contor: cate joburi sunt PE banda            (workerul il ASTEAPTA ca sa ia un job)
 *     done  = contor de notificari "am terminat un job"    (workerul il CRESTE ca sa trezeasca coordonatorul)
 *     log   = cheia de la fisierul log.txt
 *     stop  = steag "opriti-va"
 * =====================================================================================
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "t4_lib.h"

 /* Verifica header-ul din memoria partajata. Fisierele partajate intre programe compilate
  * separat sunt ca un format de fisier: daca magic/versiune/dimensiuni nu se potrivesc,
  * mai bine refuzam decat sa citim gunoi. */
static int header_ok(const T4_AREA* z)
{
	const T4_HEADER* h = &z->header;
	return h->magic == T4_MAGIC && h->version == T4_VERSION && h->header_size == sizeof(T4_HEADER) &&
		h->total_size == sizeof(T4_AREA) && h->initialized == 1 &&
		h->capacity >= 1 && h->capacity <= T4_CAPACITY &&
		(uint32_t)(h->published - h->consumed) <= h->capacity;
}

/* Ia mutexul ("cheia"). Daca altcineva o are, procesul nostru e pus LA SOMN de kernel
 * (0% CPU) pana cand cheia e data inapoi.
 * WAIT_ABANDONED = procesul care avea cheia a MURIT fara sa o dea inapoi => datele pot fi
 * pe jumatate scrise. Regula temei: e eroare fatala (workerul iese cu cod 3). */
static BOOL lock(HANDLE m)
{
	return WaitForSingleObject(m, INFINITE) == WAIT_OBJECT_0;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 4) return 2;
	const wchar_t* ses = argv[1];
	int id = _wtoi(argv[2]);
	DWORD delay = 0;
	int jobLog = 1;
	if (id < 1 || id >(int)T4_MAX_WORKERS) return 2;
	for (int i = 4; i < argc; i++) {
		if (!wcscmp(argv[i], L"--delay-ms") && i + 1 < argc) delay = wcstoul(argv[++i], NULL, 10);
		else if (!wcscmp(argv[i], L"--no-job-log")) jobLog = 0;
		else return 2;
	}

	/* Ctrl+C se trimite TUTUROR proceselor din aceeasi consola. Nu vrem sa murim brusc
	 * (am putea muri tinand mutexul). Il ignoram: ne oprim doar cand vine evenimentul "stop". */
	SetConsoleCtrlHandler(NULL, TRUE);

	/* --- Deschidem obiectele create de coordonator (Open*, NU Create*). ---
	 * Workerul nu creeaza niciodata obiecte: daca ar crea un obiect lipsa, ar crea unul gol
	 * si s-ar rataci. Cerem DREPTURI MINIME: de ex. "full" doar cu SYNCHRONIZE (doar asteptam
	 * pe el), pentru ca un worker defect sa nu poata strica ce nu trebuie. */
	wchar_t n[96];
	HANDLE hMap, mtx, empty, full, done, logm, stop;
	t4_name(n, 96, ses, T4_SUFFIX_MEM);   hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, n);
	t4_name(n, 96, ses, T4_SUFFIX_MTX);   mtx = OpenMutexW(SYNCHRONIZE, FALSE, n);
	t4_name(n, 96, ses, T4_SUFFIX_EMPTY); empty = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, n);
	t4_name(n, 96, ses, T4_SUFFIX_FULL);  full = OpenSemaphoreW(SYNCHRONIZE, FALSE, n);   /* doar asteapta */
	t4_name(n, 96, ses, T4_SUFFIX_DONE);  done = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, n);
	t4_name(n, 96, ses, T4_SUFFIX_LOG);   logm = OpenMutexW(SYNCHRONIZE, FALSE, n);
	t4_name(n, 96, ses, T4_SUFFIX_STOP);  stop = OpenEventW(SYNCHRONIZE, FALSE, n);
	if (!hMap || !mtx || !empty || !full || !done || !logm || !stop) return 3;   /* handle-urile se inchid la exit */

	/* "Mapam" memoria partajata in spatiul nostru de adrese. Adresa 'z' e valida DOAR in
	 * procesul nostru (in coordonator aceeasi memorie are alta adresa) => in memoria
	 * partajata nu avem voie sa punem pointeri, ci doar indici. */
	T4_AREA* z = (T4_AREA*)MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(T4_AREA));
	if (!z) return 3;

	int rc = 0;
	char src[8];
	snprintf(src, sizeof src, "W%d", id);
	wchar_t logpath[MAX_PATH];
	t4_join(logpath, MAX_PATH, argv[3], L"log.txt");
	if (!log_open(logpath, logm, FALSE)) { rc = 3; goto out; }

	/* Validam header-ul SUB mutex (citim memoria partajata => cheia in mana). */
	if (!lock(mtx)) { rc = 3; goto out; }
	int ok = header_ok(z);
	ReleaseMutex(mtx);
	if (!ok) { rc = 3; goto out; }

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);          /* pentru masurarea duratei cu QPC */

	for (;;) {
		/* ---------------------------------------------------------------------------
		 * PASUL 1: ASTEPTAM (fara busy waiting!)
		 * ---------------------------------------------------------------------------
		 * WaitForMultipleObjects pune procesul LA SOMN si il trezeste cand ORICARE dintre
		 * obiecte devine "semnalat":
		 *    - stop  : coordonatorul a ordonat oprirea
		 *    - full  : exista cel putin un job pe banda (contorul full > 0)
		 * Cat timp nu e nimic de facut, workerul NU ruleaza deloc (0% CPU): kernelul tine
		 * o lista cu cine asteapta ce, iar cine modifica obiectul (ReleaseSemaphore /
		 * SetEvent) trezeste automat un proces din lista. Asta e diferenta fata de o
		 * bucla "while (!gata) {}" care ar consuma 100% CPU verificand la nesfarsit.
		 *
		 * Cand asteptam pe "full", functia SCADE contorul cu 1 daca era > 0 ("am rezervat
		 * un job"). Daca erau 3 joburi si 3 workeri se trezesc, fiecare primeste cate unul.
		 *
		 * 'stop' e la indexul 0: daca ambele sunt semnalate odata, se intoarce cel cu indexul
		 * mai mic, deci oprirea are prioritate. Comparam mereu cu WAIT_OBJECT_0 + i. */
		HANDLE hs[2] = { stop, full };
		DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
		if (w == WAIT_OBJECT_0) break;                       /* stop => iesim normal, cod 0 */
		if (w != WAIT_OBJECT_0 + 1) { rc = 3; break; }       /* WAIT_FAILED etc. => eroare */

		/* ---------------------------------------------------------------------------
		 * PASUL 2: LUAM JOBUL DE PE BANDA  (sub mutex)
		 * ---------------------------------------------------------------------------
		 * REGULA DE AUR: intai semaforul (pasul 1), abia apoi mutexul. Daca am lua cheia
		 * si apoi am astepta un job, am dormi TINAND cheia; coordonatorul nu ar mai putea
		 * pune joburi (are nevoie de aceeasi cheie) => deadlock, toti blocati. */
		if (!lock(mtx)) { rc = 3; break; }

		/* Banda e circulara: pozitia de citire = consumed % capacity. 'consumed' creste
		 * mereu (contor monoton); '%' il aduce inapoi in 0..capacity-1. Capacitatea se citeste
		 * din header (poate fi 1..16 cu --capacity), NU din constanta T4_CAPACITY. */
		uint32_t cap = z->header.capacity;
		T4_JOB job = z->ring[z->header.consumed % cap];   /* COPIEM jobul: locul de pe banda se va elibera */
		z->header.consumed++;

		/* Marcam jobul ca IN_PROGRESS in tabelul de rezultate (results[job.index]).
		 * Verificam ca jobul apartine zilei curente si e QUEUED, ca sa nu atingem date vechi. */
		int mine = 0;
		if (job.day == z->header.current_day && job.index < z->header.job_count &&
			z->results[job.index].state == T4_QUEUED) {
			T4_RESULT* r = &z->results[job.index];
			r->state = T4_IN_PROGRESS; r->worker = (uint32_t)id; r->worker_pid = GetCurrentProcessId();
			mine = 1;
		}

		/* Locul de pe banda e acum liber: crestem "empty" cu 1. Daca coordonatorul dormea
		 * pentru ca banda era plina, ACUM e trezit de kernel. */
		ReleaseSemaphore(empty, 1, NULL);
		ReleaseMutex(mtx);                       /* dam cheia inapoi cat mai repede */
		if (!mine) continue;

		/* ---------------------------------------------------------------------------
		 * PASUL 3: CALCULUL, IN AFARA MUTEXULUI
		 * ---------------------------------------------------------------------------
		 * Calculul poate dura mult. Daca l-am face cu cheia in mana, ceilalti workeri ar
		 * astepta degeaba si n-ar mai exista paralelism. Cheia se tine doar cat dureaza
		 * citirea/scrierea memoriei partajate, nu calculul. */
		LARGE_INTEGER t0, t1;
		uint64_t nmax, steps;
		QueryPerformanceCounter(&t0);
		int st = t4_collatz_range(job.a, job.b, &nmax, &steps);
		QueryPerformanceCounter(&t1);
		uint64_t us = (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
		if (delay) Sleep(delay);   /* doar pentru demonstratii (timp sa apesi Ctrl+C / sa omori un worker) */

		/* ---------------------------------------------------------------------------
		 * PASUL 4: SCRIEM REZULTATUL
		 * ---------------------------------------------------------------------------
		 * JOB_DONE se scrie in log inainte de DONE: ziua nu se poate termina inainte de
		 * ultima linie JOB_DONE (ordine cronologica corecta in log). */
		if (jobLog && !log_line(src, "JOB_DONE", "day=%u id=%u duration_us=%llu", job.day, job.id,
			(unsigned long long)us)) {
			rc = 3; break;
		}
		if (!lock(mtx)) { rc = 3; break; }
		T4_RESULT* r = &z->results[job.index];   /* rezultatul merge la results[index], NU pe banda */
		r->status = (uint32_t)st; r->n_max = nmax; r->max_steps = steps; r->duration_us = us;
		r->state = T4_DONE;
		ReleaseMutex(mtx);

		/* Notificam coordonatorul: "am terminat un job". Creste contorul 'done'; coordonatorul,
		 * care doarme pe {stop, workeri, done}, se trezeste si numara joburile DONE din tabel.
		 * Notificarea doar il TREZESTE; ce e adevarat se afla din tabel, nu din numaratoarea
		 * notificarilor (un worker ar putea muri intre doua operatii si cifra ar fi gresita). */
		ReleaseSemaphore(done, 1, NULL);
	}
out:
	/* Curatenie: eliberam in ordine inversa (view-ul, apoi handle-urile). */
	log_close();
	UnmapViewOfFile(z);
	CloseHandle(hMap); CloseHandle(mtx); CloseHandle(empty); CloseHandle(full);
	CloseHandle(done); CloseHandle(logm); CloseHandle(stop);
	return rc;
}
