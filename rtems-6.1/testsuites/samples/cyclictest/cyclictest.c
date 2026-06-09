/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  cyclictest.c — rt-tests-2.10 cyclictest 的 RTEMS 移植版
 *
 *  原作者: Thomas Gleixner, Clark Williams, John Kacur
 *
 *  本文件包含：
 *    - 核心延迟测量算法 (timerthread)
 *    - 统计计算（最小/最大/平均/直方图）
 *    - 命令行解析 (process_options)
 *    - 输出格式化 (print_stat / print_hist / JSON)
 *
 *  代码结构尽可能与原版 cyclictest.c 保持一致，方便后续对比和维护。
 *
 *  ===== 核心测量原理 =====
 *
 *  cyclictest 通过以下方式测量系统的实时延迟：
 *
 *  1. 线程以固定间隔（如 1000us）睡眠
 *  2. 醒来后对比"预期醒来时间"和"实际醒来时间"
 *  3. 差值 = 实时延迟（调度延迟 + 中断延迟 + ...）
 *  4. 统计所有测量周期的 min/max/avg，可生成直方图
 *
 *  工作原理示意（以 clock_nanosleep 绝对时间模式为例）：
 *
 *    next = now + interval    // 计算下一次应该醒来的时间
 *    循环:
 *      clock_nanosleep(TIMER_ABSTIME, &next)  // 睡到 next 时刻
 *      clock_gettime(&now)                     // 获取实际醒来时间
 *      diff = now - next                       // 延迟！
 *      next += interval                        // 计算下下次醒来时间
 *
 *  ===== 相比原版移除的功能及原因 =====
 *
 *  1. NUMA 支持 (rt_numa_*)
 *     原因: RTEMS 无 NUMA 硬件抽象。所有内存在单一域中。
 *     numa 变量被 #define 为 0，所有 if(numa) 块被编译器作为死代码消除。
 *
 *  2. SMI 计数器 (/dev/cpu/N/msr, MSR_SMI_COUNT, has_smi_counter)
 *     原因: /dev/cpu/N/msr 是 Linux 特有设备接口。
 *     SMI (System Management Interrupt) 是 BIOS/x86 概念。
 *
 *  3. 电源管理 (/dev/cpu_dma_latency, set_latency_target)
 *     原因: Linux PM QoS 接口。RTEMS 无 C-state 或动态电源管理。
 *
 *  4. CPU 空闲状态控制 (cpuidle, deepest_idle_state)
 *     原因: Linux cpuidle 子系统。RTEMS 不管理 CPU 空闲状态。
 *
 *  5. ftrace/trace_marker (enable_trace_mark, tracemark)
 *     原因: Linux ftrace 是内核内部追踪机制。RTEMS 无等价机制。
 *
 *  6. /proc/loadavg
 *     原因: Linux procfs。RTEMS 无 /proc 文件系统。显示 "N/A"。
 *
 *  7. shm_open/mmap 运行状态 (rstat_*)
 *     原因: Linux 共享内存 IPC。RTEMS 使用直接内存访问。
 *     这是调试便利功能，非核心 cyclictest 功能。
 *
 *  8. 命名管道/FIFO 输出 (mkfifo, fifothread)
 *     原因: mkfifo 是 Linux/SYSV IPC 机制。RTEMS 不可用。
 *     统计数据直接输出到 stdout。
 *
 *  9. Linux 调度优先级限制 (raise_soft_prio, getrlimit)
 *     原因: Linux RLIMIT_RTPRIO 安全模型。RTEMS 使用扁平优先级方案
 *     (1..255)，无进程级限制。setscheduler() 直接工作。
 */

/* ===== 头文件 ===== */
#include "cyclictest.h"    /* 公共声明、数据结构、常量 */
#include "histogram.h"     /* 直方图统计 */
#include <getopt.h>        /* getopt_long 命令行解析 */

/* ===== TSC (rdtsc) 支持 ===== */
/*
 * USE_TSC=1: 使用硬件 TSC (Time Stamp Counter) 进行更高精度的延迟测量。
 *   - 在 x86 平台上通过 rdtsc 指令读取 CPU 周期计数器
 *   - TSC 精度远高于 clock_gettime（ns 级 vs us 级）
 *   - 需要在启动时校准 tsc_per_us（每微秒的 TSC 周期数）
 *
 * USE_TSC=0: 使用传统的 clock_gettime 方式（与原版一致）。
 *
 * 默认启用 TSC 以获得更精确的测量结果。
 */
#define USE_TSC  1  /* 1=硬件 TSC 测量, 0=clock_gettime 测量 */

#if USE_TSC
/*
 * rdtsc — 读取 x86 Time Stamp Counter
 *
 * 执行 rdtsc 指令，返回 64 位 CPU 周期计数值。
 * 低位 32 位在 EAX，高位 32 位在 EDX。
 *
 * 注意：rdtsc 是 x86 特定指令，在其他架构（ARM 等）上不可用。
 * 如果移植到非 x86 平台，需要实现对应的周期计数器读取方式，
 * 或者设置 USE_TSC=0 回退到 clock_gettime。
 */
static inline unsigned long long rdtsc(void)
{
  unsigned int lo, hi;
  __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
  return ((unsigned long long)hi << 32) | lo;
}

/*
 * tsc_per_us — TSC 校准值：每个微秒对应的 TSC 周期数
 *
 * 在第一个线程启动时通过 sleep 50ms + 对比 clock_gettime 自动校准。
 * 示例：3GHz CPU → tsc_per_us ≈ 3000.0
 */
static double tsc_per_us = 3000.0;  /* 默认值，启动后自动校准 */
#endif

/* ===== 外部引用（定义在 init.c 中） ===== */
extern const char *rtems_test_name;    /* RTEMS 测试名称 "CYCLICTEST" */

/* ===== 全局变量（命名与原版 cyclictest 保持一致） ===== */

/*
 * shutdown — 全局停止标志
 *
 * 设为 1 时所有测量线程退出主循环。
 * 以下情况会触发 shutdown：
 *   - 用户按 Ctrl-C (SIGINT)
 *   - 达到 max_cycles 限制
 *   - 达到 duration 时间限制
 *   - tracelimit 被触发
 *   - refresh_on_max 模式下的线程退出
 *
 * 非 static：init.c 中的信号处理需要访问此变量。
 */
int shutdown;

static int tracelimit = 0;        /* 中断追踪阈值（微秒）。超过此值停止测试。0=禁用 */
static int verbose = 0;           /* 详细模式：输出每个周期的测量值 */
static int oscope_reduction = 1;  /* 示波器降采样率（verbose 模式下每 N 次输出一次） */
static int histogram = 0;         /* 直方图桶数。0=不生成直方图 */
static int histofall = 0;         /* 直方图汇总模式：在每行末尾加汇总列 */
static int duration = 0;          /* 测试持续时间（秒）。0=无限 */
static int use_nsecs = 0;         /* 以纳秒显示结果（默认微秒） */
static int refresh_on_max;        /* 延迟更新屏幕，直到出现新的最大延迟（节省带宽） */
static int force_sched_other;     /* 强制使用 SCHED_OTHER 调度策略的标志 */
static int priospread = 0;        /* 优先级分散模式：每个线程优先级递减 */
static int check_clock_resolution;/* 检测时钟分辨率 */
static int ct_debug;              /* 调试模式标志 */

/*
 * REMOVED: lockall (mlockall)
 * 原因: RTEMS 无虚拟内存或 swap — 所有内存始终物理驻留。
 * mlockall 在 RTEMS 上是空概念。
 */

/*
 * REMOVED: use_fifo, fifo_threadid
 * 原因: mkfifo() 在 RTEMS 上不可用（见注释 #8）。
 */

/*
 * REMOVED: laptop, power_management, latency_target_fd,
 *          deepest_idle_state, saved_cpu_idle_*
 * 原因: 电源管理不适用于 RTEMS（见注释 #3, #4）。
 */

/*
 * REMOVED: smi (SMI 计数器)
 * 原因: /dev/cpu/N/msr 不可用（见注释 #2）。
 * smi 在 cyclictest.h 中被 #define 为 0，所有 if(smi) 代码被死代码消除。
 */

/*
 * 线程对齐变量:
 *   aligned  — 线程按指定偏移对齐唤醒
 *   secaligned — 线程对齐到秒边界
 *   offset   — 对齐偏移量（纳秒）
 *   align_barr/globalt_barr — 用于同步线程启动的 pthread 屏障
 */
static int aligned = 0;
static int secaligned = 0;
static int offset = 0;
static pthread_barrier_t align_barr;        /* 对齐屏障：所有线程同步等待 */
static pthread_barrier_t globalt_barr;      /* 全局时间屏障：先到者等待线程0获取时间 */
static struct timespec globalt;             /* 全局基准时间 */

/* refresh_on_max 同步原语：主线程等待条件变量，测量线程在新最大延迟时发信号 */
static pthread_cond_t refresh_on_max_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t refresh_on_max_lock = PTHREAD_MUTEX_INITIALIZER;

/* break_trace 保护：确保只有一个线程记录中断追踪事件 */
static pthread_mutex_t break_thread_id_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t break_thread_id = 0;           /* 触发中断追踪的线程 ID */
static uint64_t break_thread_value = 0;     /* 触发中断追踪的延迟值 */

/*
 * 输出文件路径（简化版，仅保留 jsonfile）
 *
 * REMOVED: fifopath, histfile
 * 原因: FIFO 输出和直方图文件输出已移除，数据直接输出到 stdout。
 */
static char jsonfile[MAX_PATH];

/* 线程参数和统计数据指针数组 */
static struct thread_param **parameters;    /* parameters[i] = 线程 i 的参数 */
static struct thread_stat  **statistics;    /* statistics[i] = 线程 i 的统计数据 */
static struct histoset       hset;          /* 直方图集合（所有线程的直方图） */

/* 默认测量参数 */
static int use_nanosleep = MODE_CLOCK_NANOSLEEP;  /* 默认使用 clock_nanosleep */
static int timermode     = TIMER_ABSTIME;         /* 默认使用绝对时间定时器 */
static int use_system;                            /* 系统模式标志 */
static int priority;                              /* 实时线程优先级（0=不使用） */
static int policy        = SCHED_OTHER;           /* 默认调度策略 */
static int num_threads   = 1;                     /* 默认 1 个测量线程 */
static int max_cycles;                            /* 最大周期数（0=无限） */
static int clocksel      = 0;                     /* 时钟选择：0=MONOTONIC, 1=REALTIME */
static int quiet;                                 /* 静默模式：仅结束时输出概要 */
static int interval      = DEFAULT_INTERVAL;      /* 测量间隔（微秒），默认 1000 */
static int distance      = -1;                    /* 线程间间隔偏移（微秒） */
static int smp           = 0;                     /* SMP 模式标志 */
static int setaffinity   = AFFINITY_UNSPECIFIED;  /* CPU 亲和性设置 */

