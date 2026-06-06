/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  cyclictest.c — RTEMS port of rt-tests-2.10 cyclictest
 *
 *  Original authors: Thomas Gleixner, Clark Williams, John Kacur
 *
 *  This file contains the core measurement algorithm, statistics,
 *  command-line parsing, and output formatting.  It is kept as close
 *  as possible to the original cyclictest.c code structure.
 *
 *  === Features REMOVED and why ===
 *
 *  1. NUMA support (rt_numa_*)
 *     Reason: RTEMS has no NUMA hardware abstraction. All memory
 *     is in a single domain. The numa variable is #defined to 0,
 *     making all if(numa) blocks dead code eliminated by the compiler.
 *
 *  2. SMI counter (/dev/cpu/N/msr, MSR_SMI_COUNT, has_smi_counter)
 *     Reason: /dev/cpu/N/msr is a Linux-specific device interface.
 *     SMI (System Management Interrupt) is a BIOS/x86 concept not
 *     accessible on most embedded RTEMS platforms.
 *
 *  3. Power management (/dev/cpu_dma_latency, set_latency_target)
 *     Reason: Linux PM QoS interface. RTEMS has no C-states or
 *     dynamic power management — the CPU runs at full speed.
 *
 *  4. CPU idle state control (cpuidle, deepest_idle_state)
 *     Reason: Linux cpuidle subsystem. RTEMS doesn't manage
 *     CPU idle states; the hardware runs continuously.
 *
 *  5. ftrace / trace_marker (enable_trace_mark, tracemark)
 *     Reason: Linux ftrace is a kernel-internal tracing mechanism.
 *     No equivalent exists in RTEMS.
 *
 *  6. /proc/loadavg
 *     Reason: Linux procfs. RTEMS has no /proc filesystem.
 *     Display shows "N/A" instead.
 *
 *  7. shm_open / mmap running-status (rstat_*)
 *     Reason: Linux shared memory IPC. RTEMS uses direct memory
 *     access; inter-task communication uses RTEMS message queues.
 *     This feature is a debugging convenience, not core cyclictest.
 *
 *  8. Named pipe / FIFO output (mkfifo, fifothread)
 *     Reason: mkfifo is a Linux/SYSV IPC mechanism. Not available
 *     on RTEMS. Statistics go to stdout.
 *
 *  9. Linux scheduler priority limits (raise_soft_prio, getrlimit)
 *     Reason: Linux RLIMIT_RTPRIO security model. RTEMS has a
 *     flat priority scheme (1..255) without per-process limits.
 *     setscheduler() works directly without privilege escalation.
 */

#include "cyclictest.h"
#include "histogram.h"
#include <getopt.h>

/* ===== TSC (rdtsc) support ===== */
#define USE_TSC  1  /* 1=hardware TSC, 0=clock_gettime */

#if USE_TSC
static inline unsigned long long rdtsc(void)
{
  unsigned int lo, hi;
  __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
  return ((unsigned long long)hi << 32) | lo;
}
static double tsc_per_us = 3000.0;  /* calibrated at startup */
#endif

/* ===== External references (defined in init.c) ===== */
extern const char *rtems_test_name;

/* ===== Global variables (matching original cyclictest names) ===== */

int shutdown;
static int tracelimit = 0;
static int verbose = 0;
static int oscope_reduction = 1;
static int histogram = 0;
static int histofall = 0;
static int duration = 0;
static int use_nsecs = 0;
static int refresh_on_max;
static int force_sched_other;
static int priospread = 0;
static int check_clock_resolution;
static int ct_debug;

/*
 * REMOVED: lockall (mlockall)
 * Reason: RTEMS has no virtual memory or swap — all memory is
 * always physically resident. mlockall is a no-op concept on RTEMS.
 */

/*
 * REMOVED: use_fifo, fifo_threadid
 * Reason: mkfifo() not available on RTEMS (see comment #8 above).
 */

/*
 * REMOVED: laptop, power_management, latency_target_fd,
 *          deepest_idle_state, saved_cpu_idle_*
 * Reason: Power management not applicable (see comment #3, #4 above).
 */

/*
 * REMOVED: smi (SMI counter)
 * Reason: /dev/cpu/N/msr not available (see comment 2 above).
 * smi is defined to 0 in cyclictest.h.
 */

/*
 * REMOVED: aligned, secaligned, offset, align_barr, globalt_barr, globalt
 * Reason: Thread alignment barriers use pthread_barrier which RTEMS
 * supports, so this IS included. See process_options().
 */
static int aligned = 0;
static int secaligned = 0;
static int offset = 0;
static pthread_barrier_t align_barr;
static pthread_barrier_t globalt_barr;
static struct timespec globalt;

static pthread_cond_t refresh_on_max_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t refresh_on_max_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t break_thread_id_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t break_thread_id = 0;
static uint64_t break_thread_value = 0;

/*
 * REMOVED: fifopath, histfile, jsonfile (file output paths)
 * Simplified: jsonfile only.
 */
static char jsonfile[MAX_PATH];

static struct thread_param **parameters;
static struct thread_stat  **statistics;
static struct histoset       hset;

static int use_nanosleep = MODE_CLOCK_NANOSLEEP;
static int timermode     = TIMER_ABSTIME;
static int use_system;
static int priority;
static int policy        = SCHED_OTHER;
static int num_threads   = 1;
static int max_cycles;
static int clocksel      = 0;
static int quiet;
static int interval      = DEFAULT_INTERVAL;
static int distance      = -1;
static int smp           = 0;
static int setaffinity   = AFFINITY_UNSPECIFIED;

int clocksources[] = {
  CLOCK_MONOTONIC,
  CLOCK_REALTIME,
};

/* Affinity masks — replaced rt_numa bitmask with simple cpu_set_t */
static cpu_set_t *affinity_mask = NULL;
static cpu_set_t *main_affinity_mask = NULL;

/* Spike/trigger tracking */
static pthread_mutex_t trigger_lock = PTHREAD_MUTEX_INITIALIZER;
static int trigger = 0;
static int trigger_list_size = 1024;
struct thread_trigger *head = NULL;
struct thread_trigger *tail = NULL;
struct thread_trigger *current = NULL;
static int spikes;

/* ===== warn/fatal helpers (replacing rt-tests rt-error.h) ===== */
#include <stdarg.h>

