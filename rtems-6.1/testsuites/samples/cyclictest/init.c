/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  init.c — RTEMS 系统入口，cyclictest 的启动与运行环境
 *
 *  本文件负责：
 *    1. RTEMS 内核配置（任务数、POSIX 线程数、tick 粒度等）
 *    2. 启动 cyclictest（两种模式：自动测试/交互 Shell）
 *    3. 提供最小的命令行交互环境（轮询 UART 输入）
 *
 *  ===== 两种运行模式（通过 USE_SHELL 宏切换） =====
 *
 *  USE_SHELL 0 — 自动测试模式：
 *    系统启动后直接运行 cyclictest（使用 CYCLICTEST_ARGS 指定的参数），
 *    测试完成后自动退出。适用于 CI/CD 自动化测试。
 *
 *  USE_SHELL 1 — 交互 Shell 模式（默认）：
 *    系统启动后进入命令行交互环境，用户可以：
 *      - 输入 "cyclictest -p 80 -i 100" 运行测量
 *      - 输入 "help" 查看帮助
 *      - 输入 "shell" 进入 RTEMS 标准 Shell
 *      - 输入 "exit" 退出
 *    适用于开发调试和手动测试。
 *
 *  ===== RTEMS 特殊设计决策 =====
 *
 *  1. 轮询式 UART 输入：
 *     CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER 不提供阻塞 read()，
 *     因此实现 uart_getchar() 直接轮询 UART 状态寄存器。
 *
 *  2. cyclictest 作为独立任务运行：
 *     cyclictest 内部创建多个 POSIX 线程，放在独立 RTEMS 任务中
 *     便于资源隔离、优先级管理、以及运行完成后通知主任务。
 *
 *  3. 无缓冲 I/O：
 *     setvbuf(stdin, NULL, _IONBF, 0) 确保每次 getchar() 直接从 UART
 *     读取，避免行缓冲导致输入不响应。
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
#include <bsp.h>                /* 提供 inport_byte()、COM1_BASE_IO 等 BSP 接口 */

/* ===== COM1 UART 基地址 (来自 bsp.h，如果未定义则使用 x86 标准地址) ===== */
#ifndef COM1_BASE_IO
#define COM1_BASE_IO  0x3F8     /* x86 PC COM1 端口基地址 */
#endif

/* UART 寄存器偏移 */
#define UART_RBR  0             /* 接收缓冲寄存器（读） */
#define UART_THR  0             /* 发送保持寄存器（写） */
#define UART_LSR  5             /* 线路状态寄存器 */
#define LSR_DR    0x01          /* 数据就绪标志位 */

/*
 * uart_getchar — 从 COM1 UART 读取一个字符（轮询、非阻塞）
 *
 * 直接读取 UART 硬件寄存器，不经过操作系统缓冲。
 * 如果 UART 接收缓冲区中没有数据，立即返回 -1。
 *
 * 返回: 读取到的字符（0-255），或 -1 表示无数据可用
 *
 * 注意：这是针对 x86 PC 平台的实现。在其他 BSP 上，
 * COM1_BASE_IO 可能不同或不存在。
 */
static int uart_getchar(void)
{
  /* 检查线路状态寄存器的 Data Ready 位 */
  if (inport_byte(COM1_BASE_IO + UART_LSR) & LSR_DR)
    return inport_byte(COM1_BASE_IO + UART_RBR);  /* 读取字符 */
  return -1;  /* 无数据 */
}

/* ===== 运行模式切换 ===== */
#define USE_SHELL  1   /* 1 = 交互 Shell 模式, 0 = 自动测试模式 */

extern int cyclictest_main(int argc, char *argv[]);   /* 主测量函数（定义在 cyclictest.c） */

/* RTEMS 测试名称（显示在测试输出中） */
const char rtems_test_name[] = "CYCLICTEST";

/* ===== 自动测试模式参数（USE_SHELL=0 时使用） ===== */
/*
 * 参数含义：
 *   -l 500  : 运行 500 个测量周期后自动停止
 *
 * 可根据需要修改此处参数，例如：
 *   "-S", "-p", "10", "-i", "100", "-l", "1000"
 *   表示 SMP 模式，优先级 10，间隔 100us，运行 1000 个周期
 */
#define CYCLICTEST_ARGS \
  "-l", "500"

/*
 * cyclictest_task_arg — 传递给 cyclictest 任务线程的参数
 *
 * argc/argv : cyclictest 的命令行参数
 * done      : 完成标志指针，cyclictest 退出时设为 1
 *             主线程轮询此标志以检测 cyclictest 是否结束
 */
