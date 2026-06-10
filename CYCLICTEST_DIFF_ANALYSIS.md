# Cyclictest 原版 (rt-tests-2.10) vs RTEMS 6.1 适配版 逐行差异分析

> 本文档对原版 Linux cyclictest (`rt-tests-2.10/src/cyclictest/cyclictest.c`, 2362行)
> 与 RTEMS 适配版 (`rtems-6.1/testsuites/samples/cyclictest/cyclictest.c`, ~1520行)
> 进行逐行对比，并解释每一处修改的原因。
>
> 同时涵盖新增文件 `init.c` (522行)、`cyclictest.h` (199行) 以及
> `histogram.c/h` 的细微调整。

---

## 总体变化概览

| 指标 | 原版 | RTEMS版 | 变化 |
|------|------|---------|------|
| `cyclictest.c` 行数 | 2362 | ~1520 | -842 (-36%) |
| 依赖头文件数 | 7个库头文件 | 1个库头文件 | 全部内联合并 |
| 内核特性依赖 | 12+ | 0 | 全部移除 |
| 新增文件 | 无 | `init.c` + `cyclictest.h` | +721行 |
| `histogram.c` | 181行 | 181行 | 仅格式调整 |

---

## 第1部分：许可证 & 头注释 (原版 L1-10, RTEMS L1-53)

### 原版 (L1-10)
```c
// SPDX-License-Identifier: GPL-2.0-only
/*
 * High resolution timer test software
 *
 * (C) 2013      Clark Williams <williams@redhat.com>
 * (C) 2013      John Kacur <jkacur@redhat.com>
 * (C) 2008-2012 Clark Williams <williams@redhat.com>
 * (C) 2005-2007 Thomas Gleixner <tglx@linutronix.de>
 *
 */
```

### RTEMS版 (L1-53)
```c
/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  cyclictest.c — RTEMS port of rt-tests-2.10 cyclictest
 *  ...
 *  === Features REMOVED and why ===
 *  1. NUMA support ...
 *  2. SMI counter ...
 *  ...
 *  9. Linux scheduler priority limits ...
 */
```

### 差异说明
| 对比项 | 原版 | RTEMS版 | 原因 |
|--------|------|---------|------|
| 许可证 | GPL-2.0-only | BSD-2-Clause | RTEMS 内核使用 BSD-2-Clause，适配代码需兼容 |
| 头注释 | 简短作者信息 | 详细的移除特性清单（9大项） | 方便维护者快速理解哪些特性被移除以及为什么 |

---

## 第2部分：头文件引用 (原版 L11-43, RTEMS L55-57)

### 原版
```c
#ifdef HAVE_LIBCPUPOWER_SUPPORT
#include <cpuidle.h>          // CPU idle state management
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>            // file control (open/close/fifo)
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#include <sys/stat.h>         // stat(), mkfifo()
#include <sys/types.h>
#include <sys/time.h>         // gettimeofday()
#include <sys/resource.h>     // getrlimit/setrlimit
#include <sys/utsname.h>      // uname()
#include <sys/mman.h>         // mmap/shm_open
#include <sys/syscall.h>      // syscall(SYS_gettid)

#include "rt_numa.h"          // NUMA support
#include "rt-utils.h"         // trace, json, privs
#include "rt-numa.h"          // NUMA bitmask, cpu selection
#include "rt-error.h"         // warn/fatal/err_msg
#include "histogram.h"
#include <bionic.h>           // Android bionic libc compatibility
```

### RTEMS版
```c
#include "cyclictest.h"       // 所有公共声明 + 内联函数
#include "histogram.h"
#include <getopt.h>
```

### 差异说明
| 原版头文件 | RTEMS处理方式 | 原因 |
|------------|--------------|------|
| `<cpuidle.h>` | 移除 | CPU idle 管理不适用于 RTEMS |
| `<fcntl.h>` | 移除 | 无文件操作需求（mkfifo/shm_open均移除） |
| `<limits.h>` | 移除 | rlimit 相关移除 |
| `<sys/stat.h>` | 移除 | mkfifo 不可用 |
| `<sys/resource.h>` | 移除 | getrlimit/setrlimit 不可用 |
| `<sys/utsname.h>` | 移除 | uname() 调用移除 |
| `<sys/mman.h>` | 移除 | mmap/shm_open 不可用 |
| `<sys/syscall.h>` | 移至 cyclictest.h | `__rtems__` 条件编译切换 gettid() 实现 |
| `"rt_numa.h"` | 移除 | 无 NUMA |
| `"rt-utils.h"` | 内联合并 | warn/fatal/tsnorm/calcdiff 等全部内联到 cyclictest.c |
| `"rt-numa.h"` | 移除 | 无 NUMA |
| `"rt-error.h"` | 内联合并 | warn()/fatal() 内联为简单 fprintf |
| `<bionic.h>` | 移除 | Android 兼容层，RTEMS 不需要 |
| `"cyclictest.h"` | **新增** | 统一声明所有数据结构、常量、函数原型 |

**关键设计决策**：原版依赖 7 个自定义库头文件 + 10+ 个系统头文件，RTEMS 版将所有必要声明集中到 `cyclictest.h`，将核心工具函数内联到 `cyclictest.c`，大幅简化依赖关系。

---

## 第3部分：宏定义 (原版 L45-101, RTEMS cyclictest.h L37-76)

### 原版 (L45-53)
```c
#define DEFAULT_INTERVAL 1000
#define DEFAULT_DISTANCE 500

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef SCHED_NORMAL
#define SCHED_NORMAL SCHED_OTHER
#endif

#define sigev_notify_thread_id _sigev_un._tid
```

### RTEMS版 (cyclictest.h L37-76)
```c
#define MSEC_PER_SEC        1000
#define USEC_PER_SEC        1000000
#define NSEC_PER_SEC        1000000000LL
#define USEC_TO_NSEC(u)     ((u) * 1000)
#define NSEC_TO_USEC(n)     ((n) / 1000)

#define DEFAULT_INTERVAL    1000
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

#define MODE_CYCLIC             0
#define MODE_CLOCK_NANOSLEEP    1
#define MODE_SYS_ITIMER         2   /* NOT on RTEMS */
#define MODE_SYS_NANOSLEEP      3   /* tick-based, drifting */
#define MODE_SYS_OFFSET         2

#define TIMER_RELTIME           0

#define HIST_MAX                1000000
#define VALBUF_SIZE             16384
#define MAX_PATH                256
#define ARRAY_SIZE(x)           (sizeof(x) / sizeof((x)[0]))
```

### 差异说明

| 原版 | RTEMS版 | 原因 |
|------|---------|------|
| `MSEC_PER_SEC` 等在 `rt-utils.h` 中 | 移入 `cyclictest.h` | 库头文件已移除，常量统一管理 |
| `sigev_notify_thread_id` 宏 | **移除** | Linux 使用 `SIGEV_THREAD_ID` 将信号发送到指定线程（通过 `_sigev_un._tid` 字段）。RTEMS 的 POSIX 信号支持有限，改用 `SIGEV_SIGNAL`（进程级信号），不需要此宏 |
| `SCHED_BATCH` 未定义 | 新增 `#define SCHED_BATCH 3` | 原版依赖 glibc 头文件中的定义，RTEMS 的 `<sched.h>` 可能不包含此值 |
| `MODE_SYS_ITIMER` | 保留定义但标记为不可用 | `setitimer()` 是 Linux 特有的进程定时器机制，RTEMS 不支持 |
| `MODE_SYS_NANOSLEEP` | **已实现** | 使用 POSIX `nanosleep()` (RTEMS 内部调用 `clock_nanosleep`)，tick-based 相对睡眠，延迟逐周期漂移 |

### 原版 (L57-88) — uClibc 兼容代码
```c
#ifdef __UCLIBC__
#define MAKE_PROCESS_CPUCLOCK(pid, clock) \
    ((~(clockid_t) (pid) << 3) | (clockid_t) (clock))
#define CPUCLOCK_SCHED          2

static int clock_nanosleep(clockid_t clock_id, int flags, ...)
{
    if (clock_id == CLOCK_THREAD_CPUTIME_ID)
        return -EINVAL;
    if (clock_id == CLOCK_PROCESS_CPUTIME_ID)
        clock_id = MAKE_PROCESS_CPUCLOCK(0, CPUCLOCK_SCHED);
    return syscall(__NR_clock_nanosleep, clock_id, flags, req, rem);
}

int sched_setaffinity(__pid_t __pid, size_t __cpusetsize,
               __const cpu_set_t *__cpuset)
{
    return -EINVAL;     // uClibc 不支持 CPU 亲和性
}

#undef CPU_SET
#undef CPU_ZERO
#define CPU_SET(cpu, cpusetp)    // 空宏（禁用）
#define CPU_ZERO(cpusetp)        // 空宏（禁用）
#else
extern int clock_nanosleep(...);
#endif  /* __UCLIBC__ */
```

### RTEMS版：**整个 uClibc 块移除**

**原因**：uClibc 是一个面向嵌入式 Linux 的小型 C 库。RTEMS 使用自己的 libc 实现（Newlib），不基于 uClibc。`clock_nanosleep` 在 RTEMS 中通过标准 POSIX 接口提供（或不可用）。这段兼容代码在 RTEMS 上完全不适用。

### 原版 (L101-108) — SMI 计数器检测
```c
#if (defined(__i386__) || defined(__x86_64__))
#define ARCH_HAS_SMI_COUNTER
#endif

#define MSR_SMI_COUNT       0x00000034
#define MSR_SMI_COUNT_MASK  0xFFFFFFFF
```

### RTEMS版：**整个 SMI 块移除**

