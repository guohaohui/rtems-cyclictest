/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  init.c — RTEMS entry point for cyclictest
 *
 *  Two modes (switch via USE_SHELL macro):
 *   - USE_SHELL 0 : auto-test mode, runs CYCLICTEST_ARGS and exits
 *   - USE_SHELL 1 : interactive shell mode, type "cyclictest -p 80 -i 100" etc.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rtems.h>
#include <tmacros.h>
#include <rtems/shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bsp.h>

/* COM1 I/O port (from bsp.h) */
#ifndef COM1_BASE_IO
#define COM1_BASE_IO  0x3F8
#endif

/* UART registers */
#define UART_RBR  0   /* Receiver Buffer Register (read) */
#define UART_THR  0   /* Transmitter Holding Register (write) */
#define UART_LSR  5   /* Line Status Register */
#define LSR_DR    0x01  /* Data Ready */

/* Read one character from COM1 UART (polled, non-blocking) */
static int uart_getchar(void)
{
  if (inport_byte(COM1_BASE_IO + UART_LSR) & LSR_DR)
    return inport_byte(COM1_BASE_IO + UART_RBR);
  return -1;  /* no data */
}

/* ===== Mode switch ===== */
#define USE_SHELL  1   /* 1 = interactive shell, 0 = auto-test */

extern int cyclictest_main(int argc, char *argv[]);

const char rtems_test_name[] = "CYCLICTEST";

/* ===== Auto-test parameters (used when USE_SHELL=0) ===== */
#define CYCLICTEST_ARGS \
  "-l", "500"
/* Argument for running cyclictest in a separate task */
typedef struct {
  int       argc;
  char    **argv;
  volatile int *done;
} cyclictest_task_arg;

static void cyclictest_task(rtems_task_argument arg)
{
  cyclictest_task_arg *a = (cyclictest_task_arg *)arg;
  cyclictest_main(a->argc, a->argv);
  if (a->done) *(a->done) = 1;
  rtems_task_exit();
}

#if !USE_SHELL
static rtems_task Init(rtems_task_argument ignored)
{
  char *argv[] = { "cyclictest", CYCLICTEST_ARGS, NULL };
  int   argc   = (sizeof(argv) / sizeof(argv[0])) - 1;

  rtems_print_printer_fprintf_putc(&rtems_test_printer);
  TEST_BEGIN();
  cyclictest_main(argc, argv);
  TEST_END();
  rtems_test_exit(0);
}

/* ===== Interactive shell mode ===== */
#else

#define MAX_CMD_LEN  256

/*
 * Minimal command loop — does NOT depend on RTEMS shell infrastructure.
 *
 * The RTEMS shell requires termios, line editor, /dev/console, and other
 * infrastructure that may not work with the simple console driver.
 *
 * This loop reads raw characters from stdin and runs cyclictest directly.
 */
static void run_command(int argc, char **argv)
{
  if (argc == 0)
    return;

  if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
    printf("\n");
    printf("Available commands:\n");
    printf("  cyclictest [args]  — run latency measurement\n");
    printf("  shell              — start RTEMS shell (type 'exit' to return)\n");
    printf("  help               — show this help\n");
    printf("  exit               — quit shell\n");
    printf("\n");
    printf("cyclictest examples:\n");
    printf("  cyclictest -h                 — show cyclictest help\n");
    printf("  cyclictest -l 500             — 500 cycles, default interval\n");
    printf("  cyclictest -t 2 -p 10 -i 100 — 2 threads, prio 10, 100us\n");
    printf("  cyclictest -S -v              — SMP mode, verbose\n");
    printf("\n");
    return;
  }

  if (strcmp(argv[0], "shell") == 0) {
    printf("Starting RTEMS shell... (type 'exit' to return)\n");
    rtems_shell_init(
      "SHLL",
      RTEMS_MINIMUM_STACK_SIZE * 4,
      100,
      "/dev/foobar",
      false,
      true,
      NULL
    );
    printf("RTEMS shell exited.\n");
    return;
  }

  if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
    printf("Goodbye!\n");
    TEST_END();
    rtems_test_exit(0);
  }

  if (strcmp(argv[0], "cyclictest") == 0) {
    rtems_id tid;
    rtems_status_code sc;
    static cyclictest_task_arg cyc;
    volatile int done = 0;

    cyc.argc = argc;
    cyc.argv = argv;
    cyc.done = &done;
    sc = rtems_task_create(
      rtems_build_name('C', 'Y', 'C', 'T'),
      50,
      RTEMS_MINIMUM_STACK_SIZE * 32,
      RTEMS_DEFAULT_MODES,
      RTEMS_DEFAULT_ATTRIBUTES,
      &tid
    );
    if (sc != RTEMS_SUCCESSFUL) {
      printf("Error: %s\n", rtems_status_text(sc));
      return;
    }

    sc = rtems_task_start(tid, cyclictest_task, (rtems_task_argument)&cyc);
    if (sc != RTEMS_SUCCESSFUL) {
      printf("Error: %s\n", rtems_status_text(sc));
      rtems_task_delete(tid);
      return;
    }

    /* Wait for cyclictest to finish, checking done flag */
    for (int w = 0; w < 600 && !done; w++)
      rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 10);

    printf("\r\n");
    fflush(stdout);
    return;
  }

  printf("Unknown command: '%s'.  Type 'help' for available commands.\n",
         argv[0]);
}