typedef struct {
  int       argc;
  char    **argv;
  volatile int *done;           /* volatile 保证跨任务可见性 */
} cyclictest_task_arg;

/*
 * cyclictest_task — 运行 cyclictest 的 RTEMS 任务入口
 *
 * 这是一个包装函数，在独立的 RTEMS 任务中调用 cyclictest_main()。
 * 任务完成后设置 done 标志通知主任务，然后自我终止。
 *
 * @arg: 指向 cyclictest_task_arg 结构体
 */
static void cyclictest_task(rtems_task_argument arg)
{
  cyclictest_task_arg *a = (cyclictest_task_arg *)arg;
  cyclictest_main(a->argc, a->argv);
  if (a->done) *(a->done) = 1;  /* 通知主任务 cyclictest 已完成 */
  rtems_task_exit();             /* 自我终止，释放任务资源 */
}

/* ======================================================================
 *  自动测试模式 (USE_SHELL=0)
 *
 *  系统启动后直接运行 cyclictest，完成后退出。
 *  使用 RTEMS 测试框架的 TEST_BEGIN/TEST_END 宏。
 * ====================================================================== */
#if !USE_SHELL

static rtems_task Init(rtems_task_argument ignored)
{
  /* 构建 cyclictest 命令行参数 */
  char *argv[] = { "cyclictest", CYCLICTEST_ARGS, NULL };
  int   argc   = (sizeof(argv) / sizeof(argv[0])) - 1;

  rtems_print_printer_fprintf_putc(&rtems_test_printer);
  TEST_BEGIN();                           /* 测试开始 */
  cyclictest_main(argc, argv);            /* 直接运行 cyclictest */
  TEST_END();                             /* 测试结束 */
  rtems_test_exit(0);
}

/* ======================================================================
 *  交互 Shell 模式 (USE_SHELL=1)
 *
 *  系统启动后进入命令行交互循环。
 *  提供自定义的最小命令解析器，不依赖 RTEMS Shell 的 termios/行编辑。
 * ====================================================================== */
#else

#define MAX_CMD_LEN  256       /* 最大命令长度（字符） */

/*
 * run_command — 解析并执行一条命令
 *
 * 支持的命令：
 *   cyclictest [args]  — 在新任务中运行延迟测量
 *   shell              — 启动 RTEMS 标准 Shell
 *   help / ?           — 显示帮助信息
 *   exit / quit        — 退出（调用 rtems_test_exit）
 *
 * cyclictest 在独立任务中运行，优先级 50（低于 Init 任务），
 * 使用 32 倍最小栈大小（测量线程需要较大栈空间）。
 * 主任务轮询 done 标志，最多等待 60 秒。
 *
 * @argc: 参数个数（含命令名）
 * @argv: 参数数组，argv[0] 为命令名
 */
static void run_command(int argc, char **argv)
{
  if (argc == 0)
    return;

  /* ---- help 命令 ---- */
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

  /* ---- shell 命令：启动 RTEMS 标准 Shell ---- */
  if (strcmp(argv[0], "shell") == 0) {
    printf("Starting RTEMS shell... (type 'exit' to return)\n");
    /*
     * rtems_shell_init 参数说明:
     *   "SHLL"           — 任务名
     *   4*MIN_STACK_SIZE — 栈大小（Shell 需要较大栈）
     *   100              — 优先级
     *   "/dev/foobar"    — 设备名（非 PTY 时忽略，使用 stdin/stdout）
     *   false            — forever=false，输入 'exit' 后返回
     *   true             — wait=true，阻塞直到 Shell 退出
     *   NULL             — 无登录流程
     */
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

  /* ---- exit/quit 命令：退出 ---- */
  if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
    printf("Goodbye!\n");
    TEST_END();
    rtems_test_exit(0);
  }

  /* ---- cyclictest 命令：启动延迟测量 ---- */
  if (strcmp(argv[0], "cyclictest") == 0) {
    rtems_id tid;
    rtems_status_code sc;
    static cyclictest_task_arg cyc;
    volatile int done = 0;

    cyc.argc = argc;
    cyc.argv = argv;
    cyc.done = &done;

    /*
     * 创建一个独立的 RTEMS 任务来运行 cyclictest。
     *
     * 任务名 "CYCT" — 用于调试时在任务列表中识别。
     * 优先级 50   — 低于 Init 任务（通常在 1-10），但高于普通线程。
     * 栈大小 32*MIN — cyclictest 内部使用 POSIX 线程测量，需要较大栈空间。
     */
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

    /* 启动任务 */
    sc = rtems_task_start(tid, cyclictest_task, (rtems_task_argument)&cyc);
    if (sc != RTEMS_SUCCESSFUL) {
      printf("Error: %s\n", rtems_status_text(sc));
      rtems_task_delete(tid);
      return;
    }

    /*
     * 轮询等待 cyclictest 完成，每 100ms 检查一次 done 标志。
     * 最多等待 600 * 100ms = 60 秒，防止 cyclictest 因某种原因卡死
     * 导致 Init 任务永久阻塞。
     *
     * rtems_task_wake_after(tick) 让出 CPU 给 cyclictest 的测量线程。
     */
    for (int w = 0; w < 600 && !done; w++)
      rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 10);

    printf("\r\n");
    fflush(stdout);
    return;
  }

  /* 未知命令 */
  printf("Unknown command: '%s'.  Type 'help' for available commands.\n",
         argv[0]);
}