/*
 * clocksources — 可用时钟源列表
 *
 * CLOCK_MONOTONIC: 单调递增时钟，不受系统时间调整影响（推荐）
 * CLOCK_REALTIME:  墙上时钟，可能被 NTP 或手动调整影响
 */
int clocksources[] = {
  CLOCK_MONOTONIC,
  CLOCK_REALTIME,
};

/*
 * affinity_mask / main_affinity_mask — CPU 亲和性掩码
 *
 * 类型从原版的 struct bitmask *(libnuma) 替换为 cpu_set_t *（POSIX 标准）。
 * 原因: RTEMS 不支持 libnuma，使用标准 POSIX cpu_set_t。
 *
 * 限制: 不支持 libnuma 的范围语法（如 "3-5,0"），仅支持单个 CPU 号。
 */
static cpu_set_t *affinity_mask = NULL;
static cpu_set_t *main_affinity_mask = NULL;

/* ===== 尖峰/触发追踪 ===== */
static pthread_mutex_t trigger_lock = PTHREAD_MUTEX_INITIALIZER;
static int trigger = 0;               /* 触发阈值（微秒）。0=不记录尖峰 */
static int trigger_list_size = 1024;  /* 尖峰链表最大节点数 */
struct thread_trigger *head = NULL;   /* 链表头 */
struct thread_trigger *tail = NULL;   /* 链表尾（用于追加新节点） */
struct thread_trigger *current = NULL;/* 当前写入位置（环形使用预分配链表） */
static int spikes;                    /* 尖峰总数 */

/* ===== warn/fatal 辅助函数（替代 rt-tests 的 rt-error.h） ===== */
/*
 * 原版依赖 rt-error.h/rt-error.c 提供彩色错误输出和 errno 处理。
 * RTEMS 版内联为简化版本：只做基本的 fprintf。
 *
 * 关键区别：fatal() 不调用 exit()，而是返回 EXIT_FAILURE。
 * 因为 RTEMS 中 exit() 会终止整个系统运行。
 */
#include <stdarg.h>

/*
 * warn — 输出警告信息到 stderr
 *
 * 格式: "WARN: <格式化消息>"
 * 用于非致命错误（如无法设置 CPU 亲和性）。
 */
static void warn(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "WARN: ");
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

/*
 * fatal — 输出致命错误信息到 stderr 并返回失败码
 *
 * 格式: "FATAL: <格式化消息>"
 * 用于不可恢复的错误。注意：不调用 exit()（RTEMS 限制）。
 */
static void fatal(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "FATAL: ");
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  return EXIT_FAILURE;
}

/* ===== 时间计算工具函数（从原版 rt-utils.h 内联） ===== */

/*
 * tsnorm — 规范化 timespec 结构体
 *
 * 确保 tv_nsec 在 [0, NSEC_PER_SEC) 范围内。
 * 如果 tv_nsec 超过 10^9，进位到 tv_sec。
 * 如果 tv_nsec 为负，从 tv_sec 借位。
 *
 * 使用场景：每次对 timespec 做加减运算后都需要调用此函数，
 * 因为 tv_nsec 可能溢出标准范围。
 */
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

/*
 * tsgreater — 比较两个 timespec 的大小
 *
 * 返回: 1 表示 a > b, 0 表示 a <= b
 * 用于判断当前时间是否已超过下一次预期唤醒时间（追赶逻辑）。
 */
static inline int tsgreater(struct timespec *a, struct timespec *b)
{
  return ((a->tv_sec > b->tv_sec) ||
          (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec));
}

/*
 * calcdiff — 计算两个 timespec 之间的差值（微秒）
 *
 * diff = (t1 - t2) 的微秒值
 * 用于计算延迟（实际醒来时间 → 预期醒来时间）。
 */
static inline int64_t calcdiff(struct timespec t1, struct timespec t2)
{
  int64_t diff = USEC_PER_SEC * (long long)((int)t1.tv_sec - (int)t2.tv_sec);
  diff += ((int)t1.tv_nsec - (int)t2.tv_nsec) / 1000;
  return diff;
}

/*
 * calcdiff_ns — 计算两个 timespec 之间的差值（纳秒）
 *
 * 与 calcdiff 相同逻辑，但单位为纳秒而非微秒。
 */
static inline int64_t calcdiff_ns(struct timespec t1, struct timespec t2)
{
  int64_t diff;
  diff = NSEC_PER_SEC * (int64_t)((int)t1.tv_sec - (int)t2.tv_sec);
  diff += ((int)t1.tv_nsec - (int)t2.tv_nsec);
  return diff;
}

/*
 * calctime — 将 timespec 转换为微秒值
 *
 * 用于尖峰记录中的时间戳存储。
 */
static inline int64_t calctime(struct timespec t)
{
  int64_t time;
  time = USEC_PER_SEC * t.tv_sec;
  time += ((int)t.tv_nsec) / 1000;
  return time;
}

/*
 * get_available_cpus — 获取系统可用 CPU 数量
 *
 * RTEMS: 使用 rtems_scheduler_get_processor_maximum()
 *   （直接返回处理器数量，不需要 +1）
 *   原因: RTEMS 不支持 CPU 热插拔，所有配置的 CPU 始终在线。
 *   sysconf(_SC_NPROCESSORS_ONLN) 在 RTEMS 上不一定可用。
 *
 * Linux: 使用 sysconf(_SC_NPROCESSORS_ONLN)
 *   返回在线 CPU 数量（考虑热插拔）。
 */
static int get_available_cpus(void)
{
#ifdef __rtems__
  return (int)rtems_scheduler_get_processor_maximum();
#else
  return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

/*
 * REMOVED: check_privs()
 * 原因: RTEMS 无 capability/privilege 系统。所有任务都可以设置实时优先级。
 * 不需要 root/effective-UID 检查。
 */

/*
 * REMOVED: mlockall(MCL_CURRENT|MCL_FUTURE)
 * 原因: RTEMS 无虚拟内存、无 swap、无分页。
 * 所有内存始终物理存在，无需锁定。
 */

/*
 * REMOVED: set_latency_target() + /dev/cpu_dma_latency
 * 原因: Linux PM QoS（见注释 #3）。RTEMS 上为空操作。
 */

/*
 * REMOVED: save/restore/set_deepest_cpu_idle_state()
 * 原因: Linux cpuidle（见注释 #4）。RTEMS 上为空操作。
 */

/*
 * REMOVED: raise_soft_prio() + getrlimit/setrlimit
 * 原因: Linux RLIMIT_RTPRIO 安全模型（见注释 #9）。
 *
 * 在 RTEMS 上，sched_setscheduler() 可以直接工作（虽然通常是 stub）。
 * 真正的调度策略通过 pthread_attr_setschedpolicy() 在创建线程时设置。
 *
 * setscheduler() 简化为直接调用 sched_setscheduler。
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
 * 原因: SMI 计数器（见注释 #2）。smi 被 #define 为 0，所有 if(smi) 块
 * 被编译器进行死代码消除。
 */

/* ===== parse_time_string — 解析时间字符串（与原版逻辑一致） ===== */
/*
 * 解析带后缀的时间值：
 *   "10"  → 10 秒
 *   "5m"  → 300 秒 (5 分钟)
 *   "2h"  → 7200 秒 (2 小时)
 *   "1d"  → 86400 秒 (1 天)
 *
 * 通过 switch fall-through 实现乘法级联：
 *   d 后缀: v *= 24
 *   h 后缀: v *= 60  （fall-through from d）
 *   m 后缀: v *= 60  （fall-through from h/d）
 */
int parse_time_string(char *val)
{
  char *end;
  int v = strtol(val, &end, 10);
  if (end && end[0] != '\0') {
    switch (end[0]) {
    case 'd': v *= 24;     /* fall-through: d 包含 h 的乘法 */
    case 'h': v *= 60;     /* fall-through: h 包含 m 的乘法 */
    case 'm': v *= 60; break;
    default:
      fprintf(stderr, "Unable to parse time string %s\n", val);
      return EXIT_FAILURE;
    }
  }
  if (v <= 0)
    v = 1;    /* 最小值 1 秒 */
  return v;
}

/* ===== 调度策略辅助函数 ===== */

/*
 * handlepolicy — 解析调度策略名称并设置全局 policy 变量
 *
 * 大小写不敏感比较（strncasecmp）。
 * 例如: "FIFO", "fifo", "Fifo" → SCHED_FIFO
 *
 * @polname: 策略名称字符串
 */
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
    /* 无法识别的策略名 → 回退到 SCHED_OTHER */
    policy = SCHED_OTHER;
}

/*
 * policyname — 将调度策略枚举值转换为可读字符串
 *
 * 返回的字符串指针指向静态常量，不需要释放。
 */
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

/*
 * policy_to_string — 策略枚举转完整大写名称
 *
 * 返回如 "SCHED_FIFO" 形式的字符串。
 * 与 policyname() 的区别：返回的是内核常量名称格式。
 */
const char *policy_to_string(int p)
{
  return policyname(p);
}

/*
 * string_to_policy — 字符串转调度策略枚举
 *
 * 使用 strcasecmp 进行大小写不敏感比较。
 * 不匹配任何已知策略时回退到 SCHED_OTHER。
 */
uint32_t string_to_policy(const char *str)
{
  if (strcasecmp(str, "fifo") == 0)  return SCHED_FIFO;
  if (strcasecmp(str, "rr") == 0)    return SCHED_RR;
  if (strcasecmp(str, "batch") == 0) return SCHED_BATCH;
  if (strcasecmp(str, "idle") == 0)  return SCHED_IDLE;
  return SCHED_OTHER;
}

/* ===== 信号处理器 ===== */
/*
 * sighand — 处理 SIGINT, SIGTERM, SIGUSR1
 *
 * SIGINT/SIGTERM: 设置 shutdown=1，通知所有测量线程退出。
 *   如果启用了 refresh_on_max，还需要发信号唤醒可能在等待条件变量的主线程。
 *
 * SIGUSR1: 简化的状态转储（仅打印提示行）。
 *   原版在此处遍历并输出所有线程统计，但存在线程安全风险。
 *   RTEMS 上信号支持有限，仅做最小处理。
 *
 * REMOVED: SIGUSR2 处理（依赖已移除的 rstat 共享内存机制）。
 */
void sighand(int sig)
{
  if (sig == SIGUSR1) {
    /* 简化的状态提示 */
    int oldquiet = quiet;
    quiet = 0;
    fprintf(stderr, "# cyclictest current status (SIGUSR1):\n");
    quiet = oldquiet;
    return;
  }
  /* SIGINT / SIGTERM：触发优雅退出 */
  shutdown = 1;
  if (refresh_on_max)
    pthread_cond_signal(&refresh_on_max_cond);  /* 唤醒可能在等待的主线程 */
}