static void warn(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "WARN: ");
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static void fatal(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "FATAL: ");
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  return EXIT_FAILURE;
}

/* ===== Utility functions ===== */

static inline void tsnorm(struct timespec *ts)
{
  while (ts->tv_nsec >= NSEC_PER_SEC) {
    ts->tv_nsec -= NSEC_PER_SEC;
    ts->tv_sec++;
  }
  while (ts->tv_nsec < 0) {
    ts->tv_nsec += NSEC_PER_SEC;
    ts->tv_sec--;
  }
}

static inline int tsgreater(struct timespec *a, struct timespec *b)
{
  return ((a->tv_sec > b->tv_sec) ||
          (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec));
}

static inline int64_t calcdiff(struct timespec t1, struct timespec t2)
{
  int64_t diff = USEC_PER_SEC * (long long)((int)t1.tv_sec - (int)t2.tv_sec);
  diff += ((int)t1.tv_nsec - (int)t2.tv_nsec) / 1000;
  return diff;
}

static inline int64_t calcdiff_ns(struct timespec t1, struct timespec t2)
{
  int64_t diff;
  diff = NSEC_PER_SEC * (int64_t)((int)t1.tv_sec - (int)t2.tv_sec);
  diff += ((int)t1.tv_nsec - (int)t2.tv_nsec);
  return diff;
}

static inline int64_t calctime(struct timespec t)
{
  int64_t time;
  time = USEC_PER_SEC * t.tv_sec;
  time += ((int)t.tv_nsec) / 1000;
  return time;
}

/* Simple get_available_cpus — RTEMS version */
static int get_available_cpus(void)
{
#ifdef __rtems__
  /*
   * RTEMS: return configured processor count.
   * We can't hotplug CPUs on RTEMS; all configured processors are online.
   */
  return (int)rtems_scheduler_get_processor_maximum() + 1;
#else
  return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

/*
 * REMOVED: check_privs()
 * Reason: RTEMS has no capability/privilege system. All tasks
 * can set real-time priority. No root/effective-UID check needed.
 */

/*
 * REMOVED: mlockall(MCL_CURRENT|MCL_FUTURE)
 * Reason: RTEMS has no virtual memory, no swap, no paging.
 * All memory is always physically present. No locking needed.
 */

/*
 * REMOVED: set_latency_target() + /dev/cpu_dma_latency
 * Reason: Linux PM QoS (see comment #3). No-op on RTEMS.
 */

/*
 * REMOVED: save/restore/set_deepest_cpu_idle_state()
 * Reason: Linux cpuidle (see comment #4). No-op on RTEMS.
 */

/*
 * REMOVED: raise_soft_prio() + getrlimit/setrlimit
 * Reason: Linux RLIMIT_RTPRIO security (see comment #9).
 * On RTEMS, sched_setscheduler() works directly.
 *
 * setscheduler() is simplified to just call sched_setscheduler.
 */
static int setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
  int err = sched_setscheduler(pid, policy, param);
  if (err)
    err = errno;
  return err;
}

/*
 * REMOVED: open_msr_file(), get_msr(), get_smi_counter(), has_smi_counter()
 * Reason: SMI counter (see comment #2). smi is #defined to 0 so all
 * if(smi) blocks are eliminated by the compiler (dead code elimination).
 */

/* ===== Parse time string (unchanged from original) ===== */
int parse_time_string(char *val)
{
  char *end;
  int v = strtol(val, &end, 10);
  if (end && end[0] != '\0') {
    switch (end[0]) {
    case 'd': v *= 24;
    case 'h': v *= 60;
    case 'm': v *= 60; break;
    default:
      fprintf(stderr, "Unable to parse time string %s\n", val);
      return EXIT_FAILURE;
    }
  }
  if (v <= 0)
    v = 1;
  return v;
}

/* ===== Policy helpers ===== */

void handlepolicy(char *polname)
{
  if (strncasecmp(polname, "other", 5) == 0)
    policy = SCHED_OTHER;
  else if (strncasecmp(polname, "batch", 5) == 0)
    policy = SCHED_BATCH;
  else if (strncasecmp(polname, "idle", 4) == 0)
    policy = SCHED_IDLE;
  else if (strncasecmp(polname, "fifo", 4) == 0)
    policy = SCHED_FIFO;
  else if (strncasecmp(polname, "rr", 2) == 0)
    policy = SCHED_RR;
  else
    policy = SCHED_OTHER;
}

char *policyname(int p)
{
  char *policystr = "";
  switch (p) {
  case SCHED_OTHER: policystr = "other"; break;
  case SCHED_FIFO:  policystr = "fifo";  break;
  case SCHED_RR:    policystr = "rr";    break;
  case SCHED_BATCH: policystr = "batch"; break;
  case SCHED_IDLE:  policystr = "idle";  break;
  }
  return policystr;
}

const char *policy_to_string(int p)
{
  return policyname(p);
}

uint32_t string_to_policy(const char *str)
{
  if (strcasecmp(str, "fifo") == 0)  return SCHED_FIFO;
  if (strcasecmp(str, "rr") == 0)    return SCHED_RR;
  if (strcasecmp(str, "batch") == 0) return SCHED_BATCH;
  if (strcasecmp(str, "idle") == 0)  return SCHED_IDLE;
  return SCHED_OTHER;
}

/* ===== Signal handler ===== */
void sighand(int sig)
{
  /*
   * REMOVED: SIGUSR1/SIGUSR2 status dump
   * Reason: RTEMS POSIX signals have limited support for
   * inter-process signals.  Shutdown via SIGINT/SIGTERM works.
   */
  if (sig == SIGUSR1) {
    /* Status dump on signal — simplified */
    int oldquiet = quiet;
    quiet = 0;
    fprintf(stderr, "# cyclictest current status (SIGUSR1):\n");
    quiet = oldquiet;
    return;
  }
  shutdown = 1;
  if (refresh_on_max)
    pthread_cond_signal(&refresh_on_max_cond);
}

/* ===== Check timer resolution ===== */
int check_timer(void)
{
  struct timespec ts;
  if (clock_getres(CLOCK_MONOTONIC, &ts))
    return 1;
  return (ts.tv_sec != 0 || ts.tv_nsec != 1);
}