static rtems_task Init(rtems_task_argument ignored)
{
  char line[MAX_CMD_LEN];
  char *argv[16];
  int   argc;

  rtems_print_printer_fprintf_putc(&rtems_test_printer);

  /*
   * Disable stdio buffering.  The simple console driver does polled I/O
   * and buffered stdin causes getchar() to return EOF until a full buffer
   * is filled.  Unbuffered mode makes each getchar() call read directly
   * from the UART.
   */
  setvbuf(stdin,  NULL, _IONBF, 0);
  setvbuf(stdout, NULL, _IONBF, 0);

  printf("\n");
  printf("========================================\n");
  printf("  RTEMS Cyclictest — Minimal Shell\n");
  printf("  Type 'help' for commands\n");
  printf("  Type 'exit' to quit\n");
  printf("========================================\n");

  /*
   * Main command loop.
   * Polls getchar() — the simple console driver returns EOF when no
   * character is available.  We sleep briefly between polls to avoid
   * busy-waiting.
   */
  while (1) {
    printf("\r\ncyclictest> ");
    fflush(stdout);

    /*
     * Polled character input — the only reliable approach with
     * CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER.
     *
     * fgets() returns EOF immediately because the simple console driver
     * doesn't support blocking read().  We poll getchar() instead.
     */
    int pos = 0;
    while (pos < MAX_CMD_LEN - 1) {
      int c = uart_getchar();
      if (c == EOF) {
        /* No character available — sleep and retry */
        /* No char yet — sleep ~10ms before polling again */
        rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 100);
        if (rtems_clock_get_ticks_per_second() < 100)
          rtems_task_wake_after(1);  /* at least 1 tick */
        continue;
      }
      if (c == '\r' || c == '\n') {
        break;  /* end of line */
      }
      if (c == '\b' || c == 0x7f) {  /* backspace / DEL */
        if (pos > 0) {
          pos--;
          putchar('\b');
          putchar(' ');
          putchar('\b');
        }
        continue;
      }
      line[pos++] = (char)c;
      putchar(c);  /* echo */
      fflush(stdout);
    }
    line[pos] = '\0';
    printf("\r\n");
    fflush(stdout);

    /* Skip empty lines */
    if (pos == 0)
      continue;

    /* Parse into argc/argv (simple whitespace split) */
    argc = 0;
    char *p = line;
    while (*p && argc < 15) {
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '\0') break;
      argv[argc++] = p;
      while (*p && *p != ' ' && *p != '\t') p++;
      if (*p) { *p = '\0'; p++; }
    }
    argv[argc] = NULL;

    run_command(argc, argv);
  }
}
#endif /* USE_SHELL */

/* ===== RTEMS Configuration ===== */
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER

#define CONFIGURE_MAXIMUM_TASKS              50
#define CONFIGURE_MAXIMUM_POSIX_THREADS      10
#define CONFIGURE_MAXIMUM_POSIX_MUTEXES      10
#define CONFIGURE_MAXIMUM_POSIX_CONDITION_VARIABLES  5
#define CONFIGURE_MAXIMUM_POSIX_TIMERS        0
#define CONFIGURE_MAXIMUM_POSIX_BARRIERS      5

#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES RTEMS_FLOATING_POINT
#define CONFIGURE_INITIAL_EXTENSIONS RTEMS_TEST_INITIAL_EXTENSION
#define CONFIGURE_MICROSECONDS_PER_TICK    1000

#define CONFIGURE_SHELL_COMMANDS_INIT
#define CONFIGURE_SHELL_COMMANDS_HELP
#define CONFIGURE_SHELL_COMMANDS_CMDLS
#define CONFIGURE_SHELL_COMMANDS_CMDCHOWN
#define CONFIGURE_SHELL_COMMANDS_CMDDATE
#define CONFIGURE_SHELL_COMMANDS_ECHO
#define CONFIGURE_SHELL_COMMANDS_EXIT
#define CONFIGURE_SHELL_COMMANDS_PWD
#define CONFIGURE_SHELL_COMMANDS_CD
#define CONFIGURE_SHELL_COMMANDS_MKDIR
#define CONFIGURE_SHELL_COMMANDS_RMDIR
#define CONFIGURE_SHELL_COMMANDS_CHDIR
#define CONFIGURE_SHELL_COMMANDS_LS
#define CONFIGURE_SHELL_COMMANDS_CAT
#define CONFIGURE_SHELL_COMMANDS_MV
#define CONFIGURE_SHELL_COMMANDS_CP
#define CONFIGURE_SHELL_COMMANDS_RM
#define CONFIGURE_SHELL_COMMANDS_ID
#define CONFIGURE_SHELL_COMMANDS_WHOAMI
#define CONFIGURE_SHELL_COMMANDS_SLEEP
#define CONFIGURE_SHELL_COMMANDS_CHROOT
#define CONFIGURE_SHELL_COMMANDS_ENV
#define CONFIGURE_SHELL_COMMANDS_ALIAS

#define CONFIGURE_INIT
#include <rtems/confdefs.h>

#include <rtems/shellconfig.h>