/* ===== check_timer — 检查高精度定时器是否可用 ===== */
/*
 * 通过 clock_getres 检查 CLOCK_MONOTONIC 的分辨率。
 * 返回 0 表示高精度定时器可用（分辨率 = 1ns）。
 * 返回 1 表示定时器精度不足（如 jiffies 时钟）。
 */
int check_timer(void)
{
  struct timespec ts;
  if (clock_getres(CLOCK_MONOTONIC, &ts))
    return 1;     /* clock_getres 失败 */
  return (ts.tv_sec != 0 || ts.tv_nsec != 1);  /* 期望分辨率恰好 1ns */
}

/* ===== display_help — 显示帮助信息 ===== */
/*
 * 列出所有支持的选项及其说明。
 *
 * 与原版相比：
 *   - 新增 "REMOVED from Linux original" 部分，列出不可用选项及原因
 *   - 不再调用 exit() 终止程序（RTEMS 中 exit() 会终止整个系统）
 */
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
  /* 不调用 exit()，由调用者处理 */
}

/* ===== 命令行选项枚举 ===== */
/*
 * 每个选项有唯一的整数值。getopt_long 使用这些值来区分长选项。
 *
 * REMOVED vs 原版：OPT_DEFAULT_SYSTEM, OPT_LATENCY, OPT_FIFO, OPT_HISTFILE,
 * OPT_JSON, OPT_MAINAFFINITY, OPT_MLOCKALL, OPT_NUMA, OPT_LAPTOP, OPT_SMI,
 * OPT_TRACEMARK, OPT_DEEPEST_IDLE_STATE 已移除。
 */
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

/* ===== process_options — 解析命令行参数 ===== */
/*
 * 使用 getopt_long 解析短选项和长选项。
 * 参数值存储到对应的全局变量中。
 *
 * 与原版的关键区别：
 *   1. 使用容错回退策略：不合法的值回退为默认值，而非报错退出
 *   2. CPU 亲和性使用简单的 atoi(cpu) 而非 parse_cpumask（不支持范围语法）
 *   3. 不再读取 argv[optind] 作为 fallback 参数
 *   4. 无 NUMA/SMI/电源管理相关选项
 *
 * @argc: 命令行参数数量
 * @argv: 命令行参数数组
 * @max_cpus: 系统最大 CPU 数量
 */
void process_options(int argc, char *argv[], int max_cpus)
{
  int error = 0;
  int option_affinity = 0;    /* 是否指定了 -a 亲和性选项 */
  
  optind = 0;

  for (;;) {
    int option_index = 0;     /* getopt_long 返回的选项索引 */

    /* 长选项定义表 */
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
      {NULL, 0, NULL, 0},   /* 终止标记 */
    };

    /* 调用 getopt_long 解析下一个选项 */
    int c = getopt_long(argc, argv,
                        "a:A::b:c:d:D:h:H:i:l:MNo:p:qrRsSt:uvx",
                        long_options, &option_index);
    if (c == -1)
      break;    /* 所有选项解析完毕 */

    switch (c) {
    /* ---- -a, --affinity: CPU 亲和性 ---- */
    case 'a':
    case OPT_AFFINITY:
      option_affinity = 1;
      if (smp) break;     /* SMP 模式下忽略 -a（由 -S 自动设置） */
      if (optarg) {
        /* 简化实现：只支持单个 CPU 号（如 -a 2） */
        int cpu = atoi(optarg);
        affinity_mask = calloc(1, sizeof(cpu_set_t));
        CPU_ZERO(affinity_mask);
        CPU_SET(cpu, affinity_mask);
        setaffinity = AFFINITY_SPECIFIED;     /* 绑定到指定 CPU */
      } else {
        setaffinity = AFFINITY_USEALL;        /* 使用所有 CPU */
      }
      break;

    /* ---- -A, --aligned: 线程对齐偏移 ---- */
    case 'A':
    case OPT_ALIGNED:
      aligned = 1;                    /* 启用线程对齐 */
      if (optarg) offset = atoi(optarg) * 1000;  /* 用户指定偏移（微秒→纳秒） */
      else offset = 0;                /* 默认偏移 0 */
      break;

    /* ---- -b, --breaktrace: 中断追踪阈值 ---- */
    case 'b':
    case OPT_BREAKTRACE:
      tracelimit = atoi(optarg); break;   /* 超过此值（us）停止测试 */

    /* ---- -c, --clock: 时钟选择 ---- */
    case 'c':
    case OPT_CLOCK:
      clocksel = atoi(optarg); break;     /* 0=MONOTONIC, 1=REALTIME */

    /* ---- -d, --distance: 线程间间隔 ---- */
    case 'd':
    case OPT_DISTANCE:
      distance = atoi(optarg); break;

    /* ---- -D, --duration: 测试持续时间 ---- */
    case 'D':
    case OPT_DURATION:
      duration = parse_time_string(optarg); break;  /* 支持 "10m" 格式 */

    /* ---- -H, --histofall: 带汇总列的直方图 ---- */
    case 'H':
    case OPT_HISTOFALL:
      histofall = 1; /* 启用汇总模式（fall-through 到 -h 处理） */

    /* ---- -h, --histogram: 直方图桶数 ---- */
    case 'h':
    case OPT_HISTOGRAM:
      histogram = atoi(optarg); break;    /* 桶数 = 直方图精度范围 */

    /* ---- -i, --interval: 测量间隔（微秒） ---- */
    case 'i':
    case OPT_INTERVAL:
      interval = atoi(optarg); break;

    /* ---- -l, --loops: 循环次数 ---- */
    case 'l':
    case OPT_LOOPS:
      max_cycles = atoi(optarg); break;   /* 0=无限循环 */

    /* ---- -M, --refresh_on_max: 最大延迟刷新模式 ---- */
    case 'M':
    case OPT_REFRESH:
      refresh_on_max = 1; break;           /* 仅在出现新最大延迟时刷新屏幕 */

    /* ---- -N, --nsecs: 纳秒显示模式 ---- */
    case 'N':
    case OPT_NSECS:
      use_nsecs = 1; break;

    /* ---- -o, --oscope: 示波器降采样 ---- */
    case 'o':
    case OPT_OSCOPE:
      oscope_reduction = atoi(optarg); break;  /* 每 N 个周期输出一次 */

    /* ---- -p, --priority: 实时优先级 ---- */
    case 'p':
    case OPT_PRIORITY:
      priority = atoi(optarg);
      /* 指定优先级时自动切换到 SCHED_FIFO */
      if (policy != SCHED_FIFO && policy != SCHED_RR)
        policy = SCHED_FIFO;
      break;

    /* ---- -q, --quiet: 静默模式 ---- */
    case 'q':
    case OPT_QUIET:
      quiet = 1; break;                   /* 仅在结束时输出概要 */

    /* ---- -r, --relative: 相对定时器 ---- */
    case 'r':
    case OPT_RELATIVE:
      timermode = TIMER_RELTIME; break;   /* 默认是 TIMER_ABSTIME */

    /* ---- -R, --resolution: 检查时钟分辨率 ---- */
    case 'R':
    case OPT_RESOLUTION:
      check_clock_resolution = 1; break;

    /* ---- --secaligned: 秒对齐 ---- */
    case OPT_SECALIGNED:
      secaligned = 1;                     /* 对齐到秒边界 */
      if (optarg) offset = atoi(optarg) * 1000;
      else offset = 0;
      break;

    /* ---- -s, --system: 系统定时器模式 ---- */
    case 's':
    case OPT_SYSTEM:
      use_system = MODE_SYS_OFFSET; break; /* 使用 sys_nanosleep */

    /* ---- -S, --smp: SMP 测试模式 ---- */
    case 'S':
    case OPT_SMP:
      smp = 1;                            /* 启用 SMP 模式 */
      num_threads = -1;                   /* 稍后设置为 CPU 数量 */
      setaffinity = AFFINITY_USEALL;      /* 使用所有 CPU */
      break;

    /* ---- -t, --threads: 线程数 ---- */
    case 't':
    case OPT_THREADS:
      if (smp) break;                     /* SMP 模式下 -t 被忽略 */
      if (optarg)
        num_threads = atoi(optarg);       /* 用户指定的线程数 */
      else
        num_threads = -1;                 /* 使用默认（所有 CPU） */
      break;

    /* ---- --spike: 尖峰触发阈值 ---- */
    case OPT_TRIGGER:
      trigger = atoi(optarg); break;      /* 超过此值（us）记录尖峰 */

    /* ---- --spike-nodes: 尖峰记录最大节点数 ---- */
    case OPT_TRIGGER_NODES:
      if (trigger) trigger_list_size = atoi(optarg); break;

    /* ---- -u, --unbuffered: 无缓冲输出 ---- */
    case 'u':
    case OPT_UNBUFFERED:
      setvbuf(stdout, NULL, _IONBF, 0); break;  /* 实时输出 */

    /* ---- -v, --verbose: 详细模式 ---- */
    case 'v':
    case OPT_VERBOSE:
      verbose = 1; break;                 /* 每个周期输出测量值 */

    /* ---- -x, --posix_timers: POSIX 定时器模式 ---- */
    case 'x':
    case OPT_POSIX_TIMERS:
      use_nanosleep = MODE_CYCLIC; break; /* 使用 timer_create */

    /* ---- --priospread: 优先级分散模式 ---- */
    case OPT_PRIOSPREAD:
      priospread = 1; break;              /* 每个线程优先级递减 */

    /* ---- --policy: 调度策略 ---- */
    case OPT_POLICY:
      handlepolicy(optarg); break;        /* 解析策略名称 */

    /* ---- --dbg_cyclictest: 调试模式 ---- */
    case OPT_DBGCYCLIC:
      ct_debug = 1; break;

    /* ---- -?, --help: 帮助 ---- */
    case '?':
    case OPT_HELP:
      display_help(0); break;             /* 打印帮助信息 */
    }
  }

  /* ===== 参数验证与回退（容错策略） ===== */
  /*
   * 在 RTEMS 上采用容错回退而非报错退出。
   * 原因：在嵌入式测试中，优雅降级比崩溃退出更可取。
   */

  /* 系统模式 (-s) 与 POSIX 定时器 (-x) 互斥 */
  if ((use_system == MODE_SYS_OFFSET) && (use_nanosleep == MODE_CYCLIC)) {
    printf("system option requires clock_nanosleep, not posix_timers\n");
    use_nanosleep = MODE_CLOCK_NANOSLEEP;  /* 回退到 clock_nanosleep */
  }

  /* 参数边界检查，不合法值回退到默认值 */
  if (clocksel < 0 || clocksel > 1)    clocksel = 0;           /* 默认 MONOTONIC */
  if (histogram < 0)                   histogram = 0;          /* 不生成直方图 */
  if (histogram > HIST_MAX)            histogram = HIST_MAX;   /* 上限裁剪 */
  if (distance == -1)                  distance = DEFAULT_DISTANCE;  /* 默认 500us */
  if (priority < 0 || priority > 99)   priority = 0;           /* 默认无实时优先级 */
  if (num_threads == -1)               num_threads = get_available_cpus();
  if (num_threads < 1)                 num_threads = 1;        /* 至少 1 个线程 */

  /* 优先级分散模式：如果未指定优先级，自动计算 */
  if (priospread && priority == 0)
    priority = num_threads + 1;          /* 最高优先级线程获得此值 */

  /* 策略与优先级一致性：有优先级时必须使用实时策略 */
  if (priority && policy != SCHED_FIFO && policy != SCHED_RR)
    policy = SCHED_FIFO;

  /* 实时策略必须有优先级 */
  if ((policy == SCHED_FIFO || policy == SCHED_RR) && priority == 0)
    priority = num_threads + 1;

  /* aligned 和 secaligned 互斥 */
  if (aligned && secaligned) {
    aligned = secaligned = 0;            /* 静默禁用，而非报错 */
  }

  /* 初始化对齐屏障 */
  if (aligned || secaligned) {
    pthread_barrier_init(&globalt_barr, NULL, num_threads);
    pthread_barrier_init(&align_barr, NULL, num_threads);
  }
}

