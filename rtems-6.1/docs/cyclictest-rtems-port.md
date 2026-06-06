# RTEMS Cyclictest 移植设计文档

## 一、移植概述

将 Linux rt-tests-2.10 中的 cyclictest 实时延迟测量工具移植到 RTEMS 6.1（amd64 BSP）。

**原始源码:** `rt-tests-2.10/src/cyclictest/cyclictest.c`（2362行，单文件）

**移植后:** 5个文件，合计 2184 行

```
rtems-6.1/testsuites/samples/cyclictest/
├── init.c          # RTEMS 入口+配置+极简交互Shell   ( 317行)
├── cyclictest.h    # 数据结构+常量+函数声明          ( 199行)
├── cyclictest.c    # 核心算法+参数解析+统计输出      (1487行)
├── histogram.h     # 直方图数据结构                  (  50行)
└── histogram.c     # 直方图实现（纯C数学）           ( 181行)
                              ────────
               合计: 2234行 (含histogram.h) / 2184行 (不含)
```

---

## 二、参数对照：原版 vs RTEMS

原版 cyclictest 共 **41** 个参数。RTEMS 移植后 **29** 个可用，**12** 个移除。

### 2.1 全部41个参数逐项对照

| # | 参数 | 类型 | 原版 | RTEMS | 说明 |
|---|------|------|:----:|:-----:|------|
| 1 | `-a, --affinity` | 可选参数 | ✅ | ⚠️ | pthread_setaffinity_np可能为stub |
| 2 | `-A, --aligned` | 可选参数 | ✅ | ✅ | |
| 3 | `-b, --breaktrace` | 必选参数 | ✅ | ✅ | TSC模式阈值需换算 |
| 4 | `-c, --clock` | 必选参数 | ✅ | ✅ | 0=MONO, 1=REAL |
| 5 | `--default-system` | 无参数 | ✅ | ❌ | 不抑制电源管理(无意义) |
| 6 | `-d, --distance` | 必选参数 | ✅ | ✅ | |
| 7 | `-D, --duration` | 必选参数 | ✅ | ✅ | |
| 8 | `--latency` | 必选参数 | ✅ | ❌ | /dev/cpu_dma_latency不存在 |
| 9 | `-F, --fifo` | 必选参数 | ✅ | ❌ | mkfifo命名管道不可用 |
| 10 | `-h, --histogram` | 必选参数 | ✅ | ✅ | |
| 11 | `-H, --histofall` | 必选参数 | ✅ | ✅ | |
| 12 | `--histfile` | 必选参数 | ✅ | 🔜 | 待实现 |
| 13 | `-i, --interval` | 必选参数 | ✅ | ✅ | |
| 14 | `--json` | 必选参数 | ✅ | 🔜 | 待实现 |
| 15 | `--laptop` | 无参数 | ✅ | ❌ | 无电源管理 |
| 16 | `-l, --loops` | 必选参数 | ✅ | ✅ | |
| 17 | `--mainaffinity` | 必选参数 | ✅ | ❌ | 已合并到-a |
| 18 | `-m, --mlockall` | 无参数 | ✅ | ❌ | 无虚拟内存/swap |
| 19 | `-M, --refresh_on_max` | 无参数 | ✅ | ✅ | |
| 20 | `-N, --nsecs` | 无参数 | ✅ | ✅ | |
| 21 | `-o, --oscope` | 必选参数 | ✅ | ✅ | |
| 22 | `-p, --priority` | 必选参数 | ✅ | ✅ | 通过pthread_attr预设 |
| 23 | `-q, --quiet` | 无参数 | ✅ | ✅ | |
| 24 | `--priospread` | 无参数 | ✅ | ✅ | |
| 25 | `-r, --relative` | 无参数 | ✅ | ✅ | |
| 26 | `-R, --resolution` | 无参数 | ✅ | ✅ | clock_getres会失败(BSP限制) |
| 27 | `--secaligned` | 可选参数 | ✅ | ✅ | |
| 28 | `-s, --system` | 无参数 | ✅ | ✅ | |
| 29 | `--smi` | 无参数 | ✅ | ❌ | /dev/cpu/msr不存在 |
| 30 | `-S, --smp` | 无参数 | ✅ | ✅ | |
| 31 | `--spike` | 必选参数 | ✅ | ✅ | |
| 32 | `--spike-nodes` | 必选参数 | ✅ | ✅ | |
| 33 | `-t, --threads` | 可选参数 | ✅ | ✅ | 需用-tN格式(无空格) |
| 34 | `--tracemark` | 无参数 | ✅ | ❌ | 无ftrace |
| 35 | `-u, --unbuffered` | 无参数 | ✅ | ✅ | |
| 36 | `-v, --verbose` | 无参数 | ✅ | ✅ | |
| 37 | `--dbg_cyclictest` | 无参数 | ✅ | ✅ | 调试用(保留) |
| 38 | `--policy` | 必选参数 | ✅ | ✅ | fifo/rr/other |
| 39 | `--help` | 无参数 | ✅ | ✅ | |
| 40 | `-x, --posix_timers` | 无参数 | ✅ | ⚠️ | POSIX timer未启用,自动降级 |
| 41 | `--deepest-idle-state` | 必选参数 | ✅ | ❌ | 无cpuidle子系统 |