/*
 * Init — RTEMS 初始化任务入口（交互 Shell 模式）
 *
 * 这是系统启动后的第一个用户任务。
 * 初始化控制台、进入命令循环、处理用户输入。
 *
 * 核心流程：
 *   1. 禁用 stdio 缓冲（适配简单控制台驱动的轮询 I/O）
 *   2. 打印欢迎信息
 *   3. 进入主循环：
 *      a. 显示提示符 "cyclictest> "
 *      b. 轮询 UART 读取字符（10ms 间隔）
 *      c. 支持退格删除
 *      d. 回车后解析命令并执行
 *
 * 关键设计：使用 uart_getchar() 而非 fgets()/getchar()，
 * 因为 CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
 * 配置的控制台驱动不支持阻塞 read()。
 */
static rtems_task Init(rtems_task_argument ignored)
{
  char line[MAX_CMD_LEN];       /* 命令行缓冲区 */
  char *argv[16];               /* 最多 16 个参数 */
  int   argc;

  rtems_print_printer_fprintf_putc(&rtems_test_printer);

  /*
   * 禁用 stdio 缓冲 — 对简单控制台驱动至关重要。
   *
   * 简单控制台驱动使用轮询 I/O，当 stdin 被缓冲时，
   * getchar() 会等待填充整个缓冲区后才返回数据。
   * 设置 _IONBF（无缓冲）使每次 getchar() 直接从 UART 读取。
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
   * ===== 主命令循环 =====
   *
   * 轮询 getchar() — 简单控制台驱动在无字符可读时返回 EOF。
   * 在两次轮询之间短暂休眠以避免忙等待消耗 CPU。
   *
   * 休眠时间计算：
   *   ticks_per_second / 100 ≈ 每 10ms 唤醒一次
   *   如果 tick 粒度低于 100Hz，（即 tick > 10ms），至少睡眠 1 个 tick
   */
  while (1) {
    printf("\r\ncyclictest> ");
    fflush(stdout);

    /*
     * ===== 轮询字符输入 =====
     *
     * 这是 CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER 配置下
     * 唯一可靠的输入方式。
     *
     * fgets() 会立即返回 EOF，因为简单控制台驱动不提供阻塞 read()。
     * 因此我们直接轮询 uart_getchar() 逐字符读取。
     */
    int pos = 0;
    while (pos < MAX_CMD_LEN - 1) {
      int c = uart_getchar();     /* 非阻塞读取一个字符 */

      if (c == EOF) {
        /*
         * 无字符可读 — 休眠约 10ms 后重试。
         *
         * 如果系统 tick 粒度大于 10ms（即 ticks_per_second < 100），
         * 至少保证睡眠 1 个 tick，避免在 0 tick 的忙等待中消耗 100% CPU。
         */
        rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 100);
        if (rtems_clock_get_ticks_per_second() < 100)
          rtems_task_wake_after(1);  /* 至少 1 个 tick */
        continue;
      }

      /* 回车或换行：结束输入 */
      if (c == '\r' || c == '\n') {
        break;
      }

      /* 退格 (0x08) 或 DEL (0x7F)：删除前一个字符 */
      if (c == '\b' || c == 0x7f) {
        if (pos > 0) {
          pos--;
          putchar('\b');          /* 光标左移 */
          putchar(' ');           /* 擦除字符 */
          putchar('\b');          /* 光标再左移 */
        }
        continue;
      }

      /* 普通字符：存储并回显 */
      line[pos++] = (char)c;
      putchar(c);                 /* 回显到终端 */
      fflush(stdout);
    }
    line[pos] = '\0';             /* 字符串终止符 */
    printf("\r\n");
    fflush(stdout);

    /* 跳过空行（用户直接按回车） */
    if (pos == 0)
      continue;

    /* ===== 命令行解析：简单的空格/Tab 分割 ===== */
    /*
     * 将 line 原地分割为 argc/argv 数组。
     * 用 '\0' 替换分隔符，argv 指针直接指向 line 内的子串。
     * 这是一种经典的 C 命令行解析方式，无需动态内存分配。
     */
    argc = 0;
    char *p = line;
    while (*p && argc < 15) {
      /* 跳过前导空白 */
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '\0') break;

      /* argv[argc] 指向该参数的起始位置 */
      argv[argc++] = p;

      /* 找到参数结束位置（下一个空白或字符串尾） */
      while (*p && *p != ' ' && *p != '\t') p++;
      if (*p) { *p = '\0'; p++; }   /* 用 NUL 终止当前参数 */
    }
    argv[argc] = NULL;            /* Unix 惯例：argv 数组以 NULL 结尾 */

    run_command(argc, argv);
  }
}
#endif /* USE_SHELL */