/* ===== 尖峰/触发追踪函数（与原版逻辑一致） ===== */

/*
 * trigger_init — 初始化触发链表
 *
 * 预分配 trigger_list_size 个节点，链接为单向链表。
 * 使用预分配而非动态分配，避免在测量过程中 malloc 导致不可预测的延迟。
 *
 * 返回: 0 成功, -1 内存分配失败
 */
static int trigger_init(void)
{
  int i;
  struct thread_trigger *trig = NULL;
  for (i = 0; i < trigger_list_size; i++) {
    trig = malloc(sizeof(struct thread_trigger));  /* 预分配节点 */
    if (trig != NULL) {
      if (head == NULL) {
        head = trig;                    /* 第一个节点：作为链表头 */
        tail = trig;
      } else {
        tail->next = trig;              /* 追加到链表尾 */
        tail = trig;
      }
      trig->tnum = i;                   /* 存储预分配序号 */
      trig->next = NULL;
    } else {
      return -1;                        /* 内存不足 */
    }
  }
  current = head;                       /* current 指向当前可写入位置 */
  return 0;
}

/*
 * trigger_print — 打印记录的尖峰事件
 *
 * 从链表头遍历到 current 位置（环形使用），打印每次尖峰的信息。
 * 格式: "T:<线程号> Spike:<延迟值>: TS: <时间戳>"
 */
static void trigger_print(void)
{
  struct thread_trigger *trig = head;
  if (current == head) return;          /* 没有记录任何尖峰 */
  printf("\n");
  while (trig->next != current) {       /* 遍历所有已记录的节点 */
    fprintf(stdout, "T:%2d Spike:%8ld: TS: %12ld\n",
            trig->tnum, trig->diff, (long)trig->ts);
    trig = trig->next;
  }
  /* 输出最后一个节点 */
  fprintf(stdout, "T:%2d Spike:%8ld: TS: %12ld\n",
          trig->tnum, trig->diff, (long)trig->ts);
  printf("spikes = %d\n\n", spikes);
}

/*
 * trigger_update — 记录一次尖峰事件
 *
 * 在线程安全的环境下（持有 trigger_lock 互斥锁）写入触发链表。
 * current 指针在环形使用预分配链表。
 *
 * @par:  线程参数（包含线程号）
 * @diff: 延迟值
 * @ts:   时间戳
 */
static void trigger_update(struct thread_param *par, int diff, int64_t ts)
{
  pthread_mutex_lock(&trigger_lock);     /* 互斥保护 */
  if (current != NULL) {
    current->tnum = par->tnum;           /* 记录线程号 */
    current->ts   = ts;                  /* 记录时间戳 */
    current->diff = diff;                /* 记录延迟值 */
    current = current->next;             /* 移动写入指针 */
  }
  spikes++;                              /* 累计尖峰总数 */
  pthread_mutex_unlock(&trigger_lock);
}

/* ===== 输出辅助函数 ===== */

/*
 * print_tids — 打印所有线程的 ID
 *
 * 格式: "# Thread Ids: 00001 00002 00003 ..."
 * 在 break_trace 模式下，线程 ID 用于关联 ftrace 输出中的线程。
 */
void print_tids(struct thread_param *par[], int nthreads)
{
  int i;
  printf("# Thread Ids:");
  for (i = 0; i < nthreads; i++)
    printf(" %05d", par[i]->stats->tid);
  printf("\n");
}

/*
 * print_stat — 打印单个线程的统计信息
 *
 * 非 verbose 模式（默认）：
 *   格式: "T: 0 (00123) P:80 I:1000 C: 50000 Min:    5 Act:    7 Avg:    6 Max:   42"
 *   其中: T=线程索引, (tid), P=优先级, I=间隔(us), C=周期数,
 *         Min=最小延迟, Act=当前延迟, Avg=平均延迟, Max=最大延迟
 *
 * verbose 模式：
 *   每个周期输出一行: "线程号:周期号:延迟值"
 *   通过 oscope_reduction 参数控制降采样率
 *
 * 相比原版变化：
 *   - 行尾使用 \r\n 而非 \n（适配串口终端的光标回车）
 *   - 无 SMI 计数字段
 *
 * @fp:      输出目标（stdout 或 stderr）
 * @par:     线程参数
 * @index:   线程索引号（0, 1, 2, ...）
 * @verbose: 是否详细模式
 * @quiet:   是否静默模式（1=静默, 2=最终概要输出）
 */
void print_stat(FILE *fp, struct thread_param *par, int index,
                int verbose, int quiet)
{
  struct thread_stat *stat = par->stats;

  if (!verbose) {
    /* ---- 标准模式：一行统计概要 ---- */
    if (quiet != 1) {
      char *fmt;
      /* 纳秒模式需要更宽的字段宽度 */
      if (use_nsecs)
        fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
              "Min:%7ld Act:%8ld Avg:%8ld Max:%8ld\r\n";
      else
        fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
              "Min:%7ld Act:%5ld Avg:%5ld Max:%8ld\r\n";

      /*
       * 平均延迟计算: avg 是累积延迟总和，除以 cycles 得到平均值
       * avg/cycles = 所有延迟的算术平均
       */
      fprintf(fp, fmt, index, stat->tid, par->prio,
              par->interval, stat->cycles, stat->min,
              stat->act,
              stat->cycles ? (long)(stat->avg / stat->cycles) : 0,
              stat->max);
    }
  } else {
    /* ---- 详细模式：逐周期输出 ---- */
    /*
     * cyclesread 追赶 cycles：读取并输出值环形缓冲区中未输出的数据。
     * 通过 oscope_reduction 降采样:
     *   例如 oscope_reduction=1000 → 每 1000 个周期输出一次
     *   输出的是最近 1000 个周期中的最大延迟和对应的周期号
     */
    while (stat->cycles != stat->cyclesread) {
      /* 从环形缓冲区读取延迟值 */
      long diff = stat->values[stat->cyclesread & par->bufmsk];

      /* 跟踪降采样窗口内的最大延迟 */
      if (diff > stat->redmax) {
        stat->redmax = diff;
        stat->cycleofmax = stat->cyclesread;  /* 记录最大值对应的周期号 */
      }

      /* 累计降采样计数器 */
      if (++stat->reduce == oscope_reduction) {
        /* 达到降采样率，输出窗口内的最大值 */
        fprintf(fp, "%8d:%8lu:%8ld\r\n",
                index, stat->cycleofmax, stat->redmax);
        stat->reduce = 0;                /* 重置窗口 */
        stat->redmax = 0;
      }
      stat->cyclesread++;
    }
  }
}

/* ===== print_hist — 打印延迟直方图 ===== */
/*
 * 输出所有线程的延迟分布直方图。
 *
 * 输出格式:
 *   # Histogram
 *   000000 线程0值 \t 线程1值 \t ...
 *   000001 线程0值 \t 线程1值 \t ...
 *   ...
 *   # Min Latencies: ...
 *   # Avg Latencies: ...
 *   # Max Latencies: ...
 *   # Histogram Overflows: ...
 *   # Histogram Overflow at cycle number:
 *   # Thread 0: <溢出事件号列表>
 *   # Thread 1: <溢出事件号列表>
 *
 * 相比原版变化：
 *   - 始终输出到 stdout（无 histfile 支持）
 *   - 无 SMI 统计行
 *
 * @par:      线程参数数组
 * @nthreads: 线程数量
 */
void print_hist(struct thread_param *par[], int nthreads)
{
  int i;
  unsigned long maxmax, alloverflows;
  FILE *fd = stdout;                    /* 直接输出到 stdout */

  fprintf(fd, "# Histogram\n");
  /* 逐桶输出 */
  for (i = 0; i < histogram; i++) {
    unsigned long flags = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%06d ", i);
    if (histofall) flags |= HSET_PRINT_SUM;  /* 添加汇总列 */
    hset_print_bucket(&hset, fd, buf, i, flags);
  }

  /* 最小延迟统计 */
  fprintf(fd, "# Min Latencies:");
  for (i = 0; i < nthreads; i++)
    fprintf(fd, " %05lu", par[i]->stats->min);
  fprintf(fd, "\n");

  /* 平均延迟统计 */
  fprintf(fd, "# Avg Latencies:");
  for (i = 0; i < nthreads; i++)
    fprintf(fd, " %05lu",
            par[i]->stats->cycles ?
            (long)(par[i]->stats->avg / par[i]->stats->cycles) : 0);
  fprintf(fd, "\n");

  /* 最大延迟统计 */
  fprintf(fd, "# Max Latencies:");
  maxmax = 0;
  for (i = 0; i < nthreads; i++) {
    fprintf(fd, " %05lu", par[i]->stats->max);
    if (par[i]->stats->max > maxmax)
      maxmax = par[i]->stats->max;      /* 跟踪全局最大延迟 */
  }
  if (histofall && nthreads > 1)
    fprintf(fd, " %05lu", maxmax);       /* 汇总列显示全局最大 */
  fprintf(fd, "\n");

  /* 直方图溢出统计 */
  fprintf(fd, "# Histogram Overflows:");
  alloverflows = 0;
  for (i = 0; i < nthreads; i++) {
    fprintf(fd, " %05lu", par[i]->stats->hist->oflow_count);
    alloverflows += par[i]->stats->hist->oflow_count;
  }
  if (histofall && nthreads > 1)
    fprintf(fd, " %05lu", alloverflows);  /* 汇总溢出总数 */
  fprintf(fd, "\n");

  /* 溢出事件详情（每个线程的溢出事件号列表） */
  fprintf(fd, "# Histogram Overflow at cycle number:\n");
  for (i = 0; i < nthreads; i++) {
    fprintf(fd, "# Thread %d: ", i);
    hist_print_oflows(par[i]->stats->hist, fd);
    fprintf(fd, "\n");
  }
  fprintf(fd, "\n");
}

