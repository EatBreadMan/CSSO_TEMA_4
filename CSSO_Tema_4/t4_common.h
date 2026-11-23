/*
 * t4_common.h — the shared-memory format and the object names for Assignment 4 (CSSO 2026–2027).
 *
 * The file is provided to fix a common contract between the coordinator and the workers.
 * You may use it as is or adapt it, provided that you keep the rules in the specification:
 * validated header (magic, version, sizes), no pointers in shared memory, access under the mutex.
 */
#ifndef T4_COMMON_H
#define T4_COMMON_H

#include <windows.h>
#include <stdint.h>

#define T4_MAGIC        0x34543443u      /* the bytes 'C' '4' 'T' '4' in little-endian */
#define T4_VERSION      1u
#define T4_CAPACITY     16u              /* the maximum number of slots in the ring (the size of the ring[] array);
											the effective capacity N (1..T4_CAPACITY, default 16) is
											header.capacity, chosen by the coordinator with --capacity */
#define T4_MAX_JOBS     4096u            /* valid jobs per day (maximum) */
#define T4_MAX_WORKERS  16u

											/* The states of a job in the current day's results table. */
enum {
	T4_FREE = 0,   /* unused slot */
	T4_QUEUED = 1,   /* published in the ring, not yet taken by a worker */
	T4_IN_PROGRESS = 2,   /* taken by a worker */
	T4_DONE = 3    /* result written */
};

/* The result of the computation. */
enum { T4_OK = 0, T4_OVERFLOW = 1 };

typedef struct T4_JOB {          /* one ring entry: 32 bytes */
	uint32_t day;                 /* the day number (1, 2, ...) */
	uint32_t id;                  /* the id from the day's file */
	uint32_t index;               /* the position in the day's results[] table */
	uint32_t reserved;
	uint64_t a, b;                /* the interval [a, b], 1 <= a <= b */
} T4_JOB;

typedef struct T4_RESULT {       /* 48 bytes */
	uint32_t state;               /* T4_FREE / T4_QUEUED / T4_IN_PROGRESS / T4_DONE */
	uint32_t worker;              /* the id of the worker that took it (1..K) */
	uint32_t worker_pid;
	uint32_t status;              /* T4_OK or T4_OVERFLOW */
	uint32_t id;                  /* a copy of the id from the file */
	uint32_t reserved;
	uint64_t n_max;               /* the argument with the maximum number of steps (or the first n that overflows) */
	uint64_t max_steps;           /* the maximum number of steps (undefined on overflow) */
	uint64_t duration_us;         /* the duration of the computation, measured by the worker */
} T4_RESULT;

typedef struct T4_HEADER {       /* 64 bytes */
	uint32_t magic;               /* T4_MAGIC */
	uint32_t version;             /* T4_VERSION */
	uint32_t header_size;         /* sizeof(T4_HEADER) */
	uint32_t total_size;          /* sizeof(T4_AREA) */
	uint32_t capacity;            /* effective N, 1..T4_CAPACITY (default 16) */
	uint32_t max_jobs;            /* T4_MAX_JOBS */
	uint32_t published;           /* total jobs put in the ring (monotonic counter, mod 2^32) */
	uint32_t consumed;            /* total jobs taken out of the ring (monotonic counter, mod 2^32) */
	uint32_t current_day;         /* 0 = no active day */
	uint32_t job_count;           /* valid jobs in the current day */
	uint32_t initialized;         /* 1 after the coordinator has filled in the header */
	uint32_t reserved[5];
} T4_HEADER;

/* The write position in the ring is published % header.capacity, the read position consumed % header.capacity,
 * and the number of elements in the ring is published - consumed (32-bit arithmetic). The worker reads
 * the capacity from the header (it does not use the T4_CAPACITY constant for the modulo). */
typedef struct T4_AREA {
	T4_HEADER   header;
	T4_JOB      ring[T4_CAPACITY];
	T4_RESULT   results[T4_MAX_JOBS];
} T4_AREA;

#ifdef __cplusplus
static_assert(sizeof(T4_JOB) == 32, "T4_JOB must be 32 bytes");
static_assert(sizeof(T4_RESULT) == 48, "T4_RESULT must be 48 bytes");
static_assert(sizeof(T4_HEADER) == 64, "T4_HEADER must be 64 bytes");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L   /* C11/C17: cl /std:c17 */
_Static_assert(sizeof(T4_JOB) == 32, "T4_JOB must be 32 bytes");
_Static_assert(sizeof(T4_RESULT) == 48, "T4_RESULT must be 48 bytes");
_Static_assert(sizeof(T4_HEADER) == 64, "T4_HEADER must be 64 bytes");
#else                                                              /* the default MSVC C mode (no /std): same checks */
typedef char t4_assert_job[sizeof(T4_JOB) == 32 ? 1 : -1];
typedef char t4_assert_result[sizeof(T4_RESULT) == 48 ? 1 : -1];
typedef char t4_assert_header[sizeof(T4_HEADER) == 64 ? 1 : -1];
#endif

/* The object names: the prefix Local\CSSO_T4_<session>_ followed by the suffix.
 * <session> = 1 to 32 hexadecimal digits: by default the coordinator's PID as 8 lowercase hex digits
 * (for example 00002a3c for PID 10812), or the value given with --session. */
#define T4_SUFFIX_MEM    L"mem"     /* file mapping, sizeof(T4_AREA) bytes         */
#define T4_SUFFIX_MTX    L"mtx"     /* mutex: protects the whole T4_AREA            */
#define T4_SUFFIX_EMPTY  L"empty"   /* semaphore: free slots (initially N, max N)   */
#define T4_SUFFIX_FULL   L"full"    /* semaphore: jobs in the ring (initially 0, max N) */
#define T4_SUFFIX_DONE   L"done"    /* semaphore: "job finished" notifications      */
#define T4_SUFFIX_LOG    L"log"     /* mutex: protects the log.txt file             */
#define T4_SUFFIX_STOP   L"stop"    /* manual-reset event: stop                     */

#endif /* T4_COMMON_H */