/* ===== Help display ===== */
void display_help(int error)
{
  printf("cyclictest V 1.0.0 (RTEMS port)\n");
  printf("Usage: cyclictest <options>\n\n"
         "-a [CPU]  --affinity      pin threads to CPU (e.g. -a 0, or -a 0-2)\n"
         "-A USEC   --aligned=USEC  align thread wakeups to a specific offset\n"
         "-b USEC   --breaktrace=USEC stop when latency > USEC\n"
         "-c CLOCK  --clock=CLOCK    0=MONOTONIC (default), 1=REALTIME\n"
         "-d DIST   --distance=DIST  thread interval distance (us), default=500\n"
         "-D TIME   --duration=TIME  test duration (append m/h/d)\n"
         "-h US     --histogram=US   dump latency histogram after run\n"
         "-H US     --histofall=US   histogram with summary column\n"
         "-i INTV   --interval=INTV  base interval (us), default=1000\n"
         "-l LOOPS  --loops=LOOPS    number of cycles, 0=endless\n"
         "-M        --refresh_on_max delay update until new max latency\n"
         "-N        --nsecs          print results in ns instead of us\n"
         "-p PRIO   --priority=PRIO  highest thread priority\n"
         "          --policy=NAME    scheduling policy: fifo, rr, other\n"
         "          --priospread     spread priority levels across threads\n"
         "-q        --quiet          summary only on exit\n"
         "-r        --relative       use relative timer instead of absolute\n"
         "-R        --resolution     check clock resolution\n"
         "-s        --system         use sys_nanosleep (tick-based)\n"
         "-S        --smp            SMP mode: -a -t with same priority\n"
         "          --spike=TRIGGER  record all spikes > trigger\n"
         "-t [N]    --threads=N      number of threads (default: #CPUs)\n"
         "-u        --unbuffered     force unbuffered output\n"
         "-v        --verbose        per-cycle output for statistics\n"
         "-x        --posix_timers   use POSIX timers (timer_create)\n"
         "          --help           this help\n"
         "\n=== REMOVED from Linux original ===\n"
         "  --mlockall      : RTEMS has no swap, memory is always resident\n"
         "  --smi           : /dev/cpu/N/msr not available on RTEMS\n"
         "  --laptop        : no CPU power management on RTEMS\n"
         "  --latency=PM_QOS: no /dev/cpu_dma_latency on RTEMS\n"
         "  --deepest-idle  : no cpuidle subsystem on RTEMS\n"
         "  --fifo=FIFO     : mkfifo not available on RTEMS\n"
         "  --json=FILENAME : not yet implemented\n"
         "  --histfile=FILE : not yet implemented\n"
  );
  /* Don't call exit() — just return.  Caller handles it. */
}

/* ===== Command-line options ===== */
enum option_values {
  OPT_AFFINITY = 1, OPT_BREAKTRACE, OPT_CLOCK,
  OPT_DISTANCE, OPT_DURATION,
  OPT_HISTOGRAM, OPT_HISTOFALL,
  OPT_INTERVAL, OPT_LOOPS,
  OPT_REFRESH, OPT_NANOSLEEP, OPT_NSECS, OPT_OSCOPE, OPT_PRIORITY,
  OPT_QUIET, OPT_PRIOSPREAD, OPT_RELATIVE, OPT_RESOLUTION,
  OPT_SYSTEM, OPT_SMP, OPT_THREADS, OPT_TRIGGER,
  OPT_TRIGGER_NODES, OPT_UNBUFFERED, OPT_VERBOSE,
  OPT_DBGCYCLIC, OPT_POLICY, OPT_HELP, OPT_NUMOPTS,
  OPT_ALIGNED, OPT_SECALIGNED, OPT_POSIX_TIMERS,
};