**原因**：
- SMI (System Management Interrupt) 是 x86 BIOS 层面的事务，通过 MSR (Model-Specific Register) 计数器来检测
- `/dev/cpu/N/msr` 是 Linux 特有的设备接口，用于读取 MSR 寄存器
- RTEMS 运行在嵌入式平台上，没有这个设备接口，SMI 处理也不适用
- 在 `cyclictest.h` 中 `smi` 变量被 `#define` 为 0，所有相关代码被编译器作为死代码消除

---

## 第4部分：数据结构 (原版 L110-168, RTEMS cyclictest.h L113-158)

### thread_param 结构体

原版和 RTEMS 版几乎一致，唯一差异：

| 字段 | 原版 | RTEMS版 | 原因 |
|------|------|---------|------|
| `node` | `int node;` | `int node; /* NUMA node, -1 = none */` | 保留字段但始终为 -1，注释说明原因 |
| `msr_fd` | `int msr_fd;` | `int msr_fd; /* SMI fd, -1 = none */` | 保留字段但始终为 -1，注释说明原因 |

**为什么保留这些字段而不是删除？**
- 保持结构体布局与原版一致，减小 `timerthread()` 函数的改动范围
- `if (par->node != -1)` 被编译器优化掉（`node` 始终为 -1），无运行时开销
- `if (smi)` 被编译器优化掉（`smi` 定义为 0），无运行时开销

### thread_stat 结构体

原版和 RTEMS 版完全一致。

### thread_trigger 结构体

原版和 RTEMS 版完全一致。

### 枚举

```c
enum {
    AFFINITY_UNSPECIFIED = 0,
    AFFINITY_SPECIFIED   = 1,
    AFFINITY_USEALL      = 2,
};
```

原版定义在 `rt-numa.h` 中，RTEMS 版移入 `cyclictest.h`。

---

## 第5部分：全局变量 (原版 L150-236, RTEMS L77-155)

### 逐项对比

| 原版变量 | RTEMS版 | 状态 | 原因 |
|----------|---------|------|------|
| `shutdown` | `int shutdown;` (非static) | **改为非static** | `init.c` 需要在信号处理/退出流程中访问 |
| `tracelimit` | 保留 | 不变 | |
| `trace_marker` | **移除** | — | ftrace 不可用 |
| `verbose` | 保留 | 不变 | |
| `oscope_reduction` | 保留 | 不变 | |
| `lockall` | **移除** | — | 无虚拟内存/swap |
| `histogram` | 保留 | 不变 | |
| `histofall` | 保留 | 不变 | |
| `duration` | 保留 | 不变 | |
| `use_nsecs` | 保留 | 不变 | |
| `refresh_on_max` | 保留 | 不变 | |
| `force_sched_other` | 保留 | 不变 | |
| `priospread` | 保留 | 不变 | |
| `check_clock_resolution` | 保留 | 不变 | |
| `ct_debug` | 保留 | 不变 | |
| `use_fifo` | **移除** | — | mkfifo 不可用 |
| `fifo_threadid` | **移除** | — | fifothread 移除 |
| `laptop` | **移除** | — | 无电源管理 |
| `power_management` | **移除** | — | 无电源管理 |
| `use_histfile` | **移除** | — | 简化（仅输出到 stdout） |
| `smi` | **#define smi 0** | 宏替换为 0 | 编译器死代码消除（见 cyclictest.h L88） |
| `aligned/secaligned/offset` | 保留 | 不变 | Pthread barrier 在 RTEMS 上支持 |
| `align_barr/globalt_barr` | 保留 | 不变 | |
| `globalt` | 保留 | 不变 | |
| `fifopath[]` | **移除** | — | 无 FIFO 输出 |
| `histfile[]` | **移除** | — | 简化 |
| `jsonfile[]` | 保留 | 简化 | 仅保留 JSON 文件路径 |
| `latency_target_fd` | **移除** | — | 无 /dev/cpu_dma_latency |
| `latency_target_value` | **移除** | — | 无电源管理 |
| `deepest_idle_state` | **移除** | — | 无 cpuidle |
| `rstat_fd/shm_name[]` | **移除** | — | 无 shm_open |
| `affinity_mask` | `struct bitmask *` → **`cpu_set_t *`** | 类型改变 | 见下文 |
| `main_affinity_mask` | `struct bitmask *` → **`cpu_set_t *`** | 类型改变 | 见下文 |
| `numa` | **移除** | — | 无 NUMA |

### affinity_mask 类型变更详解

```c
// 原版：使用 libnuma 的 struct bitmask
static struct bitmask *affinity_mask = NULL;
parse_cpumask(optarg, max_cpus, &affinity_mask);  // libnuma 函数
numa_bitmask_weight(affinity_mask);                 // libnuma 函数
rt_bitmask_free(affinity_mask);                    // libnuma 函数

// RTEMS版：直接使用 POSIX cpu_set_t
static cpu_set_t *affinity_mask = NULL;
CPU_ZERO(affinity_mask);
CPU_SET(cpu, affinity_mask);  // 标准 POSIX 宏
```

**原因**：
- `struct bitmask` 来自 `libnuma`，用于表达复杂的 CPU mask（如 `"3-5,0"` 这样的范围语法）
- RTEMS 不支持 libnuma
- RTEMS 上 CPU 数量少（通常 <= 4），简单的 `cpu_set_t` 完全够用
- 代价：不支持范围语法（如 `-a 0-3`），仅支持单个 CPU 号（如 `-a 0`）或全部 CPU（`-a` 无参数 = USEALL）

---

## 第6部分：移除的函数 (详细分析)

### 6.1 set_latency_target() — 原版 L247-289，完整移除
```c
// 原版功能：
// 向 /dev/cpu_dma_latency 写入延迟目标值
// 防止 CPU 进入深度 C-state（省电状态）
// 确保实时响应的低延迟
static void set_latency_target(void)
{
    latency_target_fd = open("/dev/cpu_dma_latency", O_RDWR);
    write(latency_target_fd, &latency_target_value, 4);
}
```

**RTEMS 移除原因**：
- `/dev/cpu_dma_latency` 是 Linux PM QoS (Power Management Quality of Service) 接口
- RTEMS 是实时操作系统，CPU 始终全速运行，不存在 C-state 概念
- 该功能在 RTEMS 上是纯粹的 no-op

### 6.2 CPU Idle State — 原版 L291-444，完整移除
```c
// 原版功能：
// save_cpu_idle_disable_state()   — 保存 CPU idle 状态
// restore_cpu_idle_disable_state() — 恢复 CPU idle 状态
// set_deepest_cpu_idle_state()    — 设置最深 idle 状态
// free_cpu_idle_disable_states()  — 释放资源
// have_libcpupower_support()      — 检测 libcpupower 支持
```

**RTEMS 移除原因**：
- 这些函数依赖 `libcpupower` 库，该库是 Linux 特有的
- `cpuidle_state_count()`、`cpuidle_is_state_disabled()`、`cpuidle_state_disable()` 操作 Linux cpuidle 子系统
- RTEMS 不管理 CPU idle 状态，CPU 始终运行
- `#ifdef HAVE_LIBCPUPOWER_SUPPORT` 条件编译块 ~150行 全部移除
- `#else` 分支中的空桩函数也一并移除

### 6.3 raise_soft_prio() — 原版 L457-501，完整移除
```c
// 原版功能：
// 如果当前进程的 soft rlimit 不够高但 hard rlimit 够高，
// 自动提升 soft limit 以允许设置实时优先级
static int raise_soft_prio(int policy, const struct sched_param *param)
{
    getrlimit(RLIMIT_RTPRIO, &rlim);   // 获取当前限制
    if (prio > soft_max && prio <= hard_max) {
        rlim.rlim_cur = prio;
        setrlimit(RLIMIT_RTPRIO, &rlim); // 提升 soft limit
    }
}
```

**RTEMS 移除原因**：
- Linux 的安全模型：非 root 用户有 `RLIMIT_RTPRIO` 限制
- RTEMS 是裸机/嵌入式 RTOS，**没有用户/权限概念**
- 所有任务都可以直接设置实时优先级
- `getrlimit()`/`setrlimit()` 在 RTEMS 上是未实现的 syscall

### 6.4 setscheduler() — 原版 L508-525, RTEMS L283-289
```c
// 原版：带重试逻辑的 setscheduler
static int setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
    int err = 0;
try_again:
    err = sched_setscheduler(pid, policy, param);
    if (err) {
        err = errno;
        if (err == EPERM) {
            int err1 = raise_soft_prio(policy, param);  // 尝试提升 rlimit
            if (!err1) goto try_again;                   // 重试
        }
    }
    return err;
}

// RTEMS版：直接调用 sched_setscheduler
static int setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
    int err = sched_setscheduler(pid, policy, param);
    if (err) err = errno;
    return err;
}
```

**简化原因**：
- 无 `RLIMIT_RTPRIO` 限制，不需要 `raise_soft_prio`
- `EPERM` 在 RTEMS 上不会因为权限问题触发
- 实际上 RTEMS 的 `sched_setscheduler()` 本身是 stub（返回 ENOSYS），真正的调度策略通过 `pthread_attr_setschedpolicy()` 在创建线程时设置（见下文 init.c 相关分析）

### 6.5 SMI Counter 函数 — 原版 L527-618，完整移除

```c
// 原版功能（4个函数）：
static int open_msr_file(int cpu);      // 打开 /dev/cpu/N/msr
static int get_msr(int fd, ...);        // 读取 MSR 寄存器
static int get_smi_counter(int fd, ...);// 读取 SMI 计数器
static int has_smi_counter(void);       // CPUID 检测是否支持 SMI 计数
```

