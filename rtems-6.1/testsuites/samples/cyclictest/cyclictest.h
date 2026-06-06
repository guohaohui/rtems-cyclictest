/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  cyclictest.h — RTEMS port of cyclictest from rt-tests-2.10
 *
 *  Data structures, constants, and function declarations.
 *  Kept consistent with the original cyclictest naming conventions.
 *
 *  Removed vs original:
 *   - NUMA support: RTEMS has no NUMA, single memory domain
 *   - SMI counter: /dev/cpu/N/msr not available on RTEMS
 *   - cpuidle/deepest-idle-state: Linux C-state management, not applicable
 *   - /dev/cpu_dma_latency: Linux PM QoS, no equivalent on RTEMS
 *   - /proc/loadavg: Linux procfs, no equivalent on RTEMS
 *   - ftrace/tracemark: Linux tracing, not available on RTEMS
 *   - shm_open/mmap rstat: Linux shared memory, RTEMS uses direct memory
 *   - NUMA functions (rt_numa_*): all no-op on RTEMS
 */

#ifndef _CYCLICTEST_H
#define _CYCLICTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>

#include <sys/types.h>

/* ===== Time constants ===== */
#define MSEC_PER_SEC        1000
#define USEC_PER_SEC        1000000
#define NSEC_PER_SEC        1000000000LL
#define USEC_TO_NSEC(u)     ((u) * 1000)
#define NSEC_TO_USEC(n)     ((n) / 1000)

/* ===== Defaults ===== */
#define DEFAULT_INTERVAL    1000     /* us */
#define DEFAULT_DISTANCE    500

#ifndef SCHED_IDLE
#define SCHED_IDLE          5
#endif
#ifndef SCHED_NORMAL
#define SCHED_NORMAL        SCHED_OTHER
#endif
#ifndef SCHED_BATCH
#define SCHED_BATCH         3
#endif

/* ===== Measurement modes ===== */
#define MODE_CYCLIC             0     /* POSIX timer (timer_create) */
#define MODE_CLOCK_NANOSLEEP    1     /* clock_nanosleep */
#define MODE_SYS_ITIMER         2     /* setitimer — NOT on RTEMS */
#define MODE_SYS_NANOSLEEP      3     /* nanosleep — NOT on RTEMS */
#define MODE_SYS_OFFSET         2

#define TIMER_RELTIME           0
// TIMER_ABSTIME is defined in <time.h> (value 4 on RTEMS)

/* ===== Histogram ===== */
#define HIST_MAX                1000000

/* ===== Value buffer (power of 2) ===== */
#define VALBUF_SIZE             16384

/* ===== Max path lengths ===== */
#define MAX_PATH                256

#define ARRAY_SIZE(x)           (sizeof(x) / sizeof((x)[0]))

#ifdef __rtems__
  /*
   * RTEMS: no NUMA hardware abstraction layer.
   * All memory is in a single domain. The numa variable in cyclictest.c
   * is initialized to 0; all if(numa) blocks compile to dead code.
   * Reason: RTEMS runs on embedded/RT systems without NUMA topology.
   */

  /*
   * RTEMS: no x86 SMI counter via /dev/cpu/N/msr.
   * The smi variable in cyclictest.c is initialized to 0.
   * Reason: /dev/cpu/N/msr is a Linux-specific device interface.
   */

  /*
   * RTEMS: gettid() replaced by thread ID from pthread_self().
   * Reason: Linux gettid() is a syscall; RTEMS POSIX threads provide
   * pthread_self() which uniquely identifies each thread.
   */
  static inline pid_t gettid(void)
  {
    return (pid_t)(uintptr_t)pthread_self();
  }

#else
  #include <sys/syscall.h>
  /* Original Linux gettid() */
  static inline pid_t gettid(void)
  {
    return (pid_t)syscall(SYS_gettid);
  }
#endif

/* ===== Data structures ===== */

/* Parameters passed to each measurement thread */
struct thread_param {
  int           prio;
  int           policy;
  int           mode;
  int           timermode;
  int           signal;
  int           clock;
  unsigned long max_cycles;
  struct thread_stat *stats;
  int           bufmsk;
  unsigned long interval;
  int           cpu;
  int           node;            /* NUMA node, -1 = none */
  int           tnum;            /* thread index */
  int           msr_fd;          /* SMI fd, -1 = none */
};

/* Per-thread statistics */
struct thread_stat {
  unsigned long cycles;
  unsigned long cyclesread;
  long          min;
  long          max;
  long          act;
  double        avg;
  long         *values;          /* value buffer for verbose mode */
  long         *smis;            /* SMI values, NULL if !smi */
  struct histogram *hist;        /* per-thread histogram */
  pthread_t     thread;
  int           threadstarted;
  int           tid;
  long          reduce;
  long          redmax;
  long          cycleofmax;
  unsigned long smi_count;
};

/* Spike/trigger recording */
struct thread_trigger {
  int    cpu;
  int    tnum;
  int64_t ts;
  int    diff;
  struct thread_trigger *next;
};

/* Forward declaration for histogram types (defined in histogram.h) */
struct histogram;
struct histoset;

/* ===== Enums ===== */
enum {
  AFFINITY_UNSPECIFIED = 0,
  AFFINITY_SPECIFIED   = 1,
  AFFINITY_USEALL      = 2,
};

/* ===== External globals shared across files ===== */
extern int clocksources[];

/* ===== Function declarations ===== */

/* from cyclictest.c */
void *timerthread(void *param);
void display_help(int error);
void handlepolicy(char *polname);
char *policyname(int policy);
void process_options(int argc, char *argv[], int max_cpus);
int  check_timer(void);
void sighand(int sig);
void print_tids(struct thread_param *par[], int nthreads);
void print_hist(struct thread_param *par[], int nthreads);
void print_stat(FILE *fp, struct thread_param *par, int index,
                int verbose, int quiet);
void write_stats(FILE *f, void *data);
void rt_init(int argc, char *argv[]);
void rt_write_json(const char *filename, int return_code,
                   void (*cb)(FILE *, void *), void *data);
int  parse_time_string(char *val);
const char *policy_to_string(int policy);
uint32_t string_to_policy(const char *str);

/* from init.c */
int cyclictest_main(int argc, char *argv[]);

#endif /* _CYCLICTEST_H */