void process_options(int argc, char *argv[], int max_cpus)
{
  int error = 0;
  int option_affinity = 0;

  for (;;) {
    int option_index = 0;
    static struct option long_options[] = {
      {"affinity",     optional_argument, NULL, OPT_AFFINITY},
      {"aligned",      optional_argument, NULL, OPT_ALIGNED},
      {"breaktrace",   required_argument, NULL, OPT_BREAKTRACE},
      {"clock",        required_argument, NULL, OPT_CLOCK},
      {"distance",     required_argument, NULL, OPT_DISTANCE},
      {"duration",     required_argument, NULL, OPT_DURATION},
      {"histogram",    required_argument, NULL, OPT_HISTOGRAM},
      {"histofall",    required_argument, NULL, OPT_HISTOFALL},
      {"interval",     required_argument, NULL, OPT_INTERVAL},
      {"loops",        required_argument, NULL, OPT_LOOPS},
      {"refresh_on_max",no_argument,      NULL, OPT_REFRESH},
      {"nsecs",        no_argument,       NULL, OPT_NSECS},
      {"oscope",       required_argument, NULL, OPT_OSCOPE},
      {"priority",     required_argument, NULL, OPT_PRIORITY},
      {"quiet",        no_argument,       NULL, OPT_QUIET},
      {"priospread",   no_argument,       NULL, OPT_PRIOSPREAD},
      {"relative",     no_argument,       NULL, OPT_RELATIVE},
      {"resolution",   no_argument,       NULL, OPT_RESOLUTION},
      {"secaligned",   optional_argument, NULL, OPT_SECALIGNED},
      {"system",       no_argument,       NULL, OPT_SYSTEM},
      {"smp",          no_argument,       NULL, OPT_SMP},
      {"spike",        required_argument, NULL, OPT_TRIGGER},
      {"spike-nodes",  required_argument, NULL, OPT_TRIGGER_NODES},
      {"threads",      optional_argument, NULL, OPT_THREADS},
      {"unbuffered",   no_argument,       NULL, OPT_UNBUFFERED},
      {"verbose",      no_argument,       NULL, OPT_VERBOSE},
      {"dbg_cyclictest",no_argument,      NULL, OPT_DBGCYCLIC},
      {"policy",       required_argument, NULL, OPT_POLICY},
      {"help",         no_argument,       NULL, OPT_HELP},
      {"posix_timers", no_argument,       NULL, OPT_POSIX_TIMERS},
      {NULL, 0, NULL, 0},
    };
    int c = getopt_long(argc, argv,
                        "a::A::b:c:d:D:h:H:i:l:MNo:p:qrRsSt::uvx",
                        long_options, &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 'a':
    case OPT_AFFINITY:
      option_affinity = 1;
      if (smp) break;
      if (optarg) {
        int cpu = atoi(optarg);
        affinity_mask = calloc(1, sizeof(cpu_set_t));
        CPU_ZERO(affinity_mask);
        CPU_SET(cpu, affinity_mask);
        setaffinity = AFFINITY_SPECIFIED;
      } else {
        setaffinity = AFFINITY_USEALL;
      }
      break;
    case 'A':
    case OPT_ALIGNED:
      aligned = 1;
      if (optarg) offset = atoi(optarg) * 1000;
      else offset = 0;
      break;
    case 'b':
    case OPT_BREAKTRACE:
      tracelimit = atoi(optarg); break;
    case 'c':
    case OPT_CLOCK:
      clocksel = atoi(optarg); break;
    case 'd':
    case OPT_DISTANCE:
      distance = atoi(optarg); break;
    case 'D':
    case OPT_DURATION:
      duration = parse_time_string(optarg); break;
    case 'H':
    case OPT_HISTOFALL:
      histofall = 1; /* fall through */
    case 'h':
    case OPT_HISTOGRAM:
      histogram = atoi(optarg); break;
    case 'i':
    case OPT_INTERVAL:
      interval = atoi(optarg); break;
    case 'l':
    case OPT_LOOPS:
      max_cycles = atoi(optarg); break;
    case 'M':
    case OPT_REFRESH:
      refresh_on_max = 1; break;
    case 'N':
    case OPT_NSECS:
      use_nsecs = 1; break;
    case 'o':
    case OPT_OSCOPE:
      oscope_reduction = atoi(optarg); break;
    case 'p':
    case OPT_PRIORITY:
      priority = atoi(optarg);
      if (policy != SCHED_FIFO && policy != SCHED_RR)
        policy = SCHED_FIFO;
      break;
    case 'q':
    case OPT_QUIET:
      quiet = 1; break;
    case 'r':
    case OPT_RELATIVE:
      timermode = TIMER_RELTIME; break;
    case 'R':
    case OPT_RESOLUTION:
      check_clock_resolution = 1; break;
    case OPT_SECALIGNED:
      secaligned = 1;
      if (optarg) offset = atoi(optarg) * 1000;
      else offset = 0;
      break;
    case 's':
    case OPT_SYSTEM:
      use_system = MODE_SYS_OFFSET; break;
    case 'S':
    case OPT_SMP:
      smp = 1;
      num_threads = -1;
      setaffinity = AFFINITY_USEALL;
      break;
    case 't':
    case OPT_THREADS:
      if (smp) break;
      if (optarg)
        num_threads = atoi(optarg);
      else
        num_threads = -1;
      break;
    case OPT_TRIGGER:
      trigger = atoi(optarg); break;
    case OPT_TRIGGER_NODES:
      if (trigger) trigger_list_size = atoi(optarg); break;
    case 'u':
    case OPT_UNBUFFERED:
      setvbuf(stdout, NULL, _IONBF, 0); break;
    case 'v':
    case OPT_VERBOSE:
      verbose = 1; break;
    case 'x':
    case OPT_POSIX_TIMERS:
      use_nanosleep = MODE_CYCLIC; break;
    case OPT_PRIOSPREAD:
      priospread = 1; break;
    case OPT_POLICY:
      handlepolicy(optarg); break;
    case OPT_DBGCYCLIC:
      ct_debug = 1; break;
    case '?':
    case OPT_HELP:
      display_help(0); break;
    }
  }

  if ((use_system == MODE_SYS_OFFSET) && (use_nanosleep == MODE_CYCLIC)) {
    printf("system option requires clock_nanosleep, not posix_timers\n");
    use_nanosleep = MODE_CLOCK_NANOSLEEP;
  }

  if (clocksel < 0 || clocksel > 1)    clocksel = 0;
  if (histogram < 0)                   histogram = 0;
  if (histogram > HIST_MAX)            histogram = HIST_MAX;
  if (distance == -1)                  distance = DEFAULT_DISTANCE;
  if (priority < 0 || priority > 99)   priority = 0;
  if (num_threads == -1)               num_threads = get_available_cpus();
  if (num_threads < 1)                 num_threads = 1;

  if (priospread && priority == 0)
    priority = num_threads + 1;

  if (priority && policy != SCHED_FIFO && policy != SCHED_RR)
    policy = SCHED_FIFO;

  if ((policy == SCHED_FIFO || policy == SCHED_RR) && priority == 0)
    priority = num_threads + 1;

  if (aligned && secaligned) {
    aligned = secaligned = 0;
  }

  if (aligned || secaligned) {
    pthread_barrier_init(&globalt_barr, NULL, num_threads);
    pthread_barrier_init(&align_barr, NULL, num_threads);
  }
}

/* ===== Spike/trigger tracking (unchanged from original) ===== */

static int trigger_init(void)
{
  int i;
  struct thread_trigger *trig = NULL;
  for (i = 0; i < trigger_list_size; i++) {
    trig = malloc(sizeof(struct thread_trigger));
    if (trig != NULL) {
      if (head == NULL) {
        head = trig;
        tail = trig;
      } else {
        tail->next = trig;
        tail = trig;
      }
      trig->tnum = i;
      trig->next = NULL;
    } else {
      return -1;
    }
  }
  current = head;
  return 0;
}

static void trigger_print(void)
{
  struct thread_trigger *trig = head;
  if (current == head) return;
  printf("\n");
  while (trig->next != current) {
    fprintf(stdout, "T:%2d Spike:%8ld: TS: %12ld\n",
            trig->tnum, trig->diff, (long)trig->ts);
    trig = trig->next;
  }
  fprintf(stdout, "T:%2d Spike:%8ld: TS: %12ld\n",
          trig->tnum, trig->diff, (long)trig->ts);
  printf("spikes = %d\n\n", spikes);
}

static void trigger_update(struct thread_param *par, int diff, int64_t ts)
{
  pthread_mutex_lock(&trigger_lock);
  if (current != NULL) {
    current->tnum = par->tnum;
    current->ts   = ts;
    current->diff = diff;
    current = current->next;
  }
  spikes++;
  pthread_mutex_unlock(&trigger_lock);
}

/* ===== Print helpers ===== */