**RTEMS 移除原因**：
- `/dev/cpu/N/msr` 是 Linux 设备文件系统接口
- `pread()` 系统调用读取 MSR (Model-Specific Register) 在 RTEMS 上不可用
- `__get_cpuid()` 来自 `<cpuid.h>`，特定于 x86 架构
- `ARCH_HAS_SMI_COUNTER` 条件编译块（~90 行）全部移除
- `smi` 变量在 `cyclictest.h` 中被 `#define` 为 0，编译器自动消除所有 `if(smi)` 分支

### 6.6 rstat (Running Status) — 原版 L1773-1869，完整移除

```c
// 原版功能（6个函数）：
static int  rstat_shm_open(void);       // shm_open() 创建共享内存
static int  rstat_ftruncate(int, ...);  // ftruncate() 调整共享内存大小
static void *rstat_mmap(int fd);        // mmap() 映射共享内存
static int  rstat_mlock(void *);        // mlock() 锁定共享内存页面
static void  rstat_setup(void);         // 完整初始化流程
static void  rstat_print_stat(...);     // 写入共享内存的统计输出
```

**RTEMS 移除原因**：
- `shm_open()`/`shm_unlink()`：POSIX 共享内存，RTEMS 不一定支持
- `mmap()`/`munmap()`：内存映射，RTEMS 无 MMU 的场景不支持
- `mlock()`：内存锁定，RTEMS 无虚拟内存，不需要
- `ftruncate()`：可能不可用
- `rstat` 是调试便利功能（另一个进程可读取运行状态），非核心功能
- 约 150 行代码全部移除

### 6.7 fifothread() — 原版 L1690-1717，完整移除

```c
// 原版功能：
// 创建命名管道 (FIFO)，当有进程读取时输出统计信息
static void *fifothread(void *param)
{
    mkfifo(fifopath, 0666);           // 创建命名管道
    fd = open(fifopath, O_WRONLY|O_NONBLOCK);
    // 循环向 FIFO 写入统计数据
}
```

**RTEMS 移除原因**：
- `mkfifo()` 是 Linux/SYSV IPC 机制
- RTEMS 没有命名管道的文件系统支持
- 统计数据改为直接输出到 stdout/串口

---

## 第7部分：timerthread() 核心测量线程 (原版 L632-925, RTEMS L824-1152)

这是 cyclictest 的心脏。逐段对比：

### 7.1 局部变量

原版 L632-646 vs RTEMS L824-836：

```c
// 原版
cpu_set_t mask;
unsigned long smi_now, smi_old = 0;

// RTEMS版 — 移除 smi_now/smi_old
// (smi 相关代码已通过 #define smi 0 消除)
// 新增了 USE_TSC 相关的局部变量（在测量循环内部）
```

### 7.2 NUMA 节点绑定 (原版 L649-650)

```c
// 原版
if (par->node != -1)
    rt_numa_set_numa_run_on_node(par->node, par->cpu);
```

**RTEMS 移除**：`node` 始终为 -1，条件永不成立。编译器优化掉。

### 7.3 CPU 亲和性 (原版 L652-659, RTEMS L843-851)

```c
// 原版
CPU_ZERO(&mask);
CPU_SET(par->cpu, &mask);
thread = pthread_self();
if (pthread_setaffinity_np(thread, sizeof(mask), &mask) != 0)
    warn("Could not set CPU affinity to CPU #%d\n", par->cpu);

// RTEMS版 — 逻辑相同，仅格式调整
if (par->cpu != -1) {
    cpu_set_t mask;
    pthread_t thread;
    CPU_ZERO(&mask);
    CPU_SET(par->cpu, &mask);
    thread = pthread_self();
    if (pthread_setaffinity_np(thread, sizeof(mask), &mask) != 0)
        warn("Could not set CPU affinity to CPU #%d\n", par->cpu);
}
```

**差异**：无实质性差异。`pthread_setaffinity_np` 在 RTEMS 上由 RTEMS 内核实现。

### 7.4 线程 ID (原版 L664, RTEMS L857)

```c
// 原版
stat->tid = gettid();  // gettid() 定义在 rt-utils.c: syscall(SYS_gettid)

// RTEMS版
stat->tid = gettid();  // gettid() 定义在 cyclictest.h:
                       // #ifdef __rtems__: return (pid_t)(uintptr_t)pthread_self()
                       // #else: return (pid_t)syscall(SYS_gettid)
```

**差异**：`gettid()` 实现不同。Linux 用 `SYS_gettid` syscall 获取内核线程 ID，RTEMS 用 `pthread_self()` 获取 POSIX 线程 ID。

### 7.5 MODE_CYCLIC: timer_create (原版 L670-676, RTEMS L868-877)

```c
// 原版
sigev.sigev_notify = SIGEV_THREAD_ID | SIGEV_SIGNAL;
sigev.sigev_signo = par->signal;
sigev.sigev_notify_thread_id = stat->tid;  // Linux 特有：定向到特定线程
timer_create(par->clock, &sigev, &timer);

// RTEMS版
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
sigev.sigev_notify = SIGEV_SIGNAL;         // 进程级信号（非线程定向）
sigev.sigev_signo  = par->signal;
timer_create(par->clock, &sigev, &timer);
#else
par->mode = MODE_CLOCK_NANOSLEEP;          // 回退到 clock_nanosleep
#endif
```

**差异**：
| 项目 | 原版 | RTEMS版 |
|------|------|---------|
| 通知方式 | `SIGEV_THREAD_ID \| SIGEV_SIGNAL` | `SIGEV_SIGNAL` |
| 线程定向 | `sigev_notify_thread_id` 字段 | 不支持，使用进程级信号 |
| 回退策略 | 无 | `CONFIGURE_ENABLE_POSIX_API` 未定义时回退到 `MODE_CLOCK_NANOSLEEP` |

**原因**：`SIGEV_THREAD_ID` 是 Linux 内核扩展，允许信号定向到特定线程。RTEMS 的信号模型较简单，不支持此特性。如果 RTEMS 配置了 `CONFIGURE_MAXIMUM_POSIX_TIMERS > 0`，则可以使用 POSIX 定时器 + `SIGEV_SIGNAL`；否则自动回退到 `clock_nanosleep`。

### 7.6 调度策略设置 (原版 L678-682, RTEMS L880-894)

```c
// 原版
memset(&schedp, 0, sizeof(schedp));
schedp.sched_priority = par->prio;
if (setscheduler(0, par->policy, &schedp))   // 带 rlimit 重试的版本
    fatal("timerthread%d: failed to set priority to %d\n", par->cpu, par->prio);

// RTEMS版
#ifndef __rtems__
if (par->policy != SCHED_OTHER || par->prio > 0) {
    memset(&schedp, 0, sizeof(schedp));
    schedp.sched_priority = par->prio;
    if (setscheduler(0, par->policy, &schedp))  // 简化版本
        fatal("timerthread%d: failed to set priority to %d\n", par->cpu, par->prio);
}
#endif
```

**差异**：
- RTEMS 版用 `#ifndef __rtems__` 跳过整个调度设置逻辑
- 原版即使 `SCHED_OTHER` 也调用 `sched_setscheduler`

**原因**：
- RTEMS 的 `sched_setscheduler()` 是 stub（返回 `ENOSYS`），因为 RTEMS 不支持进程级别的调度策略切换
- RTEMS 的调度策略在 **线程创建时** 通过 `pthread_attr_setschedpolicy()` 设置（见 `cyclictest_main` 中的线程创建代码 L1332-1339）
- 这是 RTEMS 与 Linux 在线程/进程模型上的根本差异：Linux 中进程有调度策略，线程继承；RTEMS 中每个任务（线程）独立设置

### 7.7 SMI 初始化 (原版 L684-693)

```c
// 原版 — 完整移除
if (smi) {
    par->msr_fd = open_msr_file(par->cpu);
    if (get_smi_counter(par->msr_fd, &smi_old))
        fatal("Could not read SMI counter\n");
}
```

`smi` 在 RTEMS 中 `#define smi 0`，整个块被编译器作为死代码消除。

### 7.8 MODE_SYS_ITIMER (原版 L739-743，完全移除)

```c
// 原版
if (par->mode == MODE_SYS_ITIMER) {
    itimer.it_interval.tv_sec  = interval.tv_sec;
    itimer.it_interval.tv_usec = interval.tv_nsec / 1000;
    itimer.it_value = itimer.it_interval;
    setitimer(ITIMER_REAL, &itimer, NULL);
}
```

**RTEMS 移除原因**：
- `setitimer()` 是 Linux 内核实现的进程级间隔定时器
- 每次到期发送 `SIGALRM` 到进程
- RTEMS 对这个系统调用的支持有限，不是一个可靠的实时调度手段
- `clock_nanosleep` 和 POSIX 定时器已覆盖所有实用场景

### 7.9 TSC 校准 (RTEMS 新增 L957-975)

```c
// RTEMS版 — 完全新增
#if USE_TSC
{
    struct timespec cal_req, cal_start, cal_end;
    unsigned long long cal_tsc1, cal_tsc2;
    cal_req.tv_sec = 0; cal_req.tv_nsec = 50000000; /* 50ms */
    clock_gettime(par->clock, &cal_start);
    cal_tsc1 = rdtsc();
    clock_nanosleep(CLOCK_MONOTONIC, 0, &cal_req, NULL);
    cal_tsc2 = rdtsc();
    clock_gettime(par->clock, &cal_end);
    // 计算每微秒的 TSC 周期数
    tsc_per_us = (double)(cal_tsc2 - cal_tsc1) / cal_ns * 1000;
}
#endif
```

**新增原因**：
- 原版完全依赖 `clock_gettime()` 测量延迟
- RTEMS 版可选使用硬件 TSC (Time Stamp Counter) 进行更高精度的测量
- 校准方法：睡眠 50ms，用 `clock_gettime` 获取真实时间，用 `rdtsc` 获取 TSC 周期数，计算出 TSC 频率
- `USE_TSC` 宏控制（默认启用），设为 0 可回退到原版的 `clock_gettime` 方式

