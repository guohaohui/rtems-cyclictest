/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  Hello World + Clock Verification Test
 *
 *  Verifies:
 *    1. CLOCK_MONOTONIC / CLOCK_REALTIME availability
 *    2. clock_getres() — reported resolution
 *    3. clock_gettime() rapid calls — actual resolution
 *    4. clock_nanosleep() precision
 *    5. Hardware TSC (rdtsc) vs software timecounter comparison
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rtems.h>
#include <tmacros.h>
#include <time.h>
#include <stdio.h>
#include <inttypes.h>

const char rtems_test_name[] = "HELLO WORLD";

/* ===== x86 TSC ===== */
static inline unsigned long long rdtsc(void)
{
  unsigned int lo, hi;
  __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
  return ((unsigned long long)hi << 32) | lo;
}

#define NSEC_PER_SEC  1000000000LL
#define NSEC_PER_USEC 1000LL

/*
 * Print timespec as seconds.nanoseconds
 */
static void print_ts(const char *label, const struct timespec *ts)
{
  printf("  %-18s  %ld.%09ld sec\n", label, ts->tv_sec, ts->tv_nsec);
}

static rtems_task Init(rtems_task_argument ignored)
{
  struct timespec res, ts1, ts2, req;
  long long        diff_ns;
  int              ret, i;

  rtems_print_printer_fprintf_putc(&rtems_test_printer);
  TEST_BEGIN();

  printf("Hello World\n");
  printf("guohaohui\n");

  /* ================================================
   * 1. Check clock resolution
   * ================================================ */
  printf("\n===== Clock Resolution Test =====\n");

  ret = clock_getres(CLOCK_MONOTONIC, &res);
  if (ret == 0) {
    printf("CLOCK_MONOTONIC  clock_getres:  %ld.%09ld sec  (%lld ns)\n",
           res.tv_sec, res.tv_nsec,
           (long long)res.tv_sec * NSEC_PER_SEC + res.tv_nsec);
  } else {
    printf("CLOCK_MONOTONIC  clock_getres:  FAILED (err=%d)\n", ret);
  }

  ret = clock_getres(CLOCK_REALTIME, &res);
  if (ret == 0) {
    printf("CLOCK_REALTIME   clock_getres:  %ld.%09ld sec  (%lld ns)\n",
           res.tv_sec, res.tv_nsec,
           (long long)res.tv_sec * NSEC_PER_SEC + res.tv_nsec);
  } else {
    printf("CLOCK_REALTIME   clock_getres:  FAILED (err=%d)\n", ret);
  }

  /* ================================================
   * 2. Measure actual resolution:
   *    Call clock_gettime rapidly and observe the
   *    smallest non-zero delta. This reveals the
   *    TRUE resolution (vs reported).
   * ================================================ */
  printf("\n===== Actual Resolution (rapid clock_gettime) =====\n");

  {
    struct timespec prev, now;
    long long min_diff = NSEC_PER_SEC;
    int non_zero_count = 0;
    int total_calls = 1000;

    clock_gettime(CLOCK_MONOTONIC, &prev);
    for (i = 0; i < total_calls; i++) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      diff_ns = (long long)(now.tv_sec  - prev.tv_sec) * NSEC_PER_SEC
              + (long long)(now.tv_nsec - prev.tv_nsec);

      if (diff_ns > 0 && diff_ns < min_diff)
        min_diff = diff_ns;
      if (diff_ns > 0)
        non_zero_count++;

      prev = now;
    }

    printf("CLOCK_MONOTONIC: %d calls, %d non-zero deltas\n",
           total_calls, non_zero_count);
    if (non_zero_count > 0)
      printf("  Smallest delta: %lld ns  (%lld us)\n",
             min_diff, min_diff / 1000);
    else
      printf("  ALL DELTAS ARE ZERO!  Clock resolution > %d calls.\n",
             total_calls);
  }

  /* ================================================
   * 3. TSC vs clock_gettime
   *    Read both simultaneously to see how they relate.
   * ================================================ */
  printf("\n===== TSC vs clock_gettime =====\n");

  {
    unsigned long long tsc1, tsc2, tsc_per_ms;
    struct timespec    gt1, gt2;

    /* Measure TSC ticks over a known time interval */
    tsc1 = rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &gt1);

    /* Sleep ~100ms */
    req.tv_sec  = 0;
    req.tv_nsec = 100000000;  /* 100 ms */
    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);

    tsc2 = rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &gt2);

    diff_ns = (long long)(gt2.tv_sec  - gt1.tv_sec) * NSEC_PER_SEC
            + (long long)(gt2.tv_nsec - gt1.tv_nsec);

    printf("clock_gettime delta:  %lld ns  (%lld us  ~%.1f ms)\n",
           diff_ns, diff_ns / 1000, (double)diff_ns / 1000000.0);

    if (diff_ns > 0) {
      tsc_per_ms = (tsc2 - tsc1) / ((unsigned long long)diff_ns / 1000000);
      printf("TSC frequency:        ~%llu MHz\n",
             (tsc2 - tsc1) * 1000 / (unsigned long long)diff_ns);
    }

    printf("TSC raw ticks:        %llu  ->  %llu  (delta=%llu)\n",
           tsc1, tsc2, tsc2 - tsc1);
  }

  /* ================================================
   * 4. clock_nanosleep accuracy
   *    Sleep for 1ms and measure actual elapsed time
   *    with both clock_gettime and TSC.
   * ================================================ */
  printf("\n===== clock_nanosleep Accuracy =====\n");
  printf("Target: 1ms sleep, measured 5 times:\n");
  printf("%-6s  %-14s  %-14s  %-14s\n",
         "Try", "TSC delta", "CLOCK delta", "Error");

  for (i = 0; i < 5; i++) {
    unsigned long long tsc_before, tsc_after;

    req.tv_sec  = 0;
    req.tv_nsec = 1000000;  /* 1 ms */

    tsc_before = rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &ts1);

    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);

    tsc_after = rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &ts2);

    diff_ns = (long long)(ts2.tv_sec  - ts1.tv_sec) * NSEC_PER_SEC
            + (long long)(ts2.tv_nsec - ts1.tv_nsec);

    printf("  #%d    %12llu  %12lld ns  %+lld ns\n",
           i + 1,
           tsc_after - tsc_before,
           diff_ns,
           diff_ns - 1000000);  /* error from 1ms target */
  }

  /* ================================================
   * 5. Summary
   * ================================================ */
  printf("\n===== Summary =====\n");
  printf("If CLOCK_MONOTONIC deltas are always 0 or 10000000 ns (10ms):\n");
  printf("  => BSP uses dummy timecounter (EFI 10ms tick).\n");
  printf("  => clock_gettime resolution = 10ms.\n");
  printf("If TSC shows fine-grained deltas:\n");
  printf("  => Hardware TSC works, but RTEMS doesn't use it.\n");
  printf("  => Fix: replace CLOCK_DRIVER_USE_DUMMY_TIMECOUNTER\n");
  printf("          with a TSC-based timecounter in the BSP.\n");

  TEST_END();
  rtems_test_exit(0);
}

/*
 * Enable clock driver — clock_gettime/clock_nanosleep need it.
 * The original hello disabled it with DOES_NOT_NEED_CLOCK_DRIVER.
 */
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER

#define CONFIGURE_MAXIMUM_TASKS            1

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE

#define CONFIGURE_INIT_TASK_ATTRIBUTES RTEMS_FLOATING_POINT

#define CONFIGURE_INITIAL_EXTENSIONS RTEMS_TEST_INITIAL_EXTENSION

#define CONFIGURE_MICROSECONDS_PER_TICK    1000

#define CONFIGURE_INIT
#include <rtems/confdefs.h>