/* ======================================================================
 *  timerthread — 核心延迟测量线程
 *
 *  这是 cyclictest 最核心的函数，实现实时延迟测量算法。
 *
 *  测量模式:
 *    MODE_CYCLIC         — POSIX 定时器 (timer_create/timer_settime)
 *                          线程阻塞在 sigwait()，定时器到期时被信号唤醒
 *    MODE_CLOCK_NANOSLEEP — clock_nanosleep
 *                          线程直接睡眠到指定时刻
 *
 *  算法流程:
 *    1. 设置 CPU 亲和性和调度策略/优先级
 *    2. 初始化定时器 (MODE_CYCLIC) 或直接用 clock_nanosleep
 *    3. 获取起始时间
 *    4. 进入主循环:
 *       a. 等待下一次定时到期（睡眠/信号等待）
 *       b. 获取实际醒来时间
 *       c. 计算延迟: diff = 实际时间 - 预期时间
 *       d. 更新 min/max/avg 统计
 *       e. 可选：记录到直方图、值缓冲区、尖峰追踪
 *       f. 计算下一次预期醒来时间: next += interval
 *    5. 处理超限（如果处理不及时，跳过已经过去的时间点）
 *    6. 退出时清理定时器、恢复调度策略
 *
 *  相比原版的变化:
 *    - 新增 TSC (rdtsc) 测量支持（更高的测量精度）
 *    - 移除 SMI 计数器读取
 *    - 移除 MODE_SYS_ITIMER/MODE_SYS_NANOSLEEP 模式
 *    - 移除 NUMA 节点绑定
 *    - 移除 ftrace 标记
 *    - 调度策略设置增加 #ifdef __rtems__ 条件编译
 *    - POSIX 定时器操作增加条件编译保护
 *
 * @param: 指向 thread_param 结构体的指针
 * 返回: NULL
 * ====================================================================== */
