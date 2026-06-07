/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  cyclictest.h — RTEMS 移植版 cyclictest 公共头文件
 *
 *  来源：从 rt-tests-2.10 移植（原版作者: Thomas Gleixner, Clark Williams, John Kacur）
 *
 *  本文件统一定义：
 *    - 时间常量与单位转换宏
 *    - 测量模式枚举
 *    - 直方图与值缓冲区参数
 *    - 线程参数与统计数据的数据结构
 *    - 全局变量声明
 *    - 所有跨文件函数声明
 *
 *  ===== 相比原版移除的内容及原因 =====
 *
 *  1. NUMA 支持 — RTEMS 无 NUMA 硬件抽象，所有内存在单一域
 *  2. SMI 计数器 — /dev/cpu/N/msr 是 Linux 特有设备接口
 *  3. cpuidle/deepest-idle-state — Linux C-state 管理，RTEMS 不适用
 *  4. /dev/cpu_dma_latency — Linux PM QoS，RTEMS 无等效机制
 *  5. /proc/loadavg — Linux procfs，RTEMS 无 /proc 文件系统
 *  6. ftrace/tracemark — Linux 内核追踪机制，RTEMS 无 ftrace
 *  7. shm_open/mmap rstat — Linux 共享内存 IPC，RTEMS 使用直接内存访问
 *  8. NUMA 函数 (rt_numa_*) — 全部在 RTEMS 上退化为空操作
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

/* ===== 时间常量 ===== */
#define MSEC_PER_SEC        1000            /* 毫秒/秒 */
#define USEC_PER_SEC        1000000         /* 微秒/秒 */
#define NSEC_PER_SEC        1000000000LL    /* 纳秒/秒 */
#define USEC_TO_NSEC(u)     ((u) * 1000)   /* 微秒转纳秒 */
#define NSEC_TO_USEC(n)     ((n) / 1000)   /* 纳秒转微秒 */

/* ===== 默认参数 ===== */
#define DEFAULT_INTERVAL    1000     /* 默认测量间隔：1000us = 1ms */
#define DEFAULT_DISTANCE    500      /* 默认线程间隔：500us */

/* ===== 调度策略常量补丁 ===== */
/*
 * RTEMS 的 <sched.h> 可能未定义 SCHED_IDLE 和 SCHED_BATCH。
 * 这里提供与 Linux 一致的值，确保代码编译通过。
 * SCHED_NORMAL 在 Linux 上等同于 SCHED_OTHER（普通分时调度）。
 */
#ifndef SCHED_IDLE
#define SCHED_IDLE          5       /* 空闲调度策略（最低优先级） */
#endif
#ifndef SCHED_NORMAL
#define SCHED_NORMAL        SCHED_OTHER
#endif
#ifndef SCHED_BATCH
#define SCHED_BATCH         3       /* 批处理调度策略 */
#endif

/* ===== 测量模式 ===== */
/*
 * MODE_CYCLIC          — POSIX 定时器 (timer_create)，精度最高
 * MODE_CLOCK_NANOSLEEP — clock_nanosleep，精度较高，兼容性最好
 * MODE_SYS_ITIMER      — setitimer，Linux 特有，RTEMS 不支持
 * MODE_SYS_NANOSLEEP   — nanosleep，精度较低，RTEMS 兼容性差
 *
 * MODE_SYS_OFFSET 用于将 use_system 标志转换为模式偏移量。
 */
#define MODE_CYCLIC             0     /* POSIX 定时器模式 */
#define MODE_CLOCK_NANOSLEEP    1     /* clock_nanosleep 模式 */
#define MODE_SYS_ITIMER         2     /* setitimer — RTEMS 上不可用 */
#define MODE_SYS_NANOSLEEP      3     /* nanosleep — RTEMS 上不可用 */
#define MODE_SYS_OFFSET         2     /* 系统模式偏移量 */

/* 定时器模式：相对时间 vs 绝对时间 */
#define TIMER_RELTIME           0     /* 相对定时器 */
/* TIMER_ABSTIME 定义在 <time.h> 中，值为 4 */

/* ===== 直方图参数 ===== */
#define HIST_MAX                1000000  /* 直方图最大桶数（约 100 万桶） */