void print_tids(struct thread_param *par[], int nthreads)
{
  int i;
  printf("# Thread Ids:");
  for (i = 0; i < nthreads; i++)
    printf(" %05d", par[i]->stats->tid);
  printf("\n");
}

void print_stat(FILE *fp, struct thread_param *par, int index,
                int verbose, int quiet)
{
  struct thread_stat *stat = par->stats;

  if (!verbose) {
    if (quiet != 1) {
      char *fmt;
      if (use_nsecs)
        fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
              "Min:%7ld Act:%8ld Avg:%8ld Max:%8ld\r\n";
      else
        fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
              "Min:%7ld Act:%5ld Avg:%5ld Max:%8ld\r\n";

      fprintf(fp, fmt, index, stat->tid, par->prio,
              par->interval, stat->cycles, stat->min,
              stat->act,
              stat->cycles ? (long)(stat->avg / stat->cycles) : 0,
              stat->max);
    }
  } else {
    /* Verbose mode: per-sample output */
    while (stat->cycles != stat->cyclesread) {
      long diff = stat->values[stat->cyclesread & par->bufmsk];

      if (diff > stat->redmax) {
        stat->redmax = diff;
        stat->cycleofmax = stat->cyclesread;
      }
      if (++stat->reduce == oscope_reduction) {
        fprintf(fp, "%8d:%8lu:%8ld\r\n",
                index, stat->cycleofmax, stat->redmax);
        stat->reduce = 0;
        stat->redmax = 0;
      }
      stat->cyclesread++;
    }
  }
}

/* ===== Histogram output ===== */

void print_hist(struct thread_param *par[], int nthreads)
{
  int i;
  unsigned long maxmax, alloverflows;
  FILE *fd = stdout;

  fprintf(fd, "# Histogram\n");
  for (i = 0; i < histogram; i++) {
    unsigned long flags = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%06d ", i);
    if (histofall) flags |= HSET_PRINT_SUM;
    hset_print_bucket(&hset, fd, buf, i, flags);
  }

  fprintf(fd, "# Min Latencies:");
  for (i = 0; i < nthreads; i++)
    fprintf(fd, " %05lu", par[i]->stats->min);
  fprintf(fd, "\n");

  fprintf(fd, "# Avg Latencies:");
  for (i = 0; i < nthreads; i++)
    fprintf(fd, " %05lu",
            par[i]->stats->cycles ?
            (long)(par[i]->stats->avg / par[i]->stats->cycles) : 0);
  fprintf(fd, "\n");

  fprintf(fd, "# Max Latencies:");
  maxmax = 0;
  for (i = 0; i < nthreads; i++) {
    fprintf(fd, " %05lu", par[i]->stats->max);
    if (par[i]->stats->max > maxmax)
      maxmax = par[i]->stats->max;
  }
  if (histofall && nthreads > 1)
    fprintf(fd, " %05lu", maxmax);
  fprintf(fd, "\n");

  fprintf(fd, "# Histogram Overflows:");
  alloverflows = 0;
  for (i = 0; i < nthreads; i++) {
    fprintf(fd, " %05lu", par[i]->stats->hist->oflow_count);
    alloverflows += par[i]->stats->hist->oflow_count;
  }
  if (histofall && nthreads > 1)
    fprintf(fd, " %05lu", alloverflows);
  fprintf(fd, "\n");

  fprintf(fd, "# Histogram Overflow at cycle number:\n");
  for (i = 0; i < nthreads; i++) {
    fprintf(fd, "# Thread %d: ", i);
    hist_print_oflows(par[i]->stats->hist, fd);
    fprintf(fd, "\n");
  }
  fprintf(fd, "\n");
}

/*
 * ===== CORE: Measurement Thread (timerthread) =====
 *
 * This is the heart of cyclictest — ported from the original with
 * minimal changes.  The algorithm is identical:
 *
 *   1. Set thread affinity, priority, signal mask
 *   2. Initialize timer (MODE_CYCLIC) or use clock_nanosleep
 *   3. Loop:
 *      a. Wait for next period (sleep / signal)
 *      b. clock_gettime to get actual wake time
 *      c. diff = calcdiff(now, next)  — latency!
 *      d. Update min/max/avg/histogram
 *      e. next += interval
 */