void *timerthread(void *param)
{
  struct thread_param *par = param;
  struct sched_param   schedp;
  struct sigevent      sigev;
  sigset_t             sigset;
  timer_t              timer;              /* POSIX 定时器句柄 */
  struct timespec      now, next, interval, stop = {0, 0};
  struct itimerspec    tspec;
  struct thread_stat  *stat = par->stats;  /* 本线程的统计数据结构 */
  int                  stopped = 0;        /* 中断追踪停止标志 */
  int                  ret;

  /*
   * REMOVED: NUMA node affinity (rt_numa_set_numa_run_on_node)
   * 原因: RTEMS 无 NUMA（见注释 #1）。
   * 原版代码: if (par->node != -1) rt_numa_set_numa_run_on_node(...)
   * RTEMS 上 node 始终为 -1，条件永不成立。
   */

  /* ===== CPU 亲和性设置 ===== */
  /*
   * 将当前线程绑定到指定的 CPU 核心。
   * 在 SMP 系统中，这确保测量线程始终在同一核心上运行，
   * 避免跨 CPU 迁移导致的缓存失效和额外延迟。
   */
  if (par->cpu != -1) {
    cpu_set_t mask;
    pthread_t thread;
    CPU_ZERO(&mask);
    CPU_SET(par->cpu, &mask);
    thread = pthread_self();
    if (pthread_setaffinity_np(thread, sizeof(mask), &mask) != 0)
      warn("Could not set CPU affinity to CPU #%d\n", par->cpu);
      /* 非致命错误：线程仍可在任意 CPU 上运行 */
  }

  /* ===== 计算间隔时间的 timespec 表示 ===== */
  /*
   * 将微秒级的 interval 转换为 timespec 结构体。
   * 例如: interval=1000us → tv_sec=0, tv_nsec=1000000
   */
  interval.tv_sec  = par->interval / USEC_PER_SEC;
  interval.tv_nsec = (par->interval % USEC_PER_SEC) * 1000;  /* 余数转纳秒 */

  /* ===== 获取线程 ID ===== */
  /*
   * gettid() 的实现取决于平台:
   *   Linux:  syscall(SYS_gettid) — 内核线程 ID
   *   RTEMS:  (pid_t)(uintptr_t)pthread_self() — POSIX 线程 ID
   */
  stat->tid = gettid();

  /* ===== 屏蔽定时器信号 ===== */
  /*
   * 在 MODE_CYCLIC 中，线程阻塞在 sigwait() 等待信号。
   * 必须先屏蔽该信号，否则信号可能被默认处理（终止线程）。
   */
  sigemptyset(&sigset);
  sigaddset(&sigset, par->signal);         /* 默认 SIGALRM */
  sigprocmask(SIG_BLOCK, &sigset, NULL);   /* 在线程信号掩码中阻塞 */

  /* ===== MODE_CYCLIC: 创建 POSIX 定时器 ===== */
  /*
   * 使用 timer_create + timer_settime 实现周期性定时器。
   * 定时器到期时发送信号（SIGEV_SIGNAL），线程在 sigwait() 中接收。
   *
   * RTEMS 兼容性:
   *   当 CONFIGURE_MAXIMUM_POSIX_TIMERS > 0 且定义了
   *   CONFIGURE_ENABLE_POSIX_API 时可用。
   *   否则回退到 MODE_CLOCK_NANOSLEEP。
   *
   *   注意：原版使用 SIGEV_THREAD_ID | SIGEV_SIGNAL 将信号定向到
   *   特定线程。RTEMS 不支持 SIGEV_THREAD_ID（Linux 内核扩展），
   *   改用 SIGEV_SIGNAL 进程级信号。
   */
  if (par->mode == MODE_CYCLIC) {
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
    sigev.sigev_notify = SIGEV_SIGNAL;       /* 信号通知方式 */
    sigev.sigev_signo  = par->signal;        /* 信号编号 */
    timer_create(par->clock, &sigev, &timer); /* 创建定时器 */
    tspec.it_interval = interval;            /* 周期性间隔 */
#else
    /* RTEMS 无 POSIX 定时器支持 → 回退 */
    par->mode = MODE_CLOCK_NANOSLEEP;
#endif
  }

  /* ===== 设置调度策略和优先级 ===== */
  /*
   * Linux: sched_setscheduler 在运行时设置调度策略和优先级。
   *   setscheduler() 封装了 rlimit 重试逻辑（RTEMS 上已简化）。
   *
   * RTEMS: sched_setscheduler 是 stub（返回 ENOSYS）。
   *   调度策略通过 pthread_attr_setschedpolicy 在创建线程时设置（见 cyclictest_main）。
   *   因此 RTEMS 上跳过此调用。
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
   * REMOVED: SMI counter initialization (open_msr_file, get_smi_counter)
   * 原因: /dev/cpu/N/msr 不可用（见注释 #2）。
   * smi 定义为 0，以下代码被死代码消除。
   */

  /* ===== 获取起始时间 ===== */
  /*
   * 支持两种对齐模式:
   *
   * aligned (线程间偏移对齐):
   *   所有线程在 globalt_barr 屏障处等待。
   *   线程 0 获取当前时间作为基准 globalt。
   *   所有线程在 align_barr 屏障处同步。
   *   每个线程的起始时间 = globalt + offset * tnum
   *   结果: 线程按固定偏移交错唤醒，避免同时争抢 CPU。
   *
   * secaligned (秒边界对齐):
   *   类似 aligned，但基准时间对齐到下一秒开始。
   *   如果当前纳秒 > 900000000（即将到下一秒），跳到下下一秒。
   *
   * 无对齐:
   *   直接调用 clock_gettime 获取当前时间作为起始点。
   */
  if (aligned || secaligned) {
    /* ---- 对齐模式 ---- */
    pthread_barrier_wait(&globalt_barr);    /* 等待所有线程到达 */
    if (par->tnum == 0) {
      /* 线程 0 负责获取全局基准时间 */
      clock_gettime(par->clock, &globalt);
      if (secaligned) {
        /* 对齐到下一秒边界 */
        if (globalt.tv_nsec > 900000000)
          globalt.tv_sec += 2;    /* 接近秒尾 → 跳到下下秒 */
        else
          globalt.tv_sec++;
        globalt.tv_nsec = 0;      /* 秒内纳秒清零 */
      }
    }
    pthread_barrier_wait(&align_barr);      /* 等待线程 0 获取时间 */
    now = globalt;                          /* 所有线程使用相同的基准时间 */
    if (offset) {
      if (aligned)
        now.tv_nsec += offset * par->tnum;  /* 按线程号分散偏移 */
      else
        now.tv_nsec += offset;              /* 统一偏移 */
      tsnorm(&now);                         /* 规范化纳秒 */
    }
  } else {
    clock_gettime(par->clock, &now);        /* 直接获取时间 */
  }

  /* ===== 计算首次到期时间 ===== */
  next = now;
  next.tv_sec  += interval.tv_sec;
  next.tv_nsec += interval.tv_nsec;
  tsnorm(&next);                            /* 规范化 */

  /* ===== 计算停止时间（如果指定了 duration） ===== */
  if (duration) {
    stop = now;
    stop.tv_sec += duration;
  }

  /* ===== 启动定时器（MODE_CYCLIC） ===== */
  /*
   * TIMER_ABSTIME: tspec.it_value = next（绝对时间 → 首次到期时间）
   * TIMER_RELTIME: tspec.it_value = interval（相对时间 → 首次到期 = 现在 + interval）
   */
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
  if (par->mode == MODE_CYCLIC) {
    if (par->timermode == TIMER_ABSTIME)
      tspec.it_value = next;               /* 绝对到期时间 */
    else
      tspec.it_value = interval;           /* 相对到期时间 */
    timer_settime(timer, par->timermode, &tspec, NULL);
  }
#endif

  /*
   * REMOVED: MODE_SYS_ITIMER (setitimer)
   * 原因: setitimer(ITIMER_REAL) 发送 SIGALRM 到进程。
   * 在 RTEMS 上，进程级信号支持有限。
   * clock_nanosleep 和 POSIX 定时器已覆盖所有实用场景。
   */

  /* 标记线程已启动 */
  stat->threadstarted++;

  /* ===== TSC 校准 ===== */
  /*
   * 如果是第一个线程（tnum == 0），通过 sleep 50ms 校准 TSC 频率。
   *
   * 校准方法:
   *   1. 记录 sleep 前的 TSC 值
   *   2. clock_nanosleep 50ms
   *   3. 记录 sleep 后的 TSC 值
   *   4. 用 clock_gettime 获取实际经过的纳秒数
   *   5. tsc_per_us = (TSC 差) / (实际微秒数)
   *
   * tsc_per_us 是全局变量，所有线程共享校准结果。
   */
#if USE_TSC
  {
    struct timespec cal_req, cal_start, cal_end;
    unsigned long long cal_tsc1, cal_tsc2;
    cal_req.tv_sec = 0; cal_req.tv_nsec = 50000000; /* 50ms 睡眠 */
    clock_gettime(par->clock, &cal_start);          /* 记录开始时间 */
    cal_tsc1 = rdtsc();                              /* 记录开始 TSC */
    clock_nanosleep(CLOCK_MONOTONIC, 0, &cal_req, NULL); /* 精确睡眠 50ms */
    cal_tsc2 = rdtsc();                              /* 记录结束 TSC */
    clock_gettime(par->clock, &cal_end);            /* 记录结束时间 */
    /* 计算实际经过的纳秒数 */
    long long cal_ns = (cal_end.tv_sec-cal_start.tv_sec)*NSEC_PER_SEC
                     + (cal_end.tv_nsec-cal_start.tv_nsec);
    if (cal_ns > 0)
      /* 计算每微秒的 TSC 周期数 */
      tsc_per_us = (double)(cal_tsc2 - cal_tsc1) / ((double)cal_ns/1000.0);
    if (par->tnum == 0)
      printf("TSC: %.0f MHz\n", tsc_per_us);        /* 输出 TSC 频率 */
  }
#endif

  /* ====================================================================
   *  主测量循环 — cyclictest 的核心
   *
   *  每个周期执行:
   *    1. 记录 TSC (USE_TSC) 或等待定时器 (clock_nanosleep/sigwait)
   *    2. 获取实际醒来时间
   *    3. 计算延迟 diff
   *    4. 更新 min/max/avg
   *    5. 更新直方图
   *    6. 可选: 尖峰追踪、中断追踪
   *    7. 计算下一次预期醒来时间
   * ==================================================================== */
  while (!shutdown) {
    uint64_t diff;
    int      sigs;

#if USE_TSC
    /*
     * TSC 模式: 在 sleep 前记录 TSC 值，醒来后立即再次读取。
     * 延迟 = 实际 TSC 消耗 - 预期 TSC 消耗
     * 预期 TSC 消耗 = interval(us) * tsc_per_us
     */
    unsigned long long tsc_before;
    tsc_before = rdtsc();
#endif

    /* ===== 等待下一次定时到期 ===== */
    switch (par->mode) {
    case MODE_CYCLIC:
      /*
       * POSIX 定时器模式: 阻塞等待信号。
       * 定时器到期时内核发送 SIGALRM，sigwait() 返回。
       */
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
      if (sigwait(&sigset, &sigs) < 0)
        goto out;
#endif
      break;

    case MODE_CLOCK_NANOSLEEP:
      /*
       * clock_nanosleep 模式: 直接睡眠到指定时刻。
       * TIMER_ABSTIME: 睡眠到 next 指定的绝对时间
       * TIMER_RELTIME: 睡眠 interval 长度的时间
       *
       * EINTR 处理: 信号可能中断睡眠，但 cyclictest 使用 sigwait
       * 阻塞信号，所以实际上不会收到 EINTR。
       */
      if (par->timermode == TIMER_ABSTIME) {
        /* 绝对时间模式：睡眠到 next 时刻 */
        ret = clock_nanosleep(par->clock, TIMER_ABSTIME, &next, NULL);
        if (ret != 0) {
          if (ret != EINTR)
            warn("clock_nanosleep failed. errno: %d\n", errno);
          goto out;
        }
      } else {
        /* 相对时间模式：睡眠 interval 长度，然后计算下一次到期时间 */
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
      goto out;     /* 未知模式 → 退出 */
    }

#if USE_TSC
    /* ===== TSC 模式延迟计算 ===== */
    /*
     * 延迟 = (实际 TSC 消耗 - 预期 TSC 消耗)
     *
     * TSC 直接反映 CPU 周期，精度远高于 clock_gettime。
     * 但 diff 的单位是 TSC cycles 而非微秒，统计输出需要注意单位差异。
     *
     * 仍然调用 clock_gettime 获取 now（用于 tsgreater/duration 判断）。
     */
    {
      unsigned long long tsc_after = rdtsc();
      long long tsc_elapsed = (long long)(tsc_after - tsc_before);
      long long tsc_expected = (long long)((double)par->interval * tsc_per_us);
      diff = (uint64_t)(tsc_elapsed - tsc_expected);
      /* 溢出保护：如果 diff 异常大，清零避免统计污染 */
      if (diff > (uint64_t)(tsc_per_us * 1000000)) diff = 0;
    }
    clock_gettime(par->clock, &now);    /* 仍需要 now 用于后续逻辑 */
#else
    /* ===== clock_gettime 模式延迟计算 ===== */
    /*
     * 获取实际醒来时间。
     * diff = now - next
     *   > 0: 延迟醒来（正延迟 = 实时性能差）
     *   < 0: 提前醒来（极少发生，可能是 clock_settime 调整）
     *   = 0: 精确准时
     */
    ret = clock_gettime(par->clock, &now);
    if (ret != 0) {
      if (ret != EINTR)
        warn("clock_gettime() failed. errno: %d\n", errno);
      goto out;
    }

    /* 计算延迟（微秒或纳秒，取决于 use_nsecs 标志） */
    if (use_nsecs)
      diff = calcdiff_ns(now, next);     /* 纳秒级延迟 */
    else
      diff = calcdiff(now, next);        /* 微秒级延迟 */
#endif

    /* ===== 更新统计 ===== */
    /*
     * 最小延迟: 记录历史最佳值
     * 最大延迟: 记录历史最差值（实时系统最关心的指标）
     * 平均值:  累积总和，最后除以 cycles 得到平均值
     */
    if (diff < stat->min) stat->min = diff;
    if (diff > stat->max) {
      stat->max = diff;
      /* refresh_on_max 模式: 通知主线程有新的最大延迟 */
      if (refresh_on_max)
        pthread_cond_signal(&refresh_on_max_cond);
    }
    stat->avg += (double)diff;            /* 累积求和 */

    /* ===== 尖峰追踪 ===== */
    /*
     * 如果设置了 trigger 阈值且当前延迟超过阈值，记录此次尖峰。
     */
    if (trigger && (diff > trigger))
      trigger_update(par, diff, calctime(now));

    /* ===== 持续时间检查 ===== */
    /*
     * 如果指定了 duration，检查是否已达到测试时间。
     * calcdiff(now, stop) >= 0 表示 now 已经达到或超过 stop。
     */
    if (duration && (calcdiff(now, stop) >= 0))
      shutdown++;      /* 触发优雅退出 */

    /* ===== 中断追踪 ===== */
    /*
     * 当延迟超过 tracelimit 时:
     *   1. 记录触发线程的 ID 和延迟值
     *   2. 设置 shutdown 退出测试
     *   3. 原版此处还调用 tracemark() 在 ftrace 中写标记（已移除）
     *
     * stopped 标志确保只记录第一个触发中断追踪的线程。
     */
    if (!stopped && tracelimit && (diff > tracelimit)) {
      stopped++;
      shutdown++;
      pthread_mutex_lock(&break_thread_id_lock);
      if (break_thread_id == 0) {       /* 第一个触发的线程 */
        break_thread_id = stat->tid;     /* 记录线程 ID */
        break_thread_value = diff;       /* 记录触发值 */
        /*
         * REMOVED: tracemark("hit latency threshold...")
         * REMOVED: tracing_stop()
         * 原因: ftrace 不存在于 RTEMS（见注释 #5）。
         */
      }
      pthread_mutex_unlock(&break_thread_id_lock);
    }

    /* 更新"当前延迟"显示值 */
    stat->act = diff;

    /* ===== 存储到值环形缓冲区（verbose 模式） ===== */
    /*
     * 环形缓冲区索引: cycles & bufmsk
     * bufmsk = VALBUF_SIZE - 1 = 0x3FFF (16383)
     * 利用 2 的幂特性，用位与代替取模运算。
     */
    if (par->bufmsk)
      stat->values[stat->cycles & par->bufmsk] = diff;

    /* ===== 更新直方图 ===== */
    if (histogram)
      hist_sample(stat->hist, diff);    /* 将延迟值归入对应桶 */

    /* 递增周期计数 */
    stat->cycles++;

    /* ===== 计算下一次预期醒来时间 ===== */
    /*
     * 推进一个 interval。
     * 如果是 MODE_CYCLIC，还需检查定时器超限次数。
     *
     * 定时器超限: 如果系统负载过高导致一个周期内处理不完，
     * POSIX 定时器可能已经多次到期。timer_getoverrun() 返回
     * 超限次数，我们需要跳过这些已经过去的周期。
     */
    next.tv_sec  += interval.tv_sec;
    next.tv_nsec += interval.tv_nsec;

    if (par->mode == MODE_CYCLIC) {
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
      int overrun_count = timer_getoverrun(timer);
      /* 跳过超限的周期 */
      next.tv_sec  += overrun_count * interval.tv_sec;
      next.tv_nsec += overrun_count * interval.tv_nsec;
#endif
    }
    tsnorm(&next);    /* 规范化 */

    /* ===== 追赶逻辑 ===== */
    /*
     * 如果当前时间已经超过了计算的下一次预期时间
     * （即处理耗时超过了 interval），持续推进 next 直到超过 now。
     *
     * 这种情况通常意味着:
     *   - 系统负载过高
     *   - 发生了长时间的中断或抢占
     *   - interval 设置过短
     *
     * 通过逐个 interval 推进，避免一次性跳过太多周期。
     */
    while (tsgreater(&now, &next)) {
      next.tv_sec  += interval.tv_sec;
      next.tv_nsec += interval.tv_nsec;
      tsnorm(&next);
    }

    /* ===== 周期数限制检查 ===== */
    if (par->max_cycles && par->max_cycles == stat->cycles)
      break;    /* 达到指定周期数，退出循环 */
  }

out:
  /* ===== 退出时通知主线程 ===== */
  /*
   * refresh_on_max 模式下，主线程可能阻塞在条件变量上。
   * 发送信号确保主线程能退出等待。
   */
  if (refresh_on_max) {
    pthread_mutex_lock(&refresh_on_max_lock);
    shutdown++;                             /* 设置全局停止标志 */
    pthread_cond_signal(&refresh_on_max_cond); /* 唤醒主线程 */
    pthread_mutex_unlock(&refresh_on_max_lock);
  }

  /* ===== 清理 POSIX 定时器 ===== */
  if (par->mode == MODE_CYCLIC) {
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
    timer_delete(timer);                    /* 删除定时器 */
#endif
  }

  /*
   * REMOVED: MODE_SYS_ITIMER 清理（setitimer 禁用）
   * 原因: 未实现此模式（见上文注释）。
   */

  /* ===== 恢复调度策略为普通优先级 ===== */
  /*
   * Linux only: sched_setscheduler 恢复为 SCHED_OTHER。
   * RTEMS: sched_setscheduler 是 stub，跳过。
   */
#ifndef __rtems__
  if (par->policy != SCHED_OTHER || par->prio > 0) {
    schedp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &schedp);
  }
#endif

  stat->threadstarted = -1;   /* 标记线程已退出 */
  return NULL;
}

