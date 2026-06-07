# RTEMS Cyclictest

[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-RTEMS%206.1-green.svg)](https://www.rtems.org/)

将 Linux [rt-tests](https://git.kernel.org/pub/scm/utils/rt-tests/rt-tests.git) 套件中的 **cyclictest** 实时延迟测量工具移植到 [RTEMS 6.1](https://www.rtems.org/) 实时操作系统。

---

## 简介

**cyclictest** 是 Linux 实时（RT）社区最权威的延迟基准测试工具，由 Thomas Gleixner、Clark Williams 等内核维护者开发。它通过精确测量线程的"预期醒来时间"与"实际醒来时间"之差，量化操作系统的**实时延迟**。

本项目将 cyclictest 完整移植到 RTEMS 6.1 平台，保留了原版核心测量算法，支持 28 个命令行参数，新增硬件 TSC 高精度计时和交互式 Shell 运行环境。

### 测量原理

```
预期醒来时间:  next = now + interval
实际醒来时间:  clock_gettime(&now)
延迟:          diff = now - next   ← 越小越好
```

测量线程以固定间隔（如 1000µs）睡眠，醒来后计算时间偏差。偏差越大，说明系统实时性越差。

---

## 项目结构

```
rtems-cyclictest/
├── rt-tests-2.10/                  # 原版 Linux rt-tests 源码
│   └── src/cyclictest/cyclictest.c # 原版 cyclictest（2362 行）
├── rtems-6.1/
│   └── testsuites/samples/cyclictest/
│       ├── cyclictest.c            # 核心测量算法（1487 行，详细中文注释）
│       ├── cyclictest.h            # 数据结构、常量、函数声明（中文注释）
│       ├── init.c                  # RTEMS 入口 + Shell 交互环境（中文注释）
│       ├── histogram.c             # 延迟分布直方图（中文注释）
│       ├── histogram.h             # 直方图数据结构（中文注释）
│       ├── cyclictest.scn          # RTEMS 测试期望输出
│       └── cyclictest.doc          # RTEMS 测试文档
├── grub/                           # QEMU 启动镜像制作工具
├── CYCLICTEST_DIFF_ANALYSIS.md     # 原版 vs RTEMS 适配版 逐行差异分析
└── rtems测试说明                    # 32 个测试用例的详细说明
```

---

## 快速开始

### 环境要求

- RTEMS 6.1 工具链（x86_64/amd64 BSP）
- QEMU x86_64（带 OVMF UEFI 固件）
- GRUB + makefs（制作启动镜像）

### 编译 & 运行

```bash
# 1. 设置工具链路径
export PATH=$HOME/ghh/opt/rtems6.1/bin:$PATH

# 2. 编译
cd rtems-6.1
./waf build

# 3. 制作启动镜像
cd ../grub
cp ../rtems-6.1/build/x86_64/amd64/testsuites/samples/cyclictest.exe ./
cp cyclictest.exe RTEMS-GRUB/rtems
makefs -t msdos -s 50m RTEMS-GRUB.img RTEMS-GRUB

# 4. QEMU 运行
qemu-system-x86_64 -m 512 -smp 4 -serial stdio -no-reboot -no-shutdown \
  --bios /usr/share/ovmf/OVMF.fd -drive file=RTEMS-GRUB.img,format=raw
```

### 运行示例

```
cyclictest> cyclictest -S -p 10 -i 100 -l 500

TSC: 3500 MHz
policy: fifo: loadavg: N/A
T: 0 (184614913) P:10 I:100 C:  500 Min:    5 Act:    7 Avg:    6 Max:   42
T: 1 (184614914) P:10 I:100 C:  500 Min:    4 Act:    6 Avg:    5 Max:   38
*** END OF TEST CYCLICTEST ***
```

---

## 特性

### 支持的参数（28 个）

| 类型 | 参数 | 说明 |
|------|------|------|
| 测量控制 | `-i`, `-l`, `-D`, `-d` | 间隔、循环次数、运行时长、线程间距 |
| 线程管理 | `-t`, `-S`, `-a` | 线程数、SMP 模式、CPU 亲和性 |
| 调度策略 | `-p`, `--policy`, `--priospread` | 优先级、FIFO/RR、优先级递减 |
| 时钟控制 | `-c`, `-r`, `-s`, `-x`, `-R` | 时钟源、相对定时器、POSIX 定时器 |
| 对齐 | `-A`, `--secaligned` | 偏移对齐、秒边界对齐 |
| 输出格式 | `-v`, `-q`, `-N`, `-u`, `-M`, `-o` | 详细、静默、纳秒、无缓冲 |
| 直方图 | `-h`, `-H` | 延迟分布直方图 |
| 异常检测 | `-b`, `--spike`, `--spike-nodes` | 断点追踪、尖峰记录 |

### RTEMS 专有特性

| 特性 | 说明 |
|------|------|
| **硬件 TSC 高精度计时** | rdtsc 指令级精度（~0.3ns），远优于 clock_gettime（~1ms） |
| **交互式 Shell** | 轮询 UART 输入，支持退格编辑，可多次运行测试 |
| **自动测试模式** | 编译切换 `USE_SHELL=0`，启动自动运行 → 退出，适合 CI/CD |
| **自动降级** | POSIX 定时器不可用时自动回退为 clock_nanosleep，无致命错误 |

### 与原版的主要差异（10 项移除 + 2 项新增）

| 移除的功能 | 原因 |
|------------|------|
| NUMA 支持 | RTEMS 无 NUMA 硬件抽象 |
| SMI 计数器 | /dev/cpu/N/msr 是 Linux 特有接口 |
| 电源管理 (cpuidle/dma_latency) | RTEMS 无 C-state 管理 |
| ftrace 追踪 | Linux 内核专有 |
| /proc/loadavg | 无 /proc 文件系统 |
| 共享内存运行状态 (rstat) | shm_open/mmap 不可用 |
| 命名管道输出 (FIFO) | mkfifo 不可用 |
| 调度优先级限制 (rlimit) | RTEMS 扁平优先级模型 |
| mlockall 内存锁定 | 无虚拟内存/swap |
| Android bionic 兼容 | 不适用 |

| 新增的功能 | 说明 |
|------------|------|
| **TSC 硬件计时** | rdtsc 指令，0.3ns 级精度，启动时自动校准 |
| **RTEMS Shell 环境** | init.c 提供交互式命令行 + 自动测试双模式 |

> 详细差异分析请阅读 [CYCLICTEST_DIFF_ANALYSIS.md](CYCLICTEST_DIFF_ANALYSIS.md)（16 个部分、逐行对比、含原理解释）

---

## 已知限制

| 限制 | 原因 | 影响 |
|------|------|------|
| CLOCK_MONOTONIC 分辨率不可用 | RTEMS POSIX 实现不完整 | `-R` 检查报告失败 |
| clock_gettime 精度 ~1ms | BSP LAPIC tick timecounter | 建议启用 `USE_TSC=1` |
| sched_setscheduler 是空桩 | RTEMS 6.1 未实现 | 优先级通过 pthread_attr 预设 |
| pthread_setaffinity_np 不可用 | RTEMS 未实现 | `-a` 参数尽力而为 |
| POSIX 定时器不可用 | 未配置 POSIX Timer | `-x` 自动降级为 clock_nanosleep |
| getopt optional_argument | newlib vs glibc 差异 | `-t`/`-a`/`-A` 需用连写格式（如 `-t2`） |

---

## 编译选项

`init.c` 中的编译宏：

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `USE_SHELL` | `1` | `1`=交互 Shell，`0`=自动测试 |
| `CYCLICTEST_ARGS` | `"-l", "500"` | 自动测试模式的默认参数 |

`cyclictest.c` 中的编译宏：

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `USE_TSC` | `1` | `1`=硬件 rdtsc 测量，`0`=clock_gettime |

---

## 许可证

BSD-2-Clause（适配版） · GPL-2.0-only（原版 rt-tests-2.10）

---

## 参考

- [原版 rt-tests 仓库](https://git.kernel.org/pub/scm/utils/rt-tests/rt-tests.git)
- [RTEMS 官方网站](https://www.rtems.org/)
- [cyclictest 原理 (OSADL)](https://www.osadl.org/Cyclictest-.cyclictest.0.html)