### 2.2 统计汇总

```
原版 41 个
  ├── ✅ 保留可用   29 个 (含 -a ⚠️受限制, -x ⚠️自动降级)
  ├── ❌ 已移除     10 个
  └── 🔜 待实现      2 个 (--json, --histfile)
```

**已移除的10个及原因：**

| # | 参数 | 移除原因 |
|---|------|---------|
| 5 | `--default-system` | RTEMS无电源管理系统，不需要"不抑制" |
| 8 | `--latency` | `/dev/cpu_dma_latency` Linux PM QoS接口 |
| 9 | `-F/--fifo` | `mkfifo()` 命名管道IPC不可用 |
| 15 | `--laptop` | RTEMS无CPU C-state/电池管理 |
| 17 | `--mainaffinity` | 功能已由 `-a` 覆盖 |
| 18 | `-m/--mlockall` | RTEMS无虚拟内存/swap, 物理内存常驻 |
| 29 | `--smi` | `/dev/cpu/*/msr` Linux专有MSR设备 |
| 34 | `--tracemark` | Linux ftrace内核追踪 |
| 41 | `--deepest-idle-state` | Linux cpuidle子系统 |

### 2.3 新增的RTEMS专有参数

| 宏 | 功能 | 值 |
|----|------|-----|
| `USE_TSC` | 测量时钟选择 | 1=硬件rdtsc(~0.3ns), 0=clock_gettime(1ms) |
| `USE_SHELL` | 运行模式 | 1=交互Shell, 0=自动测试 |

---

## 三、代码行数分析：少了什么、多了什么

### 3.1 原版的真实代码量

原版 `cyclictest.c`（2362行）**本身就是核心算法文件**，所有测量逻辑都在这一个文件里。另外引用的 `rt-utils.c`（565行）和 `rt-error.c`（101行）是多工具共享库，cyclictest 实际只用其中少量内联函数（tsnorm/calcdiff/gettid/parse_time_string/warn/fatal ~70行），这些我们都用内联方式重写了。

```
原版 cyclictest 实际有效代码:
  cyclictest.c (全部)      2362 行  (核心算法，含Linux专有功能)
  histogram.c               181 行  (直方图，独立库文件)
  内联工具函数              ~50 行  (来自 rt-utils.h, rt-error.h)
  ─────────────────────────
  合计:                   ~2593 行
```

### 3.2 算法核心公平对比

去掉 Linux 专有代码后，原版和移植的**算法核心行数几乎一样**：