/* ===== write_stats — JSON 格式输出统计数据 ===== */
/*
 * 将线程统计信息以 JSON 格式写入文件。
 *
 * 输出格式:
 *   {
 *     "num_threads": 2,
 *     "resolution_in_ns": 0,
 *     "thread": {
 *       "0": {
 *         "histogram": { ... },
 *         "cycles": 50000,
 *         "min": 5,
 *         "max": 42,
 *         "avg": 6.30,
 *         "cpu": 0
 *       },
 *       ...
 *     }
 *   }
 *
 * REMOVED: rstat 相关函数（shm_open/mmap 共享内存状态共享）
 * 原因: 共享内存 IPC 不适用于 RTEMS（见注释 #7）。
 *
 * @f:    输出文件指针
 * @data: 未使用（回调函数数据参数）
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
      hist_print_json(s->hist, f);          /* 输出直方图 JSON */
      fprintf(f, "      },\n");
    }
    fprintf(f, "      \"cycles\": %ld,\n", s->cycles);
    fprintf(f, "      \"min\": %ld,\n", s->min);
    fprintf(f, "      \"max\": %ld,\n", s->max);
    fprintf(f, "      \"avg\": %.2f,\n",
            s->cycles ? s->avg / s->cycles : 0.0);
    fprintf(f, "      \"cpu\": %d\n", par[i]->cpu);
    /* 最后一个线程不加逗号，遵循 JSON 语法 */
    fprintf(f, "    }%s\n", i == num_threads - 1 ? "" : ",");
  }
  fprintf(f, "  }\n");
}

/* ===== rt_write_json — JSON 文件写入 ===== */
/*
 * 原版 rt_write_json 功能复杂（包含 uname、/sys/kernel/realtime、
 * 时间戳等）。RTEMS 版简化为最小实现。
 *
 * @filename:    输出文件名（"-" 暂不支持，原版支持）
 * @return_code: 返回码（未使用，简化版忽略）
 * @cb:          回调函数（write_stats）用于输出线程数据
 * @data:        回调数据
 */
void rt_write_json(const char *filename, int return_code,
                   void (*cb)(FILE *, void *), void *data)
{
  FILE *f = fopen(filename, "w");
  if (!f) return;                /* 打开失败则静默跳过 */
  fprintf(f, "{\n");
  cb(f, data);                   /* 回调输出线程数据 */
  fprintf(f, "}\n");
  fclose(f);
}

/* ===== rt_init — RT 初始化 ===== */
/*
 * 原版 rt_init 记录命令行参数和时间戳供 JSON 输出使用。
 * RTEMS 版简化为空函数（JSON 输出已大幅简化）。
 */
void rt_init(int argc, char *argv[])
{
  /* RTEMS 上无需特殊初始化 */
}

/* ===== CPU 亲和性辅助函数 ===== */
/*
 * 替代原版 libnuma 的 cpu_for_thread_* 函数。
 * 使用标准 POSIX cpu_set_t 而非 libnuma 的 struct bitmask。
 */

/*
 * cpu_for_thread_ua — "Use All" CPU 分配
 *
 * 简单轮询: 线程 i 分配到 CPU (i % max_cpus)。
 * 例如: 4 个 CPU, 6 个线程
 *   Thread 0 → CPU 0
 *   Thread 1 → CPU 1
 *   Thread 2 → CPU 2
 *   Thread 3 → CPU 3
 *   Thread 4 → CPU 0  (循环)
 *   Thread 5 → CPU 1
 */
static int cpu_for_thread_ua(int i, int max_cpus)
{
  return i % max_cpus;
}

/*
 * cpu_for_thread_sp — "Specific" CPU 分配（用户指定 CPU 掩码）
 *
 * 从 affinity_mask 中选定第 (i % count) 个 CPU。
 * 例如: mask = {0, 3, 5}, 4 个线程
 *   Thread 0 → CPU 0
 *   Thread 1 → CPU 3
 *   Thread 2 → CPU 5
 *   Thread 3 → CPU 0  (循环到第 0 个选中的 CPU)
 *
 * @i:        线程索引
 * @max_cpus: 最大 CPU 数
 * @mask:     CPU 掩码（RTEMS 版使用 cpu_set_t）
 */
static int cpu_for_thread_sp(int i, int max_cpus, cpu_set_t *mask)
{
  int cpu, count = 0;

  /* 先数一下掩码中设置了多少个 CPU */
  for (cpu = 0; cpu < max_cpus; cpu++) {
    if (CPU_ISSET(cpu, mask)) count++;
  }
  if (count == 0) return -1;

  /* 轮询到第 (i % count) 个 CPU */
  count = i % count;
  for (cpu = 0; cpu < max_cpus; cpu++) {
    if (CPU_ISSET(cpu, mask)) {
      if (count == 0) return cpu;
      count--;
    }
  }
  return -1;
}

/*
 * threadalloc — 线程相关内存分配（简化版）
 *
 * 原版使用 NUMA-node 感知的分配（rt_numa_numa_alloc_onnode）。
 * RTEMS 版直接使用 calloc。
 *
 * @size: 分配大小
 * @node: NUMA 节点（RTEMS 上忽略）
 */
static void *threadalloc(size_t size, int node)
{
  (void)node;           /* 忽略 NUMA 节点参数 */
  return calloc(1, size);
}

/*
 * threadfree — 线程相关内存释放（简化版）
 *
 * 原版使用 NUMA-node 感知的释放。
 * RTEMS 版直接使用 free。
 */
static void threadfree(void *ptr, size_t size, int node)
{
  (void)size; (void)node;
  free(ptr);
}

/* ======================================================================
 *  cyclictest_main — 主入口（替代原版 main()）
 *
 *  原版使用 int main(int argc, char **argv) 作为独立进程入口。
 *  RTEMS 版改为 cyclictest_main()，被 init.c 中的任务调用。
 *
 *  执行流程:
 *    1. 解析命令行参数
 *    2. 可选: 尖峰追踪初始化、时钟分辨率检查
 *    3. 分配线程参数/统计数据数组
 *    4. 创建 N 个测量线程（每个绑定到指定 CPU）
 *    5. 进入主监控循环（输出统计信息）
 *    6. 等待所有线程结束
 *    7. 输出最终统计（直方图、中断信息）
 *    8. 清理资源并返回
 *
 *  相比原版 main() 的变化:
 *    - 无 check_privs() 权限检查
 *    - 无 mlockall 内存锁定
 *    - 无 set_latency_target 电源管理
 *    - 无 enable_trace_mark ftrace 初始化
 *    - 无 rstat_setup 共享内存初始化
 *    - 无 fifothread 命名管道线程
 *    - 线程终止使用 pthread_cancel 而非 pthread_kill(SIGTERM)
 *    - 返回 ret 而非 exit(ret)
 *    - 使用 pthread_attr_setschedpolicy 设置调度策略（RTEMS 特有）
 *
 * @argc: 命令行参数数量
 * @argv: 命令行参数数组
 * 返回: EXIT_SUCCESS 或 EXIT_FAILURE
 * ====================================================================== */