/* ===== 值缓冲区（用于 verbose 模式记录每次测量值） ===== */
/*
 * 必须是 2 的幂，这样用位掩码代替取模运算，提高效率。
 * bufmsk = VALBUF_SIZE - 1 = 0x3FFF
 * index = cycles & bufmsk 即可实现环形缓冲区索引。
 */
#define VALBUF_SIZE             16384   /* 缓冲区大小（2^14 = 16384） */

/* ===== 最大路径长度 ===== */
#define MAX_PATH                256

/* 数组长度宏（编译期计算） */
#define ARRAY_SIZE(x)           (sizeof(x) / sizeof((x)[0]))

/*
 * ===== gettid() — 获取当前线程ID =====
 *
 * Linux 使用 SYS_gettid 系统调用获取内核线程 ID。
 * RTEMS 没有这个系统调用，使用 pthread_self() 替代。
 * pthread_self() 返回的 pthread_t 在 RTEMS 上是唯一的线程标识符。
 */
#ifdef __rtems__
  /*
   * RTEMS: 无 NUMA 硬件抽象层。
   * 所有内存在单一域中。cyclictest.c 中的 numa 变量初始化为 0，
   * 所有 if(numa) 块被编译器作为死代码消除。
   */

  /*
   * RTEMS: 无 x86 SMI 计数器 (通过 /dev/cpu/N/msr)。
   * cyclictest.c 中的 smi 变量初始化为 0。
   * 原因: /dev/cpu/N/msr 是 Linux 特有设备接口。
   */

  /*
   * RTEMS: gettid() 被 pthread_self() 替代。
   * 原因: Linux gettid() 系统调用不存在于 RTEMS；
   * RTEMS POSIX 线程的 pthread_self() 提供唯一的线程标识。
   */
  static inline pid_t gettid(void)
  {
    return (pid_t)(uintptr_t)pthread_self();
  }

#else
  /*
   * 原版 Linux gettid() — 通过 SYS_gettid 系统调用获取内核线程 ID。
   * 内核线程 ID 是全局唯一的数字标识符，与 getpid() 获取的进程 ID 不同。
   */
  #include <sys/syscall.h>
  static inline pid_t gettid(void)
  {
    return (pid_t)syscall(SYS_gettid);
  }
#endif

/* ===== 数据结构 ===== */

/*
 * thread_param — 传递给每个测量线程的参数
 *
 * 每个测量线程创建时，通过此结构体接收其运行参数。
 * 线程只读取这些参数（const语义），不会修改。
 */
struct thread_param {
  int           prio;             /* 实时优先级（1-99，值越大优先级越高） */
  int           policy;           /* 调度策略: SCHED_FIFO, SCHED_RR, SCHED_OTHER 等 */
  int           mode;             /* 测量模式: MODE_CYCLIC / MODE_CLOCK_NANOSLEEP */
  int           timermode;        /* 定时器模式: TIMER_ABSTIME / TIMER_RELTIME */
  int           signal;           /* 用于唤醒的信号（默认 SIGALRM） */
  int           clock;            /* 时钟源: CLOCK_MONOTONIC / CLOCK_REALTIME */
  unsigned long max_cycles;       /* 最大测量周期数（0=无限） */
  struct thread_stat *stats;      /* 指向本线程的统计数据（线程写入） */
  int           bufmsk;           /* 值缓冲区掩码（VALBUF_SIZE - 1，环形缓冲用） */
  unsigned long interval;         /* 测量间隔（微秒） */
  int           cpu;              /* 绑定的 CPU 编号（-1=不绑定） */
  int           node;             /* NUMA 节点（-1=无，RTEMS 上始终为 -1） */
  int           tnum;             /* 线程索引号（0, 1, 2, ...） */
  int           msr_fd;           /* SMI MSR 文件描述符（-1=无，RTEMS 上始终为 -1） */
};

/*
 * thread_stat — 每个测量线程的统计数据
 *
 * 线程在测量循环中持续更新这些统计值。
 * 主线程读取这些值用于输出显示。
 */