| | 原版 | RTEMS 移植 |
|---|------|-----------|
| 算法+数据结构 | `cyclictest.c - 655行删除 + histogram.c` | `cyclictest.c + cyclictest.h + histogram.c` |
| | = 2362 - 655 + 181 = **1888 行** | = 1487 + 199 + 181 = **1867 行** |
| 入口+Shell | — (由Linux OS提供) | `init.c` = **317 行** (纯新增) |
| **合计** | **1888 行** (仅算法) | **2184 行** (算法+RTEMS适配) |

```
原版核心 ↔ 移植核心: 1888 vs 1867 = 差异仅 21 行, 几乎一样
原版含库 ↔ 移植总计: 2593 vs 2184 = 少 409 行
  (因为删除的Linux代码655行 > 新增RTEMS适配477行)
```

### 3.3 删除的代码（~655行）

| 删除的功能 | 行数 | 具体函数/代码块 |
|-----------|------|----------------|
| 电源管理 | ~250行 | `set_latency_target()`, `save_cpu_idle_disable_state()`, `restore_cpu_idle_disable_state()`, `free_cpu_idle_disable_states()`, `set_deepest_cpu_idle_state()`, `have_libcpupower_support()` |
| NUMA内存分配 | ~100行 | `rt_numa_set_numa_run_on_node()`, `rt_numa_numa_alloc_onnode()`, `rt_numa_numa_node_of_cpu()`, main中NUMA初始化+stack分配 |
| SMI计数器 | ~80行 | `open_msr_file()`, `get_msr()`, `get_smi_counter()`, `has_smi_counter()` + `ARCH_HAS_SMI_COUNTER`条件分支 |
| 共享内存(rstat) | ~80行 | `rstat_shm_open()`, `rstat_ftruncate()`, `rstat_mmap()`, `rstat_mlock()`, `rstat_setup()` |
| MODE_SYS_ITIMER/NANOSLEEP | ~50行 | timerthread中两个未实现定时器模式的分支 |
| RLIMIT_RTPRIO | ~45行 | `raise_soft_prio()` + `getrlimit/setrlimit()` |
| FIFO输出 | ~30行 | `fifothread()` + `mkfifo()` |
| ftrace/tracemark | ~15行 | `enable_trace_mark()`, `tracemark()`, `tracing_stop()`, `disable_trace_mark()` |
| /proc/loadavg | ~5行 | `open/read("/proc/loadavg")` |

### 3.4 新增的代码（~477行）

| 新增的功能 | 行数 | 说明 |
|-----------|------|------|
| init.c (RTEMS入口+Shell+配置) | +317行 | Init task, UART极简交互Shell, CONFIGURE_xxx宏 |
| TSC硬件测量 | +50行 | `rdtsc()`, TSC校准, `USE_TSC`条件编译分支 |
| 移植注释 | +100行 | 每个删除/修改处标注原因和RTEMS限制 |
| OTHER | +10行 | `#ifdef __rtems__` 条件编译守卫 |

### 3.5 平衡计算

```
 2362 行  (原版 cyclictest.c 单文件)
-  655 行  (删除的Linux专有功能)
+  477 行  (新增RTEMS适配代码)
───────
= 2184 行  (RTEMS移植 4文件合计) ✅
```

### 3.6 为什么1487 ≠ 2362-655？

cyclictest.c 单独看是 1487 行，但数据结构（~200行）提取到了 cyclictest.h，直方图（~180行）移到了 histogram.c。这些不是删除，是拆分。

```
1487 (cyclictest.c) + 199 (cyclictest.h) + 181 (histogram.c) = 1867 行  (算法+数据结构)
2362 - 655 (删除) = 1707 行 (原版算法核心)
1867 - 1707 = 160 行 (新增注释+条件编译守卫+TSC代码)
```

---

## 四、源码修改对照

### 4.1 文件拆分

**原版:** 单个 `cyclictest.c` (~2360行)，包含所有逻辑

**移植后:** 拆分为3部分