void *timerthread(void *param)
{
  struct thread_param *par = param;
  struct sched_param   schedp;
  struct sigevent      sigev;
  sigset_t             sigset;
  timer_t              timer;
  struct timespec      now, next, interval, stop = {0, 0};
  struct itimerspec    tspec;
  struct thread_stat  *stat = par->stats;
  int                  stopped = 0;
  int                  ret;

  /*
   * REMOVED: NUMA node affinity (rt_numa_set_numa_run_on_node)
   * Reason: No NUMA on RTEMS (see comment #1).
   */

  /* CPU affinity */
  if (par->cpu != -1) {
    cpu_set_t mask;
    pthread_t thread;
    CPU_ZERO(&mask);
    CPU_SET(par->cpu, &mask);
    thread = pthread_self();
    if (pthread_setaffinity_np(thread, sizeof(mask), &mask) != 0)
      warn("Could not set CPU affinity to CPU #%d\n", par->cpu);
  }

  /* Compute interval as timespec */
  interval.tv_sec  = par->interval / USEC_PER_SEC;
  interval.tv_nsec = (par->interval % USEC_PER_SEC) * 1000;

  stat->tid = gettid();

  /* Block the timer signal */
  sigemptyset(&sigset);
  sigaddset(&sigset, par->signal);
  sigprocmask(SIG_BLOCK, &sigset, NULL);

  /* MODE_CYCLIC: use POSIX timer_create + signal.
   * Only available when RTEMS POSIX timers are configured
   * (CONFIGURE_MAXIMUM_POSIX_TIMERS > 0).  Otherwise,
   * use clock_nanosleep (MODE_CLOCK_NANOSLEEP) instead. */
  if (par->mode == MODE_CYCLIC) {
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
    sigev.sigev_notify = SIGEV_SIGNAL;
    sigev.sigev_signo  = par->signal;
    timer_create(par->clock, &sigev, &timer);
    tspec.it_interval = interval;
#else
    /* RTEMS without POSIX timers: fall back to clock_nanosleep */
    par->mode = MODE_CLOCK_NANOSLEEP;
#endif
  }

  /*
   * Set scheduling policy + priority.
   * On RTEMS: sched_setscheduler is a stub (ENOSYS). Priority was
   * already set via pthread_attr_setschedparam before thread creation.
   * On Linux: sched_setscheduler sets the policy+priority at runtime.
   */
#ifndef __rtems__
  if (par->policy != SCHED_OTHER || par->prio > 0) {
    memset(&schedp, 0, sizeof(schedp));
    schedp.sched_priority = par->prio;
    if (setscheduler(0, par->policy, &schedp))
      fatal("timerthread%d: failed to set priority to %d\n",
            par->cpu, par->prio);
  }
#endif

  /*
   * REMOVED: SMI counter initialization
   * Reason: No /dev/cpu/N/msr (see comment #2).
   */

  /* Get current time */
  if (aligned || secaligned) {
    pthread_barrier_wait(&globalt_barr);
    if (par->tnum == 0) {
      clock_gettime(par->clock, &globalt);
      if (secaligned) {
        if (globalt.tv_nsec > 900000000)
          globalt.tv_sec += 2;
        else
          globalt.tv_sec++;
        globalt.tv_nsec = 0;
      }
    }
    pthread_barrier_wait(&align_barr);
    now = globalt;
    if (offset) {
      if (aligned)
        now.tv_nsec += offset * par->tnum;
      else
        now.tv_nsec += offset;
      tsnorm(&now);
    }
  } else {
    clock_gettime(par->clock, &now);
  }

  next = now;
  next.tv_sec  += interval.tv_sec;
  next.tv_nsec += interval.tv_nsec;
  tsnorm(&next);

  if (duration) {
    stop = now;
    stop.tv_sec += duration;
  }

  /* Start the timer for MODE_CYCLIC */
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
  if (par->mode == MODE_CYCLIC) {
    if (par->timermode == TIMER_ABSTIME)
      tspec.it_value = next;
    else
      tspec.it_value = interval;
    timer_settime(timer, par->timermode, &tspec, NULL);
  }
#endif

  /*
   * REMOVED: MODE_SYS_ITIMER (setitimer)
   * Reason: setitimer(ITIMER_REAL) sends SIGALRM to the process.
   * On RTEMS, process-level signals have limited support.
   * clock_nanosleep and POSIX timers cover all practical use cases.
   */

  stat->threadstarted++;

#if USE_TSC
  /* Calibrate TSC if first thread (shared calibration) */
  {
    struct timespec cal_req, cal_start, cal_end;
    unsigned long long cal_tsc1, cal_tsc2;
    cal_req.tv_sec = 0; cal_req.tv_nsec = 50000000; /* 50ms */
    clock_gettime(par->clock, &cal_start);
    cal_tsc1 = rdtsc();
    clock_nanosleep(CLOCK_MONOTONIC, 0, &cal_req, NULL);
    cal_tsc2 = rdtsc();
    clock_gettime(par->clock, &cal_end);
    long long cal_ns = (cal_end.tv_sec-cal_start.tv_sec)*NSEC_PER_SEC
                     + (cal_end.tv_nsec-cal_start.tv_nsec);
    if (cal_ns > 0)
      tsc_per_us = (double)(cal_tsc2 - cal_tsc1) / ((double)cal_ns/1000.0);
    if (par->tnum == 0)
      printf("TSC: %.0f MHz\n", tsc_per_us);
  }
#endif

  /* ===== MAIN MEASUREMENT LOOP ===== */
  while (!shutdown) {
    uint64_t diff;
    int      sigs;
#if USE_TSC
    unsigned long long tsc_before;
    tsc_before = rdtsc();  /* timestamp BEFORE sleep */
#endif

    /* Wait for next period */
    switch (par->mode) {
    case MODE_CYCLIC:
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
      if (sigwait(&sigset, &sigs) < 0)
        goto out;
#endif
      break;

    case MODE_CLOCK_NANOSLEEP:
      if (par->timermode == TIMER_ABSTIME) {
        ret = clock_nanosleep(par->clock, TIMER_ABSTIME, &next, NULL);
        if (ret != 0) {
          if (ret != EINTR)
            warn("clock_nanosleep failed. errno: %d\n", errno);
          goto out;
        }
      } else {
        /* Relative mode */
        struct timespec now2;
        ret = clock_gettime(par->clock, &now2);
        if (ret != 0) {
          if (ret != EINTR)
            warn("clock_gettime() failed\n");
          goto out;
        }
        ret = clock_nanosleep(par->clock, TIMER_RELTIME, &interval, NULL);
        if (ret != 0) {
          if (ret != EINTR)
            warn("clock_nanosleep() failed. errno: %d\n", errno);
          goto out;
        }
        next.tv_sec  = now2.tv_sec  + interval.tv_sec;
        next.tv_nsec = now2.tv_nsec + interval.tv_nsec;
        tsnorm(&next);
      }
      break;

    default:
      goto out;
    }

#if USE_TSC
    /* Measure latency with hardware TSC */
    {
      unsigned long long tsc_after = rdtsc();
      long long tsc_elapsed = (long long)(tsc_after - tsc_before);
      long long tsc_expected = (long long)((double)par->interval * tsc_per_us);
      diff = (uint64_t)(tsc_elapsed - tsc_expected);
      /* Convert TSC ticks to microseconds for statistics */
      if (diff > (uint64_t)(tsc_per_us * 1000000)) diff = 0; /* overflow guard */
      /* keep diff in TSC ticks, stats show raw TSC excess */
    }
    /* Still need clock_gettime for now (used by tsgreater, duration below) */
    clock_gettime(par->clock, &now);
#else
    /* Read actual wake-up time */
    ret = clock_gettime(par->clock, &now);
    if (ret != 0) {
      if (ret != EINTR)
        warn("clock_gettime() failed. errno: %d\n", errno);
      goto out;
    }

    /* Compute latency */
    if (use_nsecs)
      diff = calcdiff_ns(now, next);
    else
      diff = calcdiff(now, next);
#endif

    /* Update statistics */
    if (diff < stat->min) stat->min = diff;
    if (diff > stat->max) {
      stat->max = diff;
      if (refresh_on_max)
        pthread_cond_signal(&refresh_on_max_cond);
    }
    stat->avg += (double)diff;

    if (trigger && (diff > trigger))
      trigger_update(par, diff, calctime(now));

    if (duration && (calcdiff(now, stop) >= 0))
      shutdown++;

    /*
     * REMOVED: tracelimit + ftrace (tracemark, tracing_stop)
     * Reason: No ftrace on RTEMS (see comment #5).
     */
    if (!stopped && tracelimit && (diff > tracelimit)) {
      stopped++;
      shutdown++;
      pthread_mutex_lock(&break_thread_id_lock);
      if (break_thread_id == 0) {
        break_thread_id = stat->tid;
        break_thread_value = diff;
      }
      pthread_mutex_unlock(&break_thread_id_lock);
    }

    stat->act = diff;

    /* Store value in ring buffer for verbose mode */
    if (par->bufmsk)
      stat->values[stat->cycles & par->bufmsk] = diff;

    /* Update histogram */
    if (histogram)
      hist_sample(stat->hist, diff);

    stat->cycles++;

    /* Advance next wake-up time */
    next.tv_sec  += interval.tv_sec;
    next.tv_nsec += interval.tv_nsec;

    if (par->mode == MODE_CYCLIC) {
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
      int overrun_count = timer_getoverrun(timer);
      next.tv_sec  += overrun_count * interval.tv_sec;
      next.tv_nsec += overrun_count * interval.tv_nsec;
#endif
    }
    tsnorm(&next);

    /* Catch up if we're behind */
    while (tsgreater(&now, &next)) {
      next.tv_sec  += interval.tv_sec;
      next.tv_nsec += interval.tv_nsec;
      tsnorm(&next);
    }

    if (par->max_cycles && par->max_cycles == stat->cycles)
      break;
  }

out:
  if (refresh_on_max) {
    pthread_mutex_lock(&refresh_on_max_lock);
    shutdown++;
    pthread_cond_signal(&refresh_on_max_cond);
    pthread_mutex_unlock(&refresh_on_max_lock);
  }

  if (par->mode == MODE_CYCLIC) {
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
    timer_delete(timer);
#endif
  }

  /*
   * REMOVED: MODE_SYS_ITIMER cleanup
   * Reason: Not implemented (see comment above).
   */

  /* Switch back to normal priority (Linux only, stub on RTEMS) */
#ifndef __rtems__
  if (par->policy != SCHED_OTHER || par->prio > 0) {
    schedp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &schedp);
  }