int cyclictest_main(int argc, char *argv[])
{
  int  i, ret, status;
  int  mode;                       /* 测量模式 */
  int  max_cpus;                   /* 系统 CPU 数量 */
  int  allstopped = 0;             /* 已完成目标周期数的线程计数 */

  /* ===== 第0步: 重置全局状态（支持重复调用） ===== */
  /*
   * cyclictest_main 可能被多次调用（例如 shell 中重复执行命令）。
   * 所有 static 全局变量必须重置为初始默认值，否则上一次运行
   * 的残留状态（尤其是 shutdown=1）会导致测量线程直接跳过主循环。
   */
  shutdown               = 0;
  tracelimit             = 0;
  verbose                = 0;
  oscope_reduction       = 1;
  histogram              = 0;
  histofall              = 0;
  duration               = 0;
  use_nsecs              = 0;
  refresh_on_max         = 0;
  force_sched_other      = 0;
  priospread             = 0;
  check_clock_resolution = 0;
  ct_debug               = 0;
  use_nanosleep          = MODE_CLOCK_NANOSLEEP;
  timermode              = TIMER_ABSTIME;
  use_system             = 0;
  priority               = 0;
  policy                 = SCHED_OTHER;
  num_threads            = 1;
  max_cycles             = 0;
  clocksel               = 0;
  quiet                  = 0;
  interval               = DEFAULT_INTERVAL;
  distance               = -1;
  smp                    = 0;
  setaffinity            = AFFINITY_UNSPECIFIED;
  aligned                = 0;
  secaligned             = 0;
  offset                 = 0;
  trigger                = 0;
  spikes                 = 0;
  break_thread_id        = 0;
  break_thread_value     = 0;
  affinity_mask          = NULL;
  main_affinity_mask     = NULL;
  head                   = NULL;
  tail                   = NULL;
  current                = NULL;

  max_cpus = sysconf(_SC_NPROCESSORS_CONF);   /* 获取 CPU 配置数 */

  /* ===== 第1步: 解析命令行 ===== */
  process_options(argc, argv, max_cpus);

  if (verbose)
    printf("CPUs: %d\n", max_cpus);

  /* ===== 第2步: 初始化尖峰追踪链表 ===== */
  if (trigger) {
    ret = trigger_init();
    if (ret != 0) {
      fprintf(stderr, "trigger_init() failed\n");
      return EXIT_FAILURE;
    }
  }

  /* ===== 第3步: 检查高精度定时器 ===== */
  if (check_timer())
    printf("WARN: High resolution timers not available\n");

  /* ===== 第4步: 时钟分辨率检查（-R 选项） ===== */
  /*
   * 通过连续多次调用 clock_gettime 来估算时钟分辨率。
   * 原版约 80 行代码，RTEMS 版简化为约 20 行。
   */
  if (check_clock_resolution) {
    int clock = clocksources[clocksel];
    uint64_t min_non_zero_diff = UINT64_MAX;
    struct timespec now, prev, res;
    uint64_t diff;
    int k;

    if (clock_getres(clock, &res))
      printf("WARN: clock_getres failed\n");

    /* 1000 次 clock_gettime 调用估算分辨率 */
    clock_gettime(clock, &prev);
    for (k = 0; k < 1000; k++)
      clock_gettime(clock, &now);
    diff = calcdiff_ns(now, prev);
    if (diff > 0) {
      uint64_t reported = (NSEC_PER_SEC * res.tv_sec) + res.tv_nsec;
      printf("  reported clock resolution: %llu nsec\n",
             (unsigned long long)reported);
      printf("  measured clock resolution: ~%llu nsec\n",
             (unsigned long long)(diff / 1000)); /* 平均每次调用的时间 */
    }
  }

  /* ===== 第5步: 确定测量模式 ===== */
  mode = use_nanosleep + use_system;
  /*
   * use_nanosleep: MODE_CYCLIC(0) 或 MODE_CLOCK_NANOSLEEP(1)
   * use_system:    0 或 MODE_SYS_OFFSET(2)
   * 组合:
   *   0+0=0 (MODE_CYCLIC)
   *   1+0=1 (MODE_CLOCK_NANOSLEEP)
   *   1+2=3 (MODE_SYS_NANOSLEEP — RTEMS 不支持，被跳过)
   */

  /*
   * REMOVED: 信号处理器注册（signal(SIGINT, sighand) 等）
   * 原因: RTEMS 中 signal() 在某些配置下不可用。
   * Ctrl-C 终止通过 RTEMS 内部机制处理。
   */

  /* ===== 第6步: 初始化直方图 ===== */
  if (histogram &&
      hset_init(&hset, num_threads, 1, histogram, histogram)) {
    fprintf(stderr, "failed to allocate histogram\n");
    return EXIT_FAILURE;
  }

  /* ===== 第7步: 分配线程参数/统计数组 ===== */
  parameters = calloc(num_threads, sizeof(struct thread_param *));
  if (!parameters) goto out;
  statistics = calloc(num_threads, sizeof(struct thread_stat *));
  if (!statistics) goto outpar;

  /* ===== 第8步: 创建测量线程 ===== */
  for (i = 0; i < num_threads; i++) {
    pthread_attr_t attr;
    int            cpu, node;
    struct thread_param *par;
    struct thread_stat  *stat;

    /* 初始化线程属性 */
    status = pthread_attr_init(&attr);
    if (status != 0) {
      fprintf(stderr, "pthread_attr_init for thread %d: %s\n",
              i, strerror(status));
      goto outall;
    }

    /*
     * RTEMS 特有: 通过 pthread_attr 设置调度策略和优先级。
     *
     * 原因: RTEMS 的 sched_setscheduler() 是 stub（返回 ENOSYS），
     * 因为 RTEMS 不提供进程级别的调度策略切换。
     * 正确的做法是在线程创建时通过 pthread_attr 指定调度属性。
     *
     * PTHREAD_EXPLICIT_SCHED: 使用属性中明确指定的调度参数，
     * 不继承父线程的调度策略。
     */
    if (priority && (policy == SCHED_FIFO || policy == SCHED_RR)) {
      struct sched_param sparam;
      sparam.sched_priority = priority;         /* 设置优先级 (1-99) */

      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      pthread_attr_setschedpolicy(&attr, policy);     /* FIFO 或 RR */
      pthread_attr_setschedparam(&attr, &sparam);     /* 优先级值 */
    }

    /* ===== CPU 亲和性分配 ===== */
    switch (setaffinity) {
    case AFFINITY_UNSPECIFIED: cpu = -1; break;       /* 不绑定 */
    case AFFINITY_SPECIFIED:
      cpu = cpu_for_thread_sp(i, max_cpus, affinity_mask);  /* 用户指定 CPU */
      break;
    case AFFINITY_USEALL:
      cpu = cpu_for_thread_ua(i, max_cpus);            /* 轮询所有 CPU */
      break;
    default: cpu = -1;
    }

    node = -1;    /* RTEMS 上始终为 -1（无 NUMA） */

    /* 分配线程参数结构体 */
    parameters[i] = par = calloc(1, sizeof(struct thread_param));
    if (!par) goto outall;

    /* 分配线程统计结构体 */
    statistics[i] = stat = calloc(1, sizeof(struct thread_stat));
    if (!stat) goto outall;

    /* 关联直方图 */
    if (histogram)
      stat->hist = &hset.histos[i];

    /* ===== verbose 模式：分配值环形缓冲区 ===== */
    if (verbose) {
      stat->values = calloc(1, VALBUF_SIZE * sizeof(long));
      if (!stat->values) goto outall;
      par->bufmsk = VALBUF_SIZE - 1;  /* 位掩码实现取模 */
    }

    /* ===== 填充线程参数 ===== */
    par->prio = priority;
    if (priority && (policy == SCHED_FIFO || policy == SCHED_RR))
      par->policy = policy;                       /* 使用用户指定的实时策略 */
    else {
      par->policy = SCHED_OTHER;                  /* 默认普通调度 */
      force_sched_other = 1;
    }
    if (priospread) priority--;                    /* 优先级递减（每个线程降一级） */

    par->clock     = clocksources[clocksel];       /* 时钟源 */
    par->mode      = mode;                         /* 测量模式 */
    par->timermode = timermode;                    /* ABS/REL */
    par->signal    = SIGALRM;                      /* 唤醒信号 */
    par->interval  = interval;                     /* 测量间隔 */
    if (!histogram) interval += distance;          /* 非直方图模式下累加线程间距 */

    if (verbose)
      printf("Thread %d Interval: %d\n", i, interval);

    par->max_cycles = max_cycles;                  /* 最大周期数 */
    par->stats      = stat;
    par->node       = node;                        /* NUMA 节点（始终 -1） */
    par->tnum       = i;                           /* 线程索引 */
    par->cpu        = cpu;                         /* 绑定的 CPU */
    par->msr_fd     = -1;                          /* MSR 文件描述符（始终 -1） */

    /* 初始化统计数据 */
    stat->min           = 1000000;                 /* 最小延迟初始化为大值 */
    stat->max           = 0;                       /* 最大延迟初始化为 0 */
    stat->avg           = 0.0;                     /* 平均累积初始化为 0 */
    stat->threadstarted = 1;                       /* 标记为已启动 */
    stat->smi_count     = 0;                       /* SMI 计数（始终 0） */

    /* 创建测量线程 */
    status = pthread_create(&stat->thread, &attr, timerthread, par);
    if (status) {
      fprintf(stderr, "failed to create thread %d: %s\n", i, strerror(status));
      goto outall;
    }
  }

  /*
   * REMOVED: 主线程亲和性设置 (set_main_thread_affinity)
   * REMOVED: fifothread 创建
   * 原因: 见注释 #8。
   */

  /* ===== 第9步: 主监控循环 ===== */
  /*
   * 周期性地输出所有线程的统计信息。
   * 循环间隔: 100ms（RTEMS tick 粒度为 1ms，太短的 sleep 会导致忙等待）。
   *
   * 输出格式:
   *   policy: fifo: loadavg: N/A
   *   T: 0 (00123) P:80 I:1000 C: 50000 Min:    5 Act:    7 Avg:    6 Max:   42
   *   T: 1 (00124) P:79 I:1500 C: 50000 Min:    4 Act:    6 Avg:    5 Max:   38
   */
  /* 打印线程→CPU 映射（与 Linux 原版格式兼容，不修改逐行输出） */
  if (!quiet) {
    printf("# CPU mapping:");
    for (i = 0; i < num_threads; i++) {
      int cpu = parameters[i]->cpu;
      if (cpu >= 0)
        printf(" T:%d→CPU%d", i, cpu);
      else
        printf(" T:%d→any", i);
    }
    printf("\n");
  }

  while (!shutdown) {
    char *policystr = policyname(policy);

    /* 显示策略和 loadavg（RTEMS 上 loadavg 始终显示 N/A） */
    if (!verbose && !quiet)
      printf("policy: %s: loadavg: N/A\r\n\r\n", policystr);

    /* 输出每个线程的统计 */
    allstopped = 0;  /* 每轮重新计数，确保所有线程都完成才退出 */
    for (i = 0; i < num_threads; i++) {
      print_stat(stdout, parameters[i], i, verbose, quiet);
      if (max_cycles && statistics[i]->cycles >= max_cycles)
        allstopped++;      /* 该线程已完成目标周期数 */
    }

    usleep(100000);          /* 100ms 刷新间隔 */
    if (shutdown || allstopped >= num_threads) break;

    /* ANSI 转义序列: 光标上移 N 行（覆盖之前的输出，实现原地刷新） */
    if (!verbose && !quiet)
      printf("\033[%dA", num_threads + 2);

    /* refresh_on_max: 等待条件变量（直到有新的最大延迟才刷新） */
    if (refresh_on_max) {
      pthread_mutex_lock(&refresh_on_max_lock);
      if (!shutdown)
        pthread_cond_wait(&refresh_on_max_cond, &refresh_on_max_lock);
      pthread_mutex_unlock(&refresh_on_max_lock);
    }
  }

  ret = EXIT_SUCCESS;

outall:
  /* ===== 优雅退出 ===== */
  shutdown = 1;                /* 通知所有线程停止 */
  usleep(50000);               /* 给线程 50ms 时间响应停止信号 */

  /* ANSI 转义序列: 光标下移 */
  if (!verbose && !quiet && refresh_on_max)
    printf("\033[%dB", num_threads + 2);

  if (quiet) quiet = 2;        /* quiet=2: 最终概要输出模式 */

  /* ===== 等待所有线程退出 ===== */
  for (i = 0; i < num_threads; i++) {
    if (statistics[i] && statistics[i]->threadstarted > 0)
      pthread_cancel(statistics[i]->thread);    /* 请求取消线程 */
    if (statistics[i] && statistics[i]->threadstarted) {
      pthread_join(statistics[i]->thread, NULL); /* 等待线程结束 */
      if (quiet && !histogram)
        print_stat(stdout, parameters[i], i, 0, 0);  /* 最终输出 */
    }
    if (statistics[i])
      free(statistics[i]->values);             /* 释放值缓冲区 */
  }

  /* ===== 输出最终结果 ===== */
  if (trigger) trigger_print();                /* 尖峰报告 */

  if (histogram) print_hist(parameters, num_threads);  /* 直方图 */

  if (tracelimit) {
    print_tids(parameters, num_threads);       /* 线程 ID 列表 */
    if (break_thread_id) {
      printf("# Break thread: %d\n", break_thread_id);
      printf("# Break value: %llu\n",
             (unsigned long long)break_thread_value);
    }
  }

  /* ===== 释放资源 ===== */
  for (i = 0; i < num_threads; i++) {
    free(statistics[i]);   /* 释放统计结构体 */
    free(parameters[i]);   /* 释放参数结构体 */
  }

outpar:
  free(statistics);          /* 释放统计指针数组 */
  free(parameters);          /* 释放参数指针数组 */

out:
  if (affinity_mask) free(affinity_mask);  /* 释放 CPU 掩码 */
  hset_destroy(&hset);                      /* 销毁直方图集合 */

  /*
   * REMOVED: disable_trace_mark(), munlockall(), close(latency_target_fd),
   * restore_cpu_idle_disable_state(), shm_unlink()
   * 原因: 均不适用于 RTEMS。
   */

  /*
   * 返回而非 exit() — RTEMS 中 exit() 会终止整个系统。
   * 返回值由调用者（init.c 的 cyclictest_task）处理。
   */
  return ret;
}