| 原版区域 | 移植后位置 | 说明 |
|---------|-----------|------|
| 数据结构(struct thread_param/stat/trigger) | `cyclictest.h` | 提取到头文件 |
| 常量定义 | `cyclictest.h` | DEFAULT_INTERVAL, MODE_xxx等 |
| timerthread() 核心算法 | `cyclictest.c` | 几乎不变 |
| process_options() 参数解析 | `cyclictest.c` | getopt_long保留 |
| print_stat/hist/JSON 输出 | `cyclictest.c` | 原样保留 |
| main() 主循环 | `cyclictest.c` → `cyclictest_main()` | 重命名为可调用函数 |
| RTEMS Init + CONFIGURE_xxx | `init.c` | 新增 |
| 极简交互Shell | `init.c` | 新增 |

### 4.2 具体代码改动

#### 改动1: RTEMS 入口替代 main()

```c
// === 原版 (Linux) ===
int main(int argc, char **argv) {
    process_options(argc, argv, max_cpus);
    // ... 创建线程, 监控循环, 输出结果
    exit(ret);
}

// === 移植后 (RTEMS) ===
// init.c — 薄包装
static rtems_task Init(rtems_task_argument ignored) {
    TEST_BEGIN();
    cyclictest_main(argc, argv);  // 调用移植后的主逻辑
    TEST_END();
    rtems_test_exit(0);
}

// cyclictest.c — 原名main逻辑, 去掉exit改为return
int cyclictest_main(int argc, char *argv[]) {
    process_options(argc, argv, max_cpus);
    // ... 创建线程, 监控循环, 输出结果
    return ret;  // 不再调用exit(), 改为return
}
```

**原因:** RTEMS 经典API任务不能从入口函数return, 必须调用`rtems_task_exit()`。原版`main()`中的`exit()`会触发RTEMS fatal error。改为`return`后由wrapper调用`rtems_task_exit()`。

#### 改动2: sched_setscheduler 空桩适配

```c
// === 原版 ===
// timerthread() 中直接调用
memset(&schedp, 0, sizeof(schedp));
schedp.sched_priority = par->prio;
if (setscheduler(0, par->policy, &schedp))   // ← Linux: 运行时提升优先级
    fatal("failed to set priority");

// === 移植后 ===
// init.c — 在创建线程前通过attr预设
if (priority && (policy == SCHED_FIFO || policy == SCHED_RR)) {
    struct sched_param sparam;
    sparam.sched_priority = priority;
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, policy);
    pthread_attr_setschedparam(&attr, &sparam);
}

// cyclictest.c timerthread() — RTEMS上跳过运行时设置
#ifndef __rtems__
    if (setscheduler(0, par->policy, &schedp))
        fatal("failed to set priority");
#endif
```

**原因:** RTEMS 6.1的`sched_setscheduler()`是空桩(直接返回ENOSYS)。必须在pthread_create之前通过attr预设调度参数。

#### 改动3: exit() → return

```c
// === 原版 ===
// 多处调用exit()
display_help(1) → exit(EXIT_FAILURE);
trigger_init()失败 → exit(EXIT_FAILURE);
hset_init()失败 → exit(EXIT_FAILURE);
主循环结束 → exit(ret);

// === 移植后 ===
// 全部改为return
display_help(1) → /* 不调用exit, 让调用者处理 */
错误路径 → return EXIT_FAILURE;
主循环结束 → return ret;
```

**原因:** RTEMS子任务不能调用exit(), 会触发`INTERNAL_ERROR_THREAD_EXITTED` fatal error。子任务必须return后由wrapper调用`rtems_task_exit()`。

#### 改动4: TSC 测量模式