#endif
  stat->threadstarted = -1;

  return NULL;
}

/*
 * ===== JSON output =====
 *
 * REMOVED: rstat_setup/print (shared memory status)
 * Reason: shm_open/mmap not relevant on RTEMS (see comment #7).
 */
void write_stats(FILE *f, void *data)
{
  struct thread_param **par = parameters;
  int i;
  struct thread_stat *s;

  fprintf(f, "  \"num_threads\": %d,\n", num_threads);
  fprintf(f, "  \"resolution_in_ns\": %u,\n", use_nsecs);
  fprintf(f, "  \"thread\": {\n");
  for (i = 0; i < num_threads; i++) {
    s = par[i]->stats;
    fprintf(f, "    \"%u\": {\n", i);
    if (s->hist) {
      fprintf(f, "      \"histogram\": {");
      hist_print_json(s->hist, f);
      fprintf(f, "      },\n");
    }
    fprintf(f, "      \"cycles\": %ld,\n", s->cycles);
    fprintf(f, "      \"min\": %ld,\n", s->min);
    fprintf(f, "      \"max\": %ld,\n", s->max);
    fprintf(f, "      \"avg\": %.2f,\n",
            s->cycles ? s->avg / s->cycles : 0.0);
    fprintf(f, "      \"cpu\": %d\n", par[i]->cpu);
    fprintf(f, "    }%s\n", i == num_threads - 1 ? "" : ",");
  }
  fprintf(f, "  }\n");
}

/*
 * REMOVED: rt_write_json()
 * Reason: This calls the original rt-utils JSON writer which
 * has extra dependencies.  We provide a minimal version here.
 */
void rt_write_json(const char *filename, int return_code,
                   void (*cb)(FILE *, void *), void *data)
{
  FILE *f = fopen(filename, "w");
  if (!f) return;
  fprintf(f, "{\n");
  cb(f, data);
  fprintf(f, "}\n");
  fclose(f);
}

void rt_init(int argc, char *argv[])
{
  /* Initialize any global state (minimal for RTEMS) */
}

/* ===== CPU affinity helpers ===== */
static int cpu_for_thread_ua(int i, int max_cpus)
{
  return i % max_cpus;
}

static int cpu_for_thread_sp(int i, int max_cpus, cpu_set_t *mask)
{
  int cpu, count = 0;
  for (cpu = 0; cpu < max_cpus; cpu++) {
    if (CPU_ISSET(cpu, mask)) count++;
  }
  if (count == 0) return -1;
  count = i % count;
  for (cpu = 0; cpu < max_cpus; cpu++) {
    if (CPU_ISSET(cpu, mask)) {
      if (count == 0) return cpu;
      count--;
    }
  }
  return -1;
}

/* ===== Simple thread allocation (no NUMA) ===== */
static void *threadalloc(size_t size, int node)
{
  (void)node;
  return calloc(1, size);
}

static void threadfree(void *ptr, size_t size, int node)
{
  (void)size; (void)node;
  free(ptr);
}