### 7.10 测量循环核心 (原版 L748-891, RTEMS L978-1121)

这是最关键的算法部分。逐子块对比：

#### 等待下一周期

```c
// 原版 L754-808
switch (par->mode) {
case MODE_CYCLIC:
case MODE_SYS_ITIMER:
    if (sigwait(&sigset, &sigs) < 0) goto out;
    break;

case MODE_CLOCK_NANOSLEEP:
    // ABS/REL 两种方式
    break;

case MODE_SYS_NANOSLEEP:
    // nanosleep() 方式
    break;
}

// RTEMS版 L987-1066
switch (par->mode) {
case MODE_CYCLIC:
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
    if (sigwait(&sigset, &sigs) < 0) goto out;
#endif
    break;

case MODE_CLOCK_NANOSLEEP:
    // ABS/REL 两种方式（相同）
    break;

case MODE_SYS_NANOSLEEP:
    // nanosleep() tick-based 相对睡眠（已实现）
    // 记录 sleep 前时刻 → nanosleep(&interval) → 计算预期唤醒时间
    break;
}
```

**差异**：
1. `MODE_SYS_ITIMER` 分支移除（`setitimer()` 是 Linux 特有）
2. `MODE_SYS_NANOSLEEP` 分支**新增实现**：使用 POSIX `nanosleep()` 进行 tick-based 相对睡眠。与 `MODE_CLOCK_NANOSLEEP` 的关键区别在于 `next` 更新方式：`next = now + interval`（从实际醒来时刻漂移），而非 `next += interval`（绝对时间锚点推进）
3. `MODE_CYCLIC` 增加了条件编译保护

#### 延迟测量

```c
// 原版 L828-831
if (use_nsecs)
    diff = calcdiff_ns(now, next);  // 纳秒模式
else
    diff = calcdiff(now, next);     // 微秒模式

// RTEMS版 L1028-1055
#if USE_TSC
    // 用硬件 TSC 计时
    unsigned long long tsc_after = rdtsc();
    long long tsc_elapsed = (long long)(tsc_after - tsc_before);
    long long tsc_expected = (long long)((double)par->interval * tsc_per_us);
    diff = (uint64_t)(tsc_elapsed - tsc_expected);
    clock_gettime(par->clock, &now);  // 仍然需要 now 用于后续逻辑
#else
    // 原版方式：clock_gettime
    ret = clock_gettime(par->clock, &now);
    if (use_nsecs)
        diff = calcdiff_ns(now, next);
    else
        diff = calcdiff(now, next);
#endif
```

**差异**：
- TSC 模式下，`diff` 的值是 TSC 周期差（而非微秒/纳秒），统计输出也按 TSC ticks 显示
- TSC 在第一个线程中校准一次（`tsc_per_us` 是全局变量）
- TSC 测量比 `clock_gettime` 的延迟更真实（`clock_gettime` 本身有系统调用开销），但由于 `diff` 单位变化，`-N` (nsecs) 选项的意义也改变了

#### SMI 读取 (原版 L817-826，完全移除)

```c
// 原版 — RTEMS 完全移除
if (smi) {
    if (get_smi_counter(par->msr_fd, &smi_now)) ...
    diff_smi = smi_now - smi_old;
    stat->smi_count += diff_smi;
    smi_old = smi_now;
}
```

`smi` 为 0，死代码消除。

#### ftrace/break_trace (原版 L847-858, RTEMS L1072-1085)

```c
// 原版
if (!stopped && tracelimit && (diff > tracelimit)) {
    stopped++;
    shutdown++;
    pthread_mutex_lock(&break_thread_id_lock);
    if (break_thread_id == 0) {
        break_thread_id = stat->tid;
        tracemark("hit latency threshold (%llu > %d)", diff, tracelimit);
        tracing_stop();                           // 停止 ftrace
        break_thread_value = diff;
    }
    pthread_mutex_unlock(&break_thread_id_lock);
}

// RTEMS版
if (!stopped && tracelimit && (diff > tracelimit)) {
    stopped++;
    shutdown++;
    pthread_mutex_lock(&break_thread_id_lock);
    if (break_thread_id == 0) {
        break_thread_id = stat->tid;
        break_thread_value = diff;
        // 移除了 tracemark() 和 tracing_stop() 调用
    }
    pthread_mutex_unlock(&break_thread_id_lock);
}
```

**差异**：移除了 `tracemark()` 和 `tracing_stop()` 调用。

**原因**：ftrace 是 Linux 内核跟踪机制。`tracemark()` 向 `/sys/kernel/tracing/trace_marker` 写入标记，`tracing_stop()` 停止内核跟踪。RTEMS 无等价机制。

#### 值缓冲写入 (原版 L862-866, RTEMS L1090-1091)

```c
// 原版
if (par->bufmsk) {
    stat->values[stat->cycles & par->bufmsk] = diff;
    if (smi)
        stat->smis[stat->cycles & par->bufmsk] = diff_smi;  // SMI diff
}

// RTEMS版
if (par->bufmsk)
    stat->values[stat->cycles & par->bufmsk] = diff;
// SMI 相关写入移除
```

#### 定时器超限处理 (原版 L876-880, RTEMS L1103-1120)

```c
// 原版
if (par->mode == MODE_CYCLIC) {
    int overrun_count = timer_getoverrun(timer);
    next.tv_sec  += overrun_count * interval.tv_sec;
    next.tv_nsec += overrun_count * interval.tv_nsec;
}

// RTEMS版 — 区分 MODE_SYS_NANOSLEEP 的漂移语义
if (par->mode == MODE_SYS_NANOSLEEP) {
    // nanosleep 模式：从实际醒来时刻重新计算（漂移）
    next.tv_sec  = now.tv_sec  + interval.tv_sec;
    next.tv_nsec = now.tv_nsec + interval.tv_nsec;
    tsnorm(&next);
} else {
    // 绝对模式：从上一个预期时刻推进 interval
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
}
```

**差异**：新增了 `MODE_SYS_NANOSLEEP` 的 `next` 更新分支。`nanosleep()` 是相对睡眠（无绝对时间锚点），因此 `next = now + interval` 而非 `next += interval`——延迟会累积漂移到后续周期。

### 7.11 线程退出清理 (原版 L893-925, RTEMS L1123-1163)

```c
// 原版
if (par->mode == MODE_SYS_ITIMER) {
    itimer.it_value.tv_sec = 0;     // 停止 itimer
    itimer.it_value.tv_usec = 0;
    setitimer(ITIMER_REAL, &itimer, NULL);
}
if (smi) close(par->msr_fd);         // 关闭 MSR 文件
schedp.sched_priority = 0;           // 恢复普通优先级
sched_setscheduler(0, SCHED_OTHER, &schedp);

// RTEMS版
// MODE_SYS_ITIMER 清理移除
// close(msr_fd) 移除
#ifndef __rtems__
if (par->policy != SCHED_OTHER || par->prio > 0) {
    schedp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &schedp);  // 恢复普通优先级 (Linux only)
}
#endif
```

**关键修复**：`out:` 标签处的 `shutdown++` 从 `if (refresh_on_max)` 条件块内移出，改为**无条件执行**。这确保任何异常退出路径（如未知 mode 触发 `default: goto out`）都能正确通知主监控循环，防止主循环永久死等（cardead）。

---

## 第8部分：display_help() (原版 L929-1016, RTEMS L393-434)

### 原版
```c
static void display_help(int error)
{
    printf("cyclictest V %1.2f\n", VERSION);
    printf("Usage:\n"
           "cyclictest <options>\n\n"
           "-a [CPUSET] --affinity     Run thread #N on processor #N, if possible,\n"
           // ... 详细帮助
           "-F       --fifo=<path>     create a named pipe ...\n"
           "--histfile=<path> ..."
           "--json=FILENAME ..."
           "--laptop ..."
           "--latency=PM_QOS ..."
           "-m       --mlockall ..."
           "--smi ..."
           "--tracemark ..."
           "--deepest-idle-state ..."
    );
    if (error) exit(EXIT_FAILURE);
    exit(EXIT_SUCCESS);
}
```

### RTEMS版
```c
void display_help(int error)
{
    printf("cyclictest V 1.0.0 (RTEMS port)\n");
    printf("Usage: cyclictest <options>\n\n"
           "-a [CPU]  --affinity      pin threads to CPU\n"
           // ... 精简的帮助
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
```

**差异**：
1. 帮助文本精简（移除不可用选项的详细解释）
2. 新增"REMOVED"部分，告知用户哪些选项不可用及原因
3. `display_help` 不再调用 `exit()`（原版根据 `error` 参数决定 `EXIT_FAILURE` 或 `EXIT_SUCCESS`）。RTEMS 版只打印信息并设置 `help_printed=1` 标志，由调用者（`cyclictest_main`）在 `process_options` 返回后检查该标志并 `return EXIT_SUCCESS`，不执行测试循环。

---

## 第8.5部分：命令行参数完整对照表

以下是原版 Linux cyclictest 与 RTEMS 版的参数逐一对比，含每个参数的功能说明。

### 参数总量对比

| 版本 | 总数 | 说明 |
|------|------|------|
| **原版 Linux** (rt-tests-2.10) | **~40 个** | 含 short options 25 个 + long-only options 15 个 |
| **RTEMS 适配版** | **28 个** | 保留 16 个（行为一致）+ 12 个（行为有差异） |
| **已移除** | **12 个** | Linux 特有内核接口或未实现功能 |

```
原版 40个 ─┬─ 保留 28个 ─┬─ 16个 行为完全一致 (8.5.1)
           │              └─ 12个 行为有差异   (8.5.2)
           └─ 移除 12个 ───  Linux 特有/未实现  (8.5.3)
```

### 8.5.1 RTEMS 支持的参数（与原版行为一致，共 16 个）