```c
// === 新增 (cyclictest.c顶部) ===
#define USE_TSC  1  // 1=硬件rdtsc, 0=clock_gettime

#if USE_TSC
static inline unsigned long long rdtsc(void) {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((unsigned long long)hi << 32) | lo;
}
static double tsc_per_us = 3000.0;

// === timerthread() 中的改动 ===
#if USE_TSC
    tsc_before = rdtsc();           // 睡眠前读TSC
    clock_nanosleep(...);
    tsc_after = rdtsc();            // 醒来后读TSC
    diff = tsc_after - tsc_before   // TSC差值 = 真实耗时
           - expected_tsc_ticks;    // 减去预期=延迟
#else
    clock_gettime(&now);            // 原版: clock_gettime
    diff = calcdiff(now, next);     // 原版: 时间差
#endif
```

**原因:** RTEMS amd64 BSP 的 clock_gettime 精度只有 1ms（tick级别），无法测量微秒级延迟。硬件 TSC (rdtsc) 精度 ~0.3ns，完全绕开软件时间戳限制。

#### 改动5: 文件拆分 — cyclictest.h 提取

原版中 ~200 行数据结构和常量定义提取到 `cyclictest.h`：

```c
// 从原版 cyclictest.c 顶部提取
#define DEFAULT_INTERVAL 1000      // 默认间隔
#define MODE_CYCLIC 0              // 测量模式
#define MODE_CLOCK_NANOSLEEP 1
struct thread_param { ... };       // 线程参数结构
struct thread_stat { ... };        // 统计数据结构
struct thread_trigger { ... };     // 断点触发结构
```

**原因:** 拆分为多文件编译(init.c + cyclictest.c)，共享数据结构需要头文件。

#### 改动6: POSIX timer 模式条件编译

```c
// === MODE_CYCLIC 分支 ===
case MODE_CYCLIC:
#if !defined(__rtems__) || defined(CONFIGURE_ENABLE_POSIX_API)
    if (sigwait(&sigset, &sigs) < 0) goto out;
#endif
    break;

// timer_create/settime/delete 同理
```

**原因:** RTEMS POSIX timer 未启用(`CONFIGURE_MAXIMUM_POSIX_TIMERS=0`)，timer_create等函数不可链接。默认使用MODE_CLOCK_NANOSLEEP（功能相同）。

#### 改动7: 移除的代码块（条件编译/硬删除）

| 原版代码块 | 行数 | 处理方式 |
|-----------|------|---------|
| `set_latency_target()` (PM QoS) | ~45行 | 硬删除，注释说明原因 |
| `save/restore_cpu_idle_state()` (cpuidle) | ~200行 | 硬删除 |
| `open_msr_file()/get_smi_counter()` (SMI) | ~80行 | 硬删除 |
| `raise_soft_prio()` (RLIMIT_RTPRIO) | ~45行 | 硬删除 |
| `rt_numa_*` (NUMA) | ~50行 | `#define numa 0` 宏消除 |
| `fifothread()` (mkfifo) | ~30行 | 硬删除 |
| `rstat_shm_open/mmap/setup()` (共享内存) | ~80行 | 硬删除 |
| `/proc/loadavg` 读取 | ~5行 | 输出"N/A"替代 |
| `tracemark/tracing_stop` (ftrace) | ~5行 | 硬删除 |

#### 改动8: 极简交互 Shell（init.c 新增）

```c
// 通过 NS16550 UART 寄存器直接读写串口
static int uart_getchar(void) {
    if (inport_byte(COM1_BASE_IO + UART_LSR) & LSR_DR)
        return inport_byte(COM1_BASE_IO + UART_RBR);
    return -1;
}

// 命令循环
while (1) {
    printf("cyclictest> ");
    // 读一行 → 解析 → 如果是"cyclictest"就创建子任务运行
    // 如果是"help"就打印帮助
    // 如果是"exit"就退出
}
```

**原因:** RTEMS 官方 shell 需要 termios/文件系统等基础设施，在当前简单控制台驱动上不可用。极简 Shell 直接读取 UART 寄存器，零依赖。

---

## 五、RTEMS 平台限制详解