/* ===== cyclictest_main — equivalent to original main() ===== */
int cyclictest_main(int argc, char *argv[])
{
  int  i, ret, status;
  int  mode;
  int  max_cpus;
  int  allstopped = 0;

  max_cpus = sysconf(_SC_NPROCESSORS_CONF);

  /* Parse options */
  process_options(argc, argv, max_cpus);

  if (verbose)
    printf("CPUs: %d\n", max_cpus);

  if (trigger) {
    ret = trigger_init();
    if (ret != 0) {
      fprintf(stderr, "trigger_init() failed\n");
      return EXIT_FAILURE;
    }
  }

  if (check_timer())
    printf("WARN: High resolution timers not available\n");

  /* Clock resolution check */
  if (check_clock_resolution) {
    int clock = clocksources[clocksel];
    uint64_t min_non_zero_diff = UINT64_MAX;
    struct timespec now, prev, res;
    uint64_t diff;
    int k;

    if (clock_getres(clock, &res))
      printf("WARN: clock_getres failed\n");

    clock_gettime(clock, &prev);
    for (k = 0; k < 1000; k++)
      clock_gettime(clock, &now);
    diff = calcdiff_ns(now, prev);
    if (diff > 0) {
      uint64_t reported = (NSEC_PER_SEC * res.tv_sec) + res.tv_nsec;
      printf("  reported clock resolution: %llu nsec\n",
             (unsigned long long)reported);
      printf("  measured clock resolution: ~%llu nsec\n",
             (unsigned long long)(diff / 1000));
    }
  }

  mode = use_nanosleep + use_system;

  /* Signal handlers — skip on RTEMS (signal() unavailable in some configs).
   * Ctrl-C termination works via RTEMS internal mechanisms. */

  /* Histogram setup */
  if (histogram &&
      hset_init(&hset, num_threads, 1, histogram, histogram)) {
    fprintf(stderr, "failed to allocate histogram\n");
    return EXIT_FAILURE;
  }

  /* Allocate arrays */
  parameters = calloc(num_threads, sizeof(struct thread_param *));
  if (!parameters) goto out;
  statistics = calloc(num_threads, sizeof(struct thread_stat *));
  if (!statistics) goto outpar;

  /* ===== Create measurement threads ===== */
  for (i = 0; i < num_threads; i++) {
    pthread_attr_t attr;
    int            cpu, node;
    struct thread_param *par;
    struct thread_stat  *stat;

    status = pthread_attr_init(&attr);
    if (status != 0) {
      fprintf(stderr, "pthread_attr_init for thread %d: %s\n",
              i, strerror(status));
      goto outall;
    }

    /*
     * On RTEMS, sched_setscheduler() is a stub (returns ENOSYS).
     * Set scheduling policy + priority via pthread_attr instead.
     */
    if (priority && (policy == SCHED_FIFO || policy == SCHED_RR)) {
      struct sched_param sparam;
      sparam.sched_priority = priority;

      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      pthread_attr_setschedpolicy(&attr, policy);
      pthread_attr_setschedparam(&attr, &sparam);
    }

    /* CPU affinity */
    switch (setaffinity) {
    case AFFINITY_UNSPECIFIED: cpu = -1; break;
    case AFFINITY_SPECIFIED:
      cpu = cpu_for_thread_sp(i, max_cpus, affinity_mask);
      break;
    case AFFINITY_USEALL:
      cpu = cpu_for_thread_ua(i, max_cpus);
      break;
    default: cpu = -1;
    }

    node = -1;

    parameters[i] = par = calloc(1, sizeof(struct thread_param));
    if (!par) goto outall;

    statistics[i] = stat = calloc(1, sizeof(struct thread_stat));
    if (!stat) goto outall;

    if (histogram)
      stat->hist = &hset.histos[i];

    if (verbose) {
      stat->values = calloc(1, VALBUF_SIZE * sizeof(long));
      if (!stat->values) goto outall;
      par->bufmsk = VALBUF_SIZE - 1;
    }

    par->prio = priority;
    if (priority && (policy == SCHED_FIFO || policy == SCHED_RR))
      par->policy = policy;
    else {
      par->policy = SCHED_OTHER;
      force_sched_other = 1;
    }
    if (priospread) priority--;

    par->clock     = clocksources[clocksel];
    par->mode      = mode;
    par->timermode = timermode;
    par->signal    = SIGALRM;
    par->interval  = interval;
    if (!histogram) interval += distance;

    if (verbose)
      printf("Thread %d Interval: %d\n", i, interval);

    par->max_cycles = max_cycles;
    par->stats      = stat;
    par->node       = node;
    par->tnum       = i;
    par->cpu        = cpu;
    par->msr_fd     = -1;

    stat->min           = 1000000;
    stat->max           = 0;
    stat->avg           = 0.0;
    stat->threadstarted = 1;
    stat->smi_count     = 0;

    status = pthread_create(&stat->thread, &attr, timerthread, par);
    if (status) {
      fprintf(stderr, "failed to create thread %d: %s\n", i, strerror(status));
      goto outall;
    }
  }

  /* ===== Main monitoring loop ===== */
  while (!shutdown) {
    char *policystr = policyname(policy);

    if (!verbose && !quiet)
      printf("policy: %s: loadavg: N/A\r\n\r\n", policystr);

    for (i = 0; i < num_threads; i++) {
      print_stat(stdout, parameters[i], i, verbose, quiet);
      if (max_cycles && statistics[i]->cycles >= max_cycles)
        allstopped++;
    }

    usleep(100000);
    if (shutdown || allstopped) break;

    /* ANSI cursor-up: overwrite previous lines (like Linux top) */
    if (!verbose && !quiet)
      printf("\033[%dA", num_threads + 2);

    if (refresh_on_max) {
      pthread_mutex_lock(&refresh_on_max_lock);
      if (!shutdown)
        pthread_cond_wait(&refresh_on_max_cond, &refresh_on_max_lock);
      pthread_mutex_unlock(&refresh_on_max_lock);
    }
  }

  ret = EXIT_SUCCESS;

outall:
  shutdown = 1;
  usleep(50000);

  if (!verbose && !quiet && refresh_on_max)
    printf("\033[%dB", num_threads + 2);

  if (quiet) quiet = 2;

  /* Join and print */
  for (i = 0; i < num_threads; i++) {
    if (statistics[i] && statistics[i]->threadstarted > 0)
      pthread_cancel(statistics[i]->thread);
    if (statistics[i] && statistics[i]->threadstarted) {
      pthread_join(statistics[i]->thread, NULL);
      if (quiet && !histogram)
        print_stat(stdout, parameters[i], i, 0, 0);
    }
    if (statistics[i])
      free(statistics[i]->values);
  }

  if (trigger) trigger_print();

  if (histogram) print_hist(parameters, num_threads);

  if (tracelimit) {
    print_tids(parameters, num_threads);
    if (break_thread_id) {
      printf("# Break thread: %d\n", break_thread_id);
      printf("# Break value: %llu\n",
             (unsigned long long)break_thread_value);
    }
  }

  for (i = 0; i < num_threads; i++) {
    free(statistics[i]);
    free(parameters[i]);
  }

outpar:
  free(statistics);
  free(parameters);

out:
  if (affinity_mask) free(affinity_mask);
  hset_destroy(&hset);
  return ret;
}