/* ======================================================================
 *  RTEMS 内核配置
 *
 *  这些宏在编译时被 RTEMS 配置系统处理，生成内核数据结构。
 *  所有配置值根据 cyclictest 的需求估算，可在此调整。
 * ====================================================================== */

/*
 * 时钟驱动：提供 rtems_clock_get_ticks_per_second() 等时间服务
 * 简单控制台驱动：提供 printf/putchar/getchar，使用轮询 I/O
 */
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER

/*
 * 资源配额 — 根据 cyclictest 的使用场景估算：
 *
 * TASKS 50:           Init + Shell + cyclictest 任务 + 余量
 * POSIX_THREADS 10:   测量线程（每个 -t 创建一个）
 * POSIX_MUTEXES 10:   trigger_lock, refresh_on_max_lock, break_thread_id_lock 等
 * POSIX_CV 5:         refresh_on_max_cond 等条件变量
 * POSIX_TIMERS 0:     cyclictest 使用 clock_nanosleep 模式
 *                     （RTEMS POSIX 定时器支持有限）
 * POSIX_BARRIERS 5:   align_barr, globalt_barr（线程对齐用）
 */
#define CONFIGURE_MAXIMUM_TASKS              50
#define CONFIGURE_MAXIMUM_POSIX_THREADS      10
#define CONFIGURE_MAXIMUM_POSIX_MUTEXES      10
#define CONFIGURE_MAXIMUM_POSIX_CONDITION_VARIABLES  5
#define CONFIGURE_MAXIMUM_POSIX_TIMERS        0    /* 不使用 POSIX 定时器 */
#define CONFIGURE_MAXIMUM_POSIX_BARRIERS      5

/*
 * Init 任务配置：
 *   RTEMS_FLOATING_POINT — 启用浮点上下文切换
 *     cyclictest 使用 double 计算平均值，需要 FPU 支持
 *   RTEMS_TEST_INITIAL_EXTENSION — RTEMS 测试框架扩展
 */
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES RTEMS_FLOATING_POINT
#define CONFIGURE_INITIAL_EXTENSIONS RTEMS_TEST_INITIAL_EXTENSION

/*
 * 系统 tick 粒度：1000us = 1ms
 *
 * 这决定了 rtems_task_wake_after(1) 的最小睡眠时间。
 * 较小的值提供更好的时间精度，但增加时钟中断开销。
 * 1ms 是 cyclictest 测量实时延迟的合理基准。
 */
#define CONFIGURE_MICROSECONDS_PER_TICK    1000

/*
 * RTEMS Shell 配置 — 提供标准命令行工具
 *
 * 这些宏使能了 RTEMS Shell 基础设施，用户可以在交互模式下
 * 输入 "shell" 命令进入标准 RTEMS Shell 环境。
 * Shell 命令包括: ls, cat, mv, cp, rm, pwd, cd, mkdir 等。
 */
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

/* 必须的配置收尾宏 — 触发 RTEMS 配置表生成 */
#define CONFIGURE_INIT
#include <rtems/confdefs.h>

/* RTEMS Shell 配置头文件 */
#include <rtems/shellconfig.h>