### 5.1 getopt optional_argument 不兼容

**根因:** newlib (RTEMS C库) 的 `getopt` 对可选参数(`::`)的处理与 glibc 不同。

```c
// getopt 短选项字符串中的 t:: 表示 -t 参数可选
"a::A::b:c:d:D:h:H:i:l:MNo:p:qrRsSt::uvx"
//                                  ^^
// Linux glibc: -t 2  → optarg="2"   ✅
// RTEMS newlib: -t 2 → optarg=NULL  ❌ (2不被识别为-t的参数)
// RTEMS newlib: -t2  → optarg="2"   ✅ (必须连在一起)
```

**影响:** `-t` 参数需要写成 `-t2` 格式。其他带`::`的参数(`-a`, `-A`)同理。

### 5.2 sched_setscheduler 空桩

**RTEMS 6.1 源码:** `cpukit/posix/src/sched_setscheduler.c`
```c
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    rtems_set_errno_and_return_minus_one(ENOSYS);  // 功能未实现
}
```

**解决方案:** 通过 `pthread_attr_setschedparam()` 在创建线程前预设优先级。

### 5.3 clock_getres(CLOCK_MONOTONIC) 失败

**原因:** amd64 BSP 使用 LAPIC tick timecounter，每秒1000次tick(CONFIGURE_MICROSECONDS_PER_TICK=1000)。`_Timecounter_Nanouptime()` 内部实现无法报告硬件精度。

```
clock_gettime(MONOTONIC): 能用，精度1ms  ✅
clock_getres(MONOTONIC):  失败返回-1    ❌
```

**解决方案:** 使用 x86 `rdtsc` 指令直接读硬件 TSC（USE_TSC=1）。

### 5.4 pthread_setaffinity_np 可能不可用

RTEMS 声明了 `pthread_setaffinity_np` 原型但底层实现可能为空。实测无报错但实际CPU绑定效果不确定。

### 5.5 POSIX timer 未启用

`CONFIGURE_MAXIMUM_POSIX_TIMERS=0` 意味着 `timer_create/settime/delete` 不可用。MODE_CYCLIC 自动降级为 MODE_CLOCK_NANOSLEEP。

---

## 六、编译与构建配置

### 6.1 waf 构建文件

```yaml
# spec/build/testsuites/samples/cyclictest.yml
build-type: test-program
features: c cprogram
source:
- testsuites/samples/cyclictest/init.c
- testsuites/samples/cyclictest/cyclictest.c
- testsuites/samples/cyclictest/histogram.c
target: testsuites/samples/cyclictest.exe
```

### 6.2 注册到 samples 组

```yaml
# spec/build/testsuites/samples/grp.yml (新增一行)
- role: build-dependency
  uid: cyclictest
```

### 6.3 RTEMS 配置

```c
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_MAXIMUM_TASKS              50
#define CONFIGURE_MAXIMUM_POSIX_THREADS      10
#define CONFIGURE_MICROSECONDS_PER_TICK    1000
```

---

## 七、使用方式

### 7.1 交互模式 (USE_SHELL=1)

```bash
qemu-system-x86_64 -m 512 -smp 4 -serial stdio -no-reboot -no-shutdown \
  --bios /usr/share/ovmf/OVMF.fd -drive file=RTEMS-GRUB.img,format=raw

cyclictest> cyclictest -t 2 -p 10 -i 100 -l 500
cyclictest> cyclictest -S -h 100 -l 1000
cyclictest> help
cyclictest> exit
```

### 7.2 自动测试模式 (USE_SHELL=0)

```c
#define CYCLICTEST_ARGS "-S", "-p", "10", "-i", "100", "-l", "500", "-h", "100"
// 编译后自动运行一次，输出结果，退出
```

### 7.3 测量模式切换

```c
#define USE_TSC  1   // 硬件rdtsc (~0.3ns精度)
#define USE_TSC  0   // clock_gettime (1ms精度, BSP限制)
```