| 短选项 | 长选项 | 参数 | 功能说明 |
|--------|--------|------|----------|
| `-b` | `--breaktrace` | USEC | **断点追踪**：当任意线程的延迟超过 USEC 微秒时，记录触发线程和延迟值，立即停止所有测量。用于捕获最差情况延迟 |
| `-c` | `--clock` | 0/1 | **时钟源选择**：0 = `CLOCK_MONOTONIC`（默认，不受系统时间调整影响），1 = `CLOCK_REALTIME`（受系统时间调整影响） |
| `-d` | `--distance` | DIST | **线程间隔距离**：多线程模式下，相邻线程的测量间隔偏移量（微秒）。默认 500。例如 `-t 2 -d 200` → T:0 I=1000, T:1 I=1200 |
| `-D` | `--duration` | TIME | **运行时长**：测试持续时间，到达后自动停止。支持后缀 `m`（分钟）、`h`（小时）、`d`（天），如 `-D 30s`、`-D 5m` |
| `-h` | `--histogram` | USEC | **延迟直方图**：测试结束后输出延迟分布直方图。参数指定每个桶的宽度（微秒）。超过桶范围的计入溢出统计 |
| `-H` | `--histofall` | USEC | **直方图+汇总列**：与 `-h` 类似，但每行额外打印一个汇总列（所有线程的最大值），便于多线程对比 |
| `-i` | `--interval` | INTV | **基准测量间隔**：每个测量周期的睡眠时间（微秒）。默认 1000（1ms）。值越小测量越密集，但系统开销越大 |
| `-l` | `--loops` | LOOPS | **测量周期数**：每个线程执行的测量循环次数。0 = 无限运行直到手动停止。如 `-l 500` 表示 500 次后自动停止 |
| `-M` | `--refresh_on_max` | — | **最大值刷新**：仅在出现新的最大延迟时才刷新屏幕输出。减少串口 I/O 开销，适合长时间运行 |
| `-N` | `--nsecs` | — | **纳秒显示**：所有延迟值以纳秒（ns）而非微秒（µs）为单位显示。数值大约 ×1000 |
| `-p` | `--priority` | PRIO | **实时优先级**：设置测量线程的最高优先级（0-99）。优先级 > 0 时自动切换到 FIFO 调度策略。不指定时使用 SCHED_OTHER |
| `-q` | `--quiet` | — | **静默模式**：测试过程中不刷新屏幕，仅在结束时输出一行摘要。适合重定向到文件 |
| `-S` | `--smp` | — | **SMP 模式**：自动检测 CPU 数量，为每个 CPU 创建一个测量线程，所有线程使用相同的实时优先级。等价于 `-a -t N`（N=CPU数） |
| `-t` | `--threads` | N | **线程数**：创建 N 个测量线程。不带参数时默认为 CPU 数量。`-t 1` 可使用单线程 |
| `-u` | `--unbuffered` | — | **无缓冲输出**：强制 stdout 无缓冲（`setvbuf(stdout, NULL, _IONBF, 0)`），确保输出实时刷新到串口 |
| `-v` | `--verbose` | — | **详细模式**：每个测量周期输出一行 `线程号:周期号:延迟值`。数据量大，适合后续用脚本统计分析 |

### 8.5.2 RTEMS 支持的参数（行为有差异，共 12 个）

| 短选项 | 长选项 | 原版行为 | RTEMS 版行为 | 差异原因 |
|--------|--------|----------|-------------|----------|
| `-a` | `--affinity` | 支持范围语法如 `-a 0-3`、`-a 0,2`（libnuma 解析） | 仅支持单个 CPU 号如 `-a 0`。不带参数 = 使用全部 CPU | 无 libnuma，`cpu_set_t` 不支持复杂范围语法 |
| `-A` | `--aligned` | 线程在特定微秒偏移处对齐唤醒 | **相同**，但依赖 pthread barrier（需 RTEMS 配置 barrier 支持） | RTEMS barrier 支持已验证可用 |
| `-o` | `--oscope` | 振荡器缩减：verbose 输出按比例 N 抽稀 | **相同**，但 RTEMS 上未在帮助文本中列出（功能可用） | 保持帮助文本简洁 |
| `-r` | `--relative` | 使用 `TIMER_RELTIME` 相对定时器代替 `TIMER_ABSTIME` | **相同**，但在 `MODE_SYS_NANOSLEEP` 中已默认是相对模式，该选项在 `-s` 模式下无额外影响 | nanosleep 本身就是相对睡眠 |
| `-R` | `--resolution` | 通过 `clock_getres()` + 1000 次 `clock_gettime()` 估算时钟分辨率 | **容错增强**：如果 `clock_getres()` 失败，跳过 reported resolution 输出（原版会打印未初始化的栈变量垃圾值） | 防御性修复 |
| `-s` | `--system` | 使用 `sys_nanosleep()`（tick-based 相对睡眠），测量系统原生定时器粒度 | **完整实现**：调用 RTEMS 的 `nanosleep()`（内部封装 `clock_nanosleep(CLOCK_REALTIME,...)`）。关键差异：延迟逐周期漂移（无绝对时间锚点） | RTEMS `nanosleep()` 可用，漂移特性正是该模式的测量目标 |
| `-x` | `--posix_timers` | 使用 `timer_create()` + `SIGEV_THREAD_ID` 实现高精度周期定时器 | 使用 `SIGEV_SIGNAL`（进程级信号）。若 RTEMS 未配置 `CONFIGURE_ENABLE_POSIX_API`，自动回退到 `MODE_CLOCK_NANOSLEEP` | RTEMS 不支持 `SIGEV_THREAD_ID`（Linux 内核扩展） |
| `--policy` | — | 支持 `fifo`、`rr`、`other`、`batch`、`idle` | 支持 `fifo`、`rr`、`other`（RTEMS 无 SCHED_BATCH/SCHED_IDLE） | RTEMS 调度策略集有限 |
| `--priospread` | — | 多线程时优先级逐个递减（最高线程 = `-p` 值，其他递减） | **相同**，使用相同递减逻辑 | |
| `--spike` | — | 记录所有延迟超过 TRIGGER 的尖峰 | **相同**，尖峰最大记录数由 `--spike-nodes` 控制 | |
| `--secaligned` | — | 线程在秒边界处对齐唤醒 | **相同**，依赖 pthread barrier | |
| `--help` | — | 打印帮助后 `exit(0)` | 打印帮助后设置 `help_printed=1`，`cyclictest_main` 检查该标志并 `return EXIT_SUCCESS` | RTEMS 中 `exit()` 会终止整个系统，改用返回 |

### 8.5.3 从原版移除的参数及原因（共 12 个）

| 原版短选项 | 原版长选项 | 原版功能 | 移除原因 |
|-----------|-----------|----------|----------|
| `-m` | `--mlockall` | 锁定进程所有内存页，防止 swap 导致的延迟抖动 | RTEMS 无虚拟内存/swap，所有内存始终驻留。**功能天然满足** |
| `-F` | `--fifo` | 创建命名管道（`mkfifo`），将统计信息写入管道供外部进程读取 | RTEMS 无命名管道文件系统支持。统计数据直接输出到 stdout/串口 |
| — | `--smi` | 读取 `/dev/cpu/N/msr` 中的 SMI (System Management Interrupt) 计数器 | `/dev/cpu/N/msr` 是 Linux 设备文件；SMI 是 x86 BIOS 概念。即使硬件支持，RTEMS 也无此设备接口 |
| — | `--laptop` | 笔记本模式：CPU 电源管理相关，减少因 C-state 切换导致的延迟尖峰 | RTEMS 无 CPU 电源管理、无 C-state。CPU 始终全速运行。**功能天然满足** |
| — | `--latency` | 向 `/dev/cpu_dma_latency` 写入延迟目标值，阻止 CPU 进入深度睡眠 | `/dev/cpu_dma_latency` 是 Linux PM QoS 接口。RTEMS 无此机制 |
| — | `--deepest-idle-state` | 设置 CPU 最深 idle 状态（依赖 `libcpupower` 库） | `libcpupower` 是 Linux 特有库；RTEMS 不管理 CPU idle 状态 |
| — | `--tracemark` | 向 ftrace trace_marker 写入标记，配合内核 ftrace 追踪延迟 | ftrace 是 Linux 内核追踪基础设施。RTEMS 无等价机制 |
| — | `--numa` | NUMA 感知：在最近的内存节点分配线程栈和数据结构 | RTEMS 无 NUMA 硬件抽象。所有内存在单一平面域中 |
| — | `--mainaffinity` | 设置主线程（监控循环）的 CPU 亲和性 | 功能简化：主线程已在启动时绑定到合适的 CPU。嵌入式 CPU 数量少，手动指定意义不大 |
| — | `--default-system` | 默认使用系统定时器模式（`nanosleep`）而非 `clock_nanosleep` | 功能与 `-s` 重叠。RTEMS 版始终默认 `MODE_CLOCK_NANOSLEEP`，如需 nanosleep 用 `-s` 显式指定 |
| — | `--json` | 将测试结果以 JSON 格式写入文件 | **未实现**（非不可用）。未来版本可添加，但当前嵌入式场景 stdout 输出已足够 |
| — | `--histfile` | 将直方图数据写入独立文件而非 stdout | **未实现**（非不可用）。与 `--json` 类似，stdout 输出已覆盖主要使用场景 |

### 8.5.4 参数功能分类速览