struct thread_stat {
  unsigned long cycles;           /* 已完成的测量周期计数 */
  unsigned long cyclesread;       /* 已读取（输出）的周期计数（verbose 模式用） */
  long          min;              /* 最小延迟（微秒） */
  long          max;              /* 最大延迟（微秒） */
  long          act;              /* 当前（最近一次）延迟（微秒） */
  double        avg;              /* 累积延迟总和（用于计算平均值 = avg/cycles） */
  long         *values;           /* 值环形缓冲区（verbose 模式，存储每次测量值） */
  long         *smis;             /* SMI 值缓冲区（RTEMS 上为 NULL） */
  struct histogram *hist;         /* 指向本线程的直方图 */
  pthread_t     thread;           /* 线程句柄 */
  int           threadstarted;    /* 线程启动标志：1=运行中, -1=已退出, 0=未启动 */
  int           tid;              /* 线程 ID（由 gettid() 获取） */
  long          reduce;           /* 降采样计数器（oscope_reduction 模式） */
  long          redmax;           /* 降采样周期内的最大延迟 */
  long          cycleofmax;       /* 降采样周期内最大延迟对应的周期号 */
  unsigned long smi_count;        /* SMI 中断计数（RTEMS 上始终为 0） */
};

/*
 * thread_trigger — 尖峰/触发记录节点
 *
 * 当延迟超过 trigger 阈值时，记录该事件的详细信息。
 * 使用单向链表存储，预分配固定数量的节点。
 */
struct thread_trigger {
  int    cpu;                     /* 发生尖峰的 CPU */
  int    tnum;                    /* 发生尖峰的线程号 */
  int64_t ts;                     /* 时间戳 */
  int    diff;                    /* 延迟值 */
  struct thread_trigger *next;    /* 下一个节点 */
};

/* histogram 结构体的前向声明（定义在 histogram.h 中） */
struct histogram;
struct histoset;

/* ===== CPU 亲和性枚举 ===== */
/*
 * AFFINITY_UNSPECIFIED — 未指定亲和性（线程可在任意 CPU 上运行）
 * AFFINITY_SPECIFIED   — 用户通过 -a 参数指定了 CPU 绑定
 * AFFINITY_USEALL      — 使用所有可用 CPU（SMP 模式或 -a 无参数）
 */
enum {
  AFFINITY_UNSPECIFIED = 0,
  AFFINITY_SPECIFIED   = 1,
  AFFINITY_USEALL      = 2,
};

/* ===== 跨文件共享的全局变量 ===== */
extern int clocksources[];        /* 可用时钟源数组: CLOCK_MONOTONIC, CLOCK_REALTIME */

/* ===== 函数声明 ===== */

/* 来自 cyclictest.c */
void *timerthread(void *param);                           /* 核心测量线程 */
void display_help(int error);                              /* 打印帮助信息 */
void handlepolicy(char *polname);                          /* 解析调度策略名称 */
char *policyname(int policy);                              /* 调度策略转字符串 */
void process_options(int argc, char *argv[], int max_cpus); /* 解析命令行参数 */
int  check_timer(void);                                    /* 检查高精度定时器可用性 */
void sighand(int sig);                                     /* 信号处理器 */
void print_tids(struct thread_param *par[], int nthreads); /* 打印线程 ID 列表 */
void print_hist(struct thread_param *par[], int nthreads); /* 打印延迟直方图 */
void print_stat(FILE *fp, struct thread_param *par, int index,
                int verbose, int quiet);                   /* 打印线程统计信息 */
void write_stats(FILE *f, void *data);                     /* JSON 格式输出统计 */
void rt_init(int argc, char *argv[]);                      /* RT 初始化（简化） */
void rt_write_json(const char *filename, int return_code,
                   void (*cb)(FILE *, void *), void *data); /* JSON 文件输出 */
int  parse_time_string(char *val);                         /* 解析时间字符串（如 "5m"） */
const char *policy_to_string(int policy);                  /* 调度策略枚举转字符串 */
uint32_t string_to_policy(const char *str);                 /* 字符串转调度策略枚举 */

/* 来自 init.c */
int cyclictest_main(int argc, char *argv[]);               /* cyclictest 主入口（替代 main） */

#endif /* _CYCLICTEST_H */