```
测量控制:
  -i 间隔  -l 循环次数  -D 运行时长  -d 线程间距

延迟分析:
  -b 断点追踪  --spike 尖峰记录  -M 最大值刷新
  -h 直方图  -H 汇总直方图  -R 时钟分辨率

调度与优先级:
  -p 优先级  --policy 调度策略  --priospread 优先级递减

线程与CPU:
  -t 线程数  -S SMP模式  -a CPU亲和性
  -A 对齐唤醒  --secaligned 秒对齐

定时器模式:
  默认 clock_nanosleep (高精度)  -r 相对定时器
  -s sys_nanosleep (漂移)  -x POSIX定时器

输出控制:
  -v 逐周期  -q 静默  -N 纳秒  -o 抽稀  -u 无缓冲
  --help 帮助
```

---

## 第9部分：process_options() (原版 L1094-1440, RTEMS L450-642)

### 9.1 option_values 枚举

```c
// 原版 — 28 个选项
enum option_values {
    OPT_AFFINITY=1, OPT_BREAKTRACE, OPT_CLOCK,
    OPT_DEFAULT_SYSTEM, OPT_DISTANCE, OPT_DURATION, OPT_LATENCY,
    OPT_FIFO, OPT_HISTOGRAM, OPT_HISTOFALL, OPT_HISTFILE,
    OPT_INTERVAL, OPT_JSON, OPT_MAINAFFINITY, OPT_LOOPS, OPT_MLOCKALL,
    OPT_REFRESH, OPT_NANOSLEEP, OPT_NSECS, OPT_OSCOPE, OPT_PRIORITY,
    OPT_QUIET, OPT_PRIOSPREAD, OPT_RELATIVE, OPT_RESOLUTION,
    OPT_SYSTEM, OPT_SMP, OPT_THREADS, OPT_TRIGGER,
    OPT_TRIGGER_NODES, OPT_UNBUFFERED, OPT_NUMA, OPT_VERBOSE,
    OPT_DBGCYCLIC, OPT_POLICY, OPT_HELP, OPT_NUMOPTS,
    OPT_ALIGNED, OPT_SECALIGNED, OPT_LAPTOP, OPT_SMI,
    OPT_TRACEMARK, OPT_POSIX_TIMERS, OPT_DEEPEST_IDLE_STATE,
};

// RTEMS版 — 移除 12 个选项
enum option_values {
    OPT_AFFINITY=1, OPT_BREAKTRACE, OPT_CLOCK,
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
```

移除的 12 个选项：`OPT_DEFAULT_SYSTEM`, `OPT_LATENCY`, `OPT_FIFO`, `OPT_HISTFILE`, `OPT_JSON`, `OPT_MAINAFFINITY`, `OPT_MLOCKALL`, `OPT_NUMA`, `OPT_LAPTOP`, `OPT_SMI`, `OPT_TRACEMARK`, `OPT_DEEPEST_IDLE_STATE`

### 9.2 long_options 数组

```c
// 原版 — 35 个长选项
// RTEMS版 — 22 个长选项
```

对应移除了 13 个不可用的长选项。

### 9.3 getopt_long 短选项字符串

```c
// 原版
"a::A::b:c:d:D:F:h:H:i:l:MNo:p:mqrRsSt::uvx"

// RTEMS版
"a::A::b:c:d:D:h:H:i:l:MNo:p:qrRsSt::uvx"
//                        ^ 移除了 'm'（mlockall）
```

### 9.4 选项解析差异

#### -a (affinity) — 最显著的简化

```c
// 原版 L1155-1182 (~28 行)
option_affinity = 1;
if (smp) break;
numa = numa_initialize();                    // libnuma 初始化
if (optarg) {
    parse_cpumask(optarg, max_cpus, &affinity_mask);  // libnuma 解析范围语法
    setaffinity = AFFINITY_SPECIFIED;
} else if (optind < argc && (atoi(argv[optind]) || ...)) {
    parse_cpumask(argv[optind], max_cpus, &affinity_mask);
    setaffinity = AFFINITY_SPECIFIED;
} else {
    setaffinity = AFFINITY_USEALL;
}
if (verbose && affinity_mask)
    printf("Using %u cpus.\n", numa_bitmask_weight(affinity_mask));

// RTEMS版 L497-509 (~11 行)
option_affinity = 1;
if (smp) break;
if (optarg) {
    int cpu = atoi(optarg);                  // 简单的 atoi（不支持范围）
    affinity_mask = calloc(1, sizeof(cpu_set_t));
    CPU_ZERO(affinity_mask);
    CPU_SET(cpu, affinity_mask);
    setaffinity = AFFINITY_SPECIFIED;
} else {
    setaffinity = AFFINITY_USEALL;
}
```

**差异**：
- 原版使用 `parse_cpumask()`（libnuma）解析复杂的 CPU 范围语法（如 `"3-5,0"`）
- RTEMS 版仅支持单个 CPU 编号
- 去掉了从 `argv[optind]` 读取参数的 fallback 逻辑

### 9.5 参数验证

```c
// 原版 — 严格的错误处理
if (smi) {
    if (setaffinity == AFFINITY_UNSPECIFIED)
        fatal("SMI counter relies on thread affinity\n");
    if (!has_smi_counter())
        fatal("SMI counter is not supported on this processor\n");
}
if (clocksel < 0 || clocksel > ARRAY_SIZE(clocksources))
    error = 1;
if (oscope_reduction < 1)
    error = 1;
if (oscope_reduction > 1 && !verbose) {
    warn("-o option only meaningful, if verbose\n");
    error = 1;
}
if (histogram < 0)
    error = 1;
if (priority < 0 || priority > 99)
    error = 1;
if (num_threads == -1)
    num_threads = get_available_cpus(affinity_mask);
if (aligned && secaligned)
    error = 1;
if (error) { display_help(1); }   // 错误时 exit

// RTEMS版 — 容错处理
if (clocksel < 0 || clocksel > 1)    clocksel = 0;        // fallback 而非 error
if (histogram < 0)                   histogram = 0;        // fallback 而非 error
if (histogram > HIST_MAX)            histogram = HIST_MAX;
if (distance == -1)                  distance = DEFAULT_DISTANCE;
if (priority < 0 || priority > 99)   priority = 0;         // fallback 而非 error
if (num_threads == -1)               num_threads = get_available_cpus();
if (num_threads < 1)                 num_threads = 1;      // 最小 1 个线程
if (aligned && secaligned) {
    aligned = secaligned = 0;                               // 静默禁用而非 error
}
// 不再调用 exit()
```

**差异**：RTEMS 版采用"容错回退"策略（不合法值 → 默认值），而非原版的"严格错误退出"。这是因为 RTEMS 测试中优雅降级比崩溃退出更好。

另外新增了两项参数兼容性处理：
- `MODE_SYS_OFFSET` (`-s` flag)：打印 `NOTE: sys_nanosleep (tick-based, relative, drifting)`，不再强制回退。`MODE_SYS_NANOSLEEP` 现由 `timerthread()` 完整实现。
- `help_printed` 标志：`--help` 选项设置此标志，`cyclictest_main` 在 `process_options` 返回后检查并直接 `return EXIT_SUCCESS`，不再执行测试。

---

## 第10部分：sighand() (原版 L1452-1487, RTEMS L363-381)

```c
// 原版 — 支持 SIGUSR1/SIGUSR2 状态转储
static void sighand(int sig)
{
    if (sig == SIGUSR1) {
        // 打印当前延迟统计到 stderr
        for (i = 0; i < num_threads; i++)
            print_stat(stderr, parameters[i], i, 0, 0);
        return;
    } else if (sig == SIGUSR2) {
        // 写入共享内存状态
        for (i = 0; i < num_threads; i++)
            rstat_print_stat(parameters[i], i, 0, 0);
        return;
    }
    shutdown = 1;
}

// RTEMS版 — 简化信号处理
void sighand(int sig)
{
    if (sig == SIGUSR1) {
        // 仅打印提示
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
```

**差异**：
1. `SIGUSR2` 处理移除（依赖已移除的 `rstat`）
2. `SIGUSR1` 只打印标题不再打印各线程状态（因为 `sighand` 中访问 `parameters[i]` 有线程安全风险）
3. `sighand` 从 `static` 改为非 `static`（`init.c` 需要引用）

---

## 第11部分：print_stat() (原版 L1574-1627, RTEMS L709-748)

```c
// 原版
static void print_stat(FILE *fp, struct thread_param *par, int index,
                        int verbose, int quiet)
{
    // ...
    if (!verbose) {
        fprintf(fp, fmt, ...);
        if (smi)
            fprintf(fp, " SMI:%8ld", stat->smi_count);  // SMI 计数
        fprintf(fp, "\n");
    } else {
        while (...) {
            if (smi) diff_smi = stat->smis[...];       // SMI 值输出
            if (!smi)
                fprintf(fp, "%8d:%8lu:%8ld\n", ...);
            else
                fprintf(fp, "%8d:%8lu:%8ld%8ld\n", ...); // 带 SMI 输出
        }
    }
}

// RTEMS版
void print_stat(FILE *fp, struct thread_param *par, int index,
                int verbose, int quiet)
{
    // ...
    if (!verbose) {
        fprintf(fp, fmt, ...);     // 无 SMI 字段
    } else {
        while (...) {
            fprintf(fp, "%8d:%8lu:%8ld\r\n", ...);  // 始终不带 SMI
        }
    }
    // 注意: 行尾加了 \r\n (CRLF) 适用于串口终端
}
```

**差异**：
1. 移除 SMI 相关输出
2. 行尾使用 `\r\n` 而不是 `\n`（适配串口终端，`\r` 将光标移到行首，覆盖旧输出）
3. 从 `static` 改为非 `static`

---

## 第12部分：print_hist() (原版 L1499-1572, RTEMS L752-807)

```c
// 原版
static void print_hist(struct thread_param *par[], int nthreads)
{
    FILE *fd;
    if (use_histfile) {
        fd = fopen(histfile, "w");    // 可写入文件
    } else {
        fd = stdout;
    }
    // ... 输出直方图 ...
    if (smi) {
        fprintf(fd, "# SMIs:");
        for (i = 0; i < nthreads; i++)
            fprintf(fd, " %05lu", par[i]->stats->smi_count);
    }
    if (use_histfile) fclose(fd);
}

// RTEMS版
void print_hist(struct thread_param *par[], int nthreads)
{
    FILE *fd = stdout;              // 始终输出到 stdout
    // ... 输出直方图 ...
    // 无 SMI，无 histfile
}
```

**差异**：
1. 移除 `use_histfile` 支持（始终输出到 stdout）
2. 移除 SMI 统计输出
3. 从 `static` 改为非 `static`

---

## 第13部分：Main 函数 (原版 main() L1910-2362, RTEMS cyclictest_main() L1246-1520)

### 13.1 函数签名

```c
// 原版
int main(int argc, char **argv)    // 标准 C main (进程入口)

// RTEMS版
int cyclictest_main(int argc, char *argv[])  // 被 init.c 调用的函数
```

**差异**：RTEMS 不是独立程序，是 RTEMS 测试框架中的一个测试用例。`main()` 由 RTEMS 内核控制（在 `init.c` 中定义为 `Init` 任务），cyclictest 作为子函数被调用。

### 13.2 初始化阶段

```c
// 原版 L1920-1976
rt_init(argc, argv);                    // 保存命令行、时间戳
process_options(argc, argv, max_cpus);

if (check_privs())                      // 检查是否有实时权限
    exit(EXIT_FAILURE);

if (verbose) {
    printf("Max CPUs = %d\n", max_cpus);
    printf("Online CPUs = %d\n", online_cpus);
}

// 主线程亲和性
if (affinity_mask != NULL) {
    set_main_thread_affinity(affinity_mask);  // libnuma
}

// 锁定所有内存
if (lockall)
    mlockall(MCL_CURRENT|MCL_FUTURE);

// 设置延迟目标
set_latency_target();

// 设置 CPU idle 状态
if (deepest_idle_state >= -1) {
    save_cpu_idle_disable_state(i);
    set_deepest_cpu_idle_state(i, deepest_idle_state);
}

// 启用 ftrace 标记
if (tracelimit && trace_marker)
    enable_trace_mark();

// RTEMS版 L1252-1295
process_options(argc, argv, max_cpus);   // 直接解析选项

/* --help 已打印，直接返回（不执行测试） */
if (help_printed)
    return EXIT_SUCCESS;

if (verbose)
    printf("CPUs: %d\n", max_cpus);       // 简化输出

// check_privs() 移除
// mlockall 移除
// set_latency_target 移除
// deepest_idle_state 移除
// enable_trace_mark 移除

// clock_getres 失败修复：如果 clock_getres() 失败，
// 使用 res_ok 标志跳过 reported clock resolution 输出，
// 避免打印未初始化的栈变量（垃圾值）
```

### 13.3 线程创建

```c
// 原版 L2100-2220
for (i = 0; i < num_threads; i++) {
    pthread_attr_init(&attr);

    // CPU 选择
    switch (setaffinity) {
    case AFFINITY_SPECIFIED:
        cpu = cpu_for_thread_sp(i, max_cpus, affinity_mask);  // libnuma 版本
    case AFFINITY_USEALL:
        cpu = cpu_for_thread_ua(i, max_cpus);
    }

    // NUMA 内存分配（如果启用）
    if (numa) {
        node = rt_numa_numa_node_of_cpu(node_cpu);
        stack = rt_numa_numa_alloc_onnode(stksize, node);
        pthread_attr_setstack(&attr, stack, stksize);         // 自定义栈
    }

    // 用 threadalloc 分配（支持 NUMA 节点）
    parameters[i] = threadalloc(sizeof(struct thread_param), node);
    statistics[i]  = threadalloc(sizeof(struct thread_stat), node);

    // 调度策略设置
    par->prio = priority;
    if (priority && (policy == SCHED_FIFO || policy == SCHED_RR))
        par->policy = policy;

    pthread_create(&stat->thread, &attr, timerthread, par);
}

// RTEMS版 L1315-1407
for (i = 0; i < num_threads; i++) {
    pthread_attr_init(&attr);

    // RTEMS 特有：通过 pthread_attr 设置调度策略
    if (priority && (policy == SCHED_FIFO || policy == SCHED_RR)) {
        struct sched_param sparam;
        sparam.sched_priority = priority;
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, policy);
        pthread_attr_setschedparam(&attr, &sparam);
    }

    // CPU 选择（简化版 cpu_set_t）
    switch (setaffinity) {
    case AFFINITY_SPECIFIED:
        cpu = cpu_for_thread_sp(i, max_cpus, affinity_mask);  // 简化版
    case AFFINITY_USEALL:
        cpu = cpu_for_thread_ua(i, max_cpus);
    }

    // 直接用 calloc 分配（无 NUMA）
    parameters[i] = calloc(1, sizeof(struct thread_param));
    statistics[i]  = calloc(1, sizeof(struct thread_stat));

    // 调度策略设置（相同）
    par->prio = priority;
    if (priority && (policy == SCHED_FIFO || policy == SCHED_RR))
        par->policy = policy;

    pthread_create(&stat->thread, &attr, timerthread, par);
}
```

**差异**：

| 项目 | 原版 | RTEMS版 |
|------|------|---------|
| 调度策略设置 | `sched_setscheduler()` 在线程内调用 | `pthread_attr_setschedpolicy()` 在线程创建时设置 |
| 内存分配 | `threadalloc()` → NUMA-node 感知 malloc | 直接 `calloc()` |
| 自定义栈 | NUMA 模式下设置自定义栈 | 不使用自定义栈 |
| `threadfree()` | NUMA-node 感知 free | 直接 `free()` |

**为什么 RTEMS 通过 pthread_attr 而非 sched_setscheduler 设置优先级**：
- RTEMS 内核的 `sched_setscheduler()` 返回 `ENOSYS`（不支持运行时改变调度策略）
- RTEMS 使用任务创建时的静态调度策略分配
- `pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED)` 告诉 pthread 库使用属性中指定的调度参数，而非继承父线程

### 13.4 主监控循环

```c
// 原版 L2231-2277
while (!shutdown) {
    // 读取 /proc/loadavg
    fd = open("/proc/loadavg", O_RDONLY, 0666);
    len = read(fd, &lavg, 255);
    printf("policy: %s: loadavg: %s\n", policystr, lavg);

    for (i = 0; i < num_threads; i++) {
        print_stat(stdout, parameters[i], i, verbose, quiet);
        if (max_cycles && statistics[i]->cycles >= max_cycles)
            allstopped++;
    }
    usleep(10000);     // 10ms
    if (!verbose && !quiet)
        printf("\033[%dA", num_threads + 2);  // ANSI 光标上移
}

// RTEMS版 L1410-1435
while (!shutdown) {
    printf("policy: %s: loadavg: N/A\r\n\r\n", policystr);  // loadavg 显示 N/A

    for (i = 0; i < num_threads; i++) {
        print_stat(stdout, parameters[i], i, verbose, quiet);
        if (max_cycles && statistics[i]->cycles >= max_cycles)
            allstopped++;
    }
    usleep(100000);    // 100ms（RTEMS 上 tick 粒度较粗）
    if (!verbose && !quiet)
        printf("\033[%dA", num_threads + 2);
}
```

**差异**：

| 项目 | 原版 | RTEMS版 | 原因 |
|------|------|---------|------|
| loadavg | 读取 `/proc/loadavg` | 显示 `"N/A"` | RTEMS 无 `/proc` 文件系统 |
| 休眠间隔 | 10ms | 100ms | RTEMS 的 `usleep` 粒度受 `CONFIGURE_MICROSECONDS_PER_TICK` (1000us) 限制，过短的 sleep 会 busy-wait |
| 行尾 | `\n` | `\r\n` | 适配串口终端 |

### 13.5 清理 & 退出

```c
// 原版 L2280-2362
// 线程终止
pthread_kill(statistics[i]->thread, SIGTERM);
pthread_join(statistics[i]->thread, NULL);
threadfree(statistics[i]->values, ...);

// ftrace 清理
disable_trace_mark();

// 解锁内存
munlockall();

// 关闭延迟目标 fd
close(latency_target_fd);

// 恢复 CPU idle 状态
restore_cpu_idle_disable_state(i);

// 释放 NUMA 位掩码
rt_bitmask_free(affinity_mask);

// 删除共享内存
shm_unlink(shm_name);

// RTEMS版 L1439-1487
// 线程终止
pthread_cancel(statistics[i]->thread);     // RTEMS 用 cancel 而非 kill
pthread_join(statistics[i]->thread, NULL);
free(statistics[i]->values);

// 简单释放
free(statistics[i]);
free(parameters[i]);
free(statistics);
free(parameters);
free(affinity_mask);
hset_destroy(&hset);
return ret;
```

**差异**：

| 项目 | 原版 | RTEMS版 | 原因 |
|------|------|---------|------|
| 线程终止 | `pthread_kill(SIGTERM)` | `pthread_cancel()` | RTEMS 信号支持有限，`pthread_cancel` 更可靠 |
| 内存释放 | `threadfree()` (NUMA-aware) | `free()` | 无 NUMA |
| ftrace 清理 | `disable_trace_mark()` | 移除 | 无 ftrace |
| 内存解锁 | `munlockall()` | 移除 | 无 mlock |
| idle 恢复 | `restore_cpu_idle_*` | 移除 | 无 cpuidle |
| 共享内存 | `shm_unlink()` | 移除 | 无 shm |
| 返回值 | `exit(ret)` | `return ret` | RTEMS 中不能调用 `exit()`（整个系统会退出！） |

**`exit()` vs `return` 是 RTEMS 移植中的关键点**：
- 原版调用 `exit(ret)` 终止进程
- RTEMS 中 `exit()` 会终止整个 RTEMS 内核运行
- RTEMS 版改为 `return ret`，由调用者（`init.c` 中的 `cyclictest_task`）决定后续行为

---

## 第14部分：init.c — 全新的 RTEMS 入口文件

`init.c` (522 行) 在 RTEMS 版中是全新的，没有原版对应文件。

### 14.1 两种运行模式

```c
#define USE_SHELL  1   /* 1 = interactive shell, 0 = auto-test */
```

| 模式 | 行为 | 用途 |
|------|------|------|
| `USE_SHELL=0` | 自动测试：启动后直接运行 cyclictest 并退出 | CI/CD、自动化测试 |
| `USE_SHELL=1` | 交互 Shell：提供命令提示符，可多次运行 cyclictest | 开发调试、手动测试 |

### 14.2 自动测试模式

```c
static rtems_task Init(rtems_task_argument ignored)
{
    char *argv[] = { "cyclictest", "-l", "500", NULL };
    cyclictest_main(argc, argv);
    TEST_END();
    rtems_test_exit(0);
}
```

直接运行 500 个周期的 cyclictest，然后退出。

### 14.3 交互 Shell 模式

```c
// 轮询 UART 输入，每次读取一个字符
static int uart_getchar(void)
{
    if (inport_byte(COM1_BASE_IO + UART_LSR) & LSR_DR)
        return inport_byte(COM1_BASE_IO + UART_RBR);
    return -1;  // 无数据
}

// 主循环
while (1) {
    printf("cyclictest> ");
    // 逐字符读取，支持退格删除
    // 解析命令行
    // 执行命令
    run_command(argc, argv);
}
```

**关键设计**：
- **轮询式 UART 读取**：因为 `CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER` 不支持阻塞 `read()`
- **无缓冲 I/O**：`setvbuf(stdin, NULL, _IONBF, 0)` 确保 `getchar()` 直接从 UART 读取
- **自定义命令解析**：简单的空格分割，不依赖 RTEMS shell 的行编辑功能（termios 可能不可用）

### 14.4 cyclictest 作为独立任务运行

```c
if (strcmp(argv[0], "cyclictest") == 0) {
    rtems_task_create(
        rtems_build_name('C', 'Y', 'C', 'T'),
        50,                              // 优先级 50
        RTEMS_MINIMUM_STACK_SIZE * 32,   // 栈大小
        RTEMS_DEFAULT_MODES,
        RTEMS_DEFAULT_ATTRIBUTES,
        &tid
    );
    rtems_task_start(tid, cyclictest_task, (rtems_task_argument)&cyc);

    // 等待完成（最多 60 秒）
    for (int w = 0; w < 600 && !done; w++)
        rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 10);
}
```

**原因**：
- cyclictest 内部创建多个 POSIX 线程进行测量
- 将这些线程放在一个独立的任务中，便于资源隔离和清理
- 使用 `rtems_task_wake_after` 代替忙等，让出 CPU 给 cyclictest 的测量线程

### 14.5 RTEMS 配置

```c
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER

#define CONFIGURE_MAXIMUM_TASKS              50
#define CONFIGURE_MAXIMUM_POSIX_THREADS      10
#define CONFIGURE_MAXIMUM_POSIX_MUTEXES      10
#define CONFIGURE_MAXIMUM_POSIX_CONDITION_VARIABLES  5
#define CONFIGURE_MAXIMUM_POSIX_TIMERS        0    // <-- POSIX定时器关闭！
#define CONFIGURE_MAXIMUM_POSIX_BARRIERS      5

#define CONFIGURE_MICROSECONDS_PER_TICK    1000       // 1ms tick
```

**关键配置**：
- `CONFIGURE_MAXIMUM_POSIX_TIMERS 0`：不配置 POSIX 定时器 → 触发 `timerthread()` 中的 `#else` 分支，回退到 `MODE_CLOCK_NANOSLEEP`
- `CONFIGURE_MICROSECONDS_PER_TICK 1000`：1ms 系统 tick → `usleep()` 的最小粒度
- `SIMPLE_CONSOLE_DRIVER`：使用轮询式简单控制台驱动

### 14.6 Shell 命令

```c
#define CONFIGURE_SHELL_COMMANDS_INIT
#define CONFIGURE_SHELL_COMMANDS_HELP
#define CONFIGURE_SHELL_COMMANDS_CMDLS
// ... 等 Shell 命令
```

这些配置使得 RTEMS Shell 基础设施可用（通过 `shell` 命令启动）。

---

## 第15部分：histogram.c/h (几乎不变)

### histogram.h

| 项目 | 原版 | RTEMS版 | 差异 |
|------|------|---------|------|
| 许可证 | GPL-2.0-or-later | BSD-2-Clause | 许可证变更 |
| 头注释 | 无 | 有（标注来源） | |
| `struct histogram` | 完全相同 | 完全相同 | |
| `struct histoset` | 完全相同 | 完全相同 | |
| 函数声明 | 完全相同 | 完全相同 | |
| 宏定义 | 相同 | 相同 | |

### histogram.c

| 项目 | 原版 (181行) | RTEMS版 (181行) | 差异 |
|------|-------------|----------------|------|
| 许可证 | GPL-2.0-or-later | BSD-2-Clause | 许可证变更 |
| 头注释 | 有 | 有（修改） | |
| 缩进风格 | Tab 缩进 | 2空格缩进 | 风格统一 |
| 所有函数逻辑 | 完全相同 | 完全相同 | 纯C数学，无平台依赖 |

histogram.c 是唯一逻辑完全不变的文件——纯数据结构的 bucket/overflow 统计，不涉及任何系统调用，在 Linux 和 RTEMS 上完全一致。

---

## 第16部分：新增/内联的函数

### 16.1 warn() / fatal() — 替代 rt-error.h
```c
// 原版：rt-error.h/rt-error.c 提供（需要 errno 处理、彩色输出等）
// RTEMS版：内联简化版本
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
```

注意 `fatal()` 不调用 `exit()`（RTEMS 中 `exit()` 会终止整个系统），而是返回 `EXIT_FAILURE`。

### 16.2 tsnorm / tsgreater / calcdiff / calcdiff_ns / calctime
```c
// 原版：定义在 rt-utils.h 中
// RTEMS版：直接内联到 cyclictest.c
// 实现完全相同
```

### 16.3 get_available_cpus()
```c
// 原版：rt-numa.c/libnuma
// RTEMS版：
static int get_available_cpus(void)
{
#ifdef __rtems__
    return (int)rtems_scheduler_get_processor_maximum() + 1;
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}
```

### 16.4 cpu_for_thread_ua() / cpu_for_thread_sp()
```c
// 原版：rt-numa.c，使用 struct bitmask + libnuma
// RTEMS版：直接内联，使用 cpu_set_t + 标准宏
static int cpu_for_thread_sp(int i, int max_cpus, cpu_set_t *mask)
{
    int cpu, count = 0;
    for (cpu = 0; cpu < max_cpus; cpu++)
        if (CPU_ISSET(cpu, mask)) count++;
    if (count == 0) return -1;
    count = i % count;
    for (cpu = 0; cpu < max_cpus; cpu++)
        if (CPU_ISSET(cpu, mask)) {
            if (count == 0) return cpu;
            count--;
        }
    return -1;
}
```

### 16.5 threadalloc() / threadfree()
```c
// 原版：NUMA-aware 分配（rt-numa.c）
// RTEMS版：直接 calloc/free
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
```

### 16.6 rt_init() / rt_write_json()
```c
// 原版：rt-utils.c，功能完整（记录命令行、时间戳、uname 等）
// RTEMS版：简化为最小实现
void rt_init(int argc, char *argv[])
{
    /* Initialize any global state (minimal for RTEMS) */
}

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
```

原版的 `rt_write_json` 依赖 `uname()`、`/sys/kernel/realtime` 等，RTEMS 上全部不可用，简化为只输出线程统计数据。

---

## 总结

### 设计原则

1. **最小侵入**：尽可能保持原版代码结构不变，用 `#ifdef __rtems__` 条件编译 + `#define` 死代码消除替代大规模重构
2. **渐进退化**：不支持的选项不回退为错误，而是回退为默认行为
3. **自注释**：移除的特性都有明确的注释说明原因（header 注释 + inline 注释）
4. **新增不污染**：`init.c` 和 `cyclictest.h` 作为独立文件承载 RTEMS 特有的入口和声明
5. **防御性修复**：线程退出时无条件设置 `shutdown` 防止主循环死等；`--help` 通过标志位提前返回而非继续执行测试

### 代码量对比

| 类别 | 行数变化 | 说明 |
|------|----------|------|
| 移除的 Linux 特性代码 | ~-600 行 | NUMA/SMI/cpuidle/ftrace/rstat/fifo/rlimit/laptop |
| 移除的库依赖代码 | ~-400 行 | rt-utils, rt-numa, rt-error, rt-sched, rt-get_cpu |
| 内联的替代代码 | ~+150 行 | warn/fatal/tsnorm/calcdiff/亲和性/threadalloc |
| 新增 TSC 支持 | ~+40 行 | rdtsc + 校准逻辑 |
| 新增 MODE_SYS_NANOSLEEP | ~+30 行 | nanosleep() tick-based 相对睡眠模式 |
| 新增 init.c | +522 行 | RTEMS 入口、Shell、任务配置（含 Shell 命令宏配置） |
| 新增 cyclictest.h | +199 行 | 统一声明、条件编译 gettid() |
| Bug 修复 | ~+15 行 | --help 退出、-R 垃圾值保护、shutdown 无条件置位 |

这代表了将一个深度依赖 Linux 内核特性（12+ 个内核接口）的实时测试程序成功移植到一个裸机实时操作系统的完整过程。
