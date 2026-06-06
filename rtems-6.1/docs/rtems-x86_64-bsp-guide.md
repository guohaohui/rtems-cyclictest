# RTEMS x86_64 BSP 与驱动学习讲义

这份文档面向这样一种学习目标：

- 你希望从源码层面理解 RTEMS，而不是只会写应用。
- 你重点关心 x86_64 BSP、启动过程、中断、时钟、串口和 SMP。
- 你希望知道哪些代码属于 BSP，哪些属于 CPU port，哪些属于 RTEMS 通用内核。

本文基于当前仓库中的 `RTEMS 6.1` 源码整理，重点针对本仓库实际存在的 x86_64 代码，而不是泛泛介绍。

## 1. 先纠正一个常见误解

在这份 `RTEMS 6.1` 源码里，x86_64 BSP 并没有被去掉。

本仓库中与 x86_64 直接相关的目录有：

- `bsps/x86_64/amd64`
- `bsps/x86_64/include`
- `cpukit/score/cpu/x86_64`
- `spec/build/bsps/x86_64/amd64`

其中最重要的两个 BSP 变体是：

- `amd64`
- `amd64efi`

它们的构建描述分别在：

- `spec/build/bsps/x86_64/amd64/bspamd64.yml`
- `spec/build/bsps/x86_64/amd64/bspamd64efi.yml`

你可以把它们理解成：

- `amd64`：传统 x86_64 BSP 变体
- `amd64efi`：带 UEFI/EFI 相关支持的变体

## 2. 学 RTEMS 先要分清的 4 层

理解 RTEMS 最容易乱的地方，是把 BSP、CPU port、驱动框架、内核混在一起看。建议始终按下面这 4 层来思考：

### 第 1 层：应用/API 层

这一层是你写应用时最常接触的部分，例如：

- Classic API
- POSIX API
- `confdefs.h` 配置
- 驱动注册和 `/dev/*`

对应源码主要在：

- `cpukit/rtems`
- `cpukit/posix`
- `cpukit/include`

### 第 2 层：RTEMS 通用内核层

这一层是 RTEMS 的核心实现，包括：

- 线程
- 调度器
- 时间管理
- 对象管理
- 系统初始化框架
- 中断扩展框架

对应源码主要在：

- `cpukit/score`
- `cpukit/sapi`
- `cpukit/libcsupport`

### 第 3 层：CPU port

CPU port 负责“这个体系结构上 RTEMS 怎么切任务、怎么处理中断上下文、怎么定义 CPU 相关类型和寄存器约定”。

x86_64 CPU port 在：

- `cpukit/score/cpu/x86_64`

这里的典型文件：

- `cpu.c`
- `x86_64-context-initialize.c`
- `x86_64-context-switch.S`
- `include/rtems/score/cpu.h`
- `include/rtems/score/cpuimpl.h`
- `include/rtems/score/idt.h`
- `include/rtems/score/x86_64.h`

### 第 4 层：BSP

BSP 负责“把 RTEMS 接到某个具体平台上”。

对于 x86_64 BSP，它主要负责：

- 早期启动
- 页表与基础平台初始化
- 解析 bootloader/firmware 提供的信息
- ACPI 表获取
- IDT/LAPIC/PIC 初始化
- 时钟中断源接入
- 串口控制台接入
- SMP 次级 CPU 启动

x86_64 BSP 代码在：

- `bsps/x86_64/amd64`

## 3. x86_64 BSP 目录怎么读

`bsps/x86_64/amd64` 下面最重要的子目录如下：

- `start`
- `interrupts`
- `clock`
- `console`
- `acpi`
- `include`
- `config`

你可以先把它们理解为：

- `start`：启动和早期平台初始化
- `interrupts`：IDT、PIC、LAPIC、中断入口和分发接线
- `clock`：时钟驱动
- `console`：串口或 EFI 控制台
- `acpi`：ACPI 表及相关支持
- `include`：BSP 专用头文件
- `config`：构建相关配置

## 4. amd64 和 amd64efi 的区别

这是理解 x86_64 BSP 时非常重要的一点。

### `amd64`

`amd64` 变体主要包含：

- `clock/clock.c`
- `console/console.c`
- `start/freebsd_loader.c`

说明这个变体最初更偏传统启动方式和串口控制台。

补充说明：

- 当前仓库已经额外补上了 plain `amd64` 的 multiboot2 默认启动改造
- `freebsd_loader.c` 仍然保留，可作为关闭 `BSP_MULTIBOOT_SUPPORT` 时的 fallback 路径

### `amd64efi`

`amd64efi` 在 `amd64` 的基础上又额外引入了：

- `clock/eficlock.c`
- `console/eficonsole.c`
- `console/efistop.c`
- `console/efigop.c`
- `console/outch.c`
- `start/multiboot2.c`
- `start/efimem.c`

说明这个变体会更多涉及：

- UEFI
- Multiboot2
- EFI console
- EFI memory map

学习建议：

- 第一阶段先只学 `amd64`
- 第二阶段再补 `amd64efi`

否则一开始会同时遇到 x86_64、ACPI、EFI、Multiboot2，多条线交织，很容易失焦。

## 5. 构建配置里的关键信息

文件：

- `bsps/x86_64/amd64/config/amd64.cfg`

里面最值得记住的是：

- `RTEMS_CPU = x86_64`
- `-mno-red-zone`
- `-mcmodel=large`

这两个编译选项的含义很关键：

- `-mno-red-zone`
  在中断/异常环境下，编译器不能假定栈指针以下的 red zone 是安全的，因为中断不会尊重它。
- `-mcmodel=large`
  告诉编译器不要过度假设地址可以塞进较小的寻址模型，避免 64 位地址和链接重定位相关问题。

这类选项是“体系结构知识落到 BSP 工程实践里”的典型例子。

## 6. 启动链路总览

这是你学习 x86_64 BSP 最重要的一条主线。

推荐你把下面这个流程背下来：

1. bootloader 把控制权交给 `_start`
2. `_start` 建立早期执行环境
3. `_start` 调用 `boot_card()`
4. `boot_card()` 调用 `rtems_initialize_executive()`
5. RTEMS sysinit 框架开始运行
6. 其中会执行 BSP 的 `bsp_start()`
7. BSP 完成分页、ACPI、中断等平台初始化
8. 后续设备驱动、时钟、控制台等继续初始化
9. 初始化线程或应用入口开始运行

这条路径的关键文件是：

- `bsps/x86_64/amd64/start/start.S`
- `bsps/shared/start/bootcard.c`
- `bsps/x86_64/amd64/start/bspstart.c`

## 7. `_start` 到底做了什么

文件：

- `bsps/x86_64/amd64/start/start.S`

这是 BSP 的最早入口。

它的主要工作可以概括为：

- 切换到 RTEMS 使用的中断栈
- 在非 EFI boot services 情况下重新装载 GDT
- 设置段寄存器
- 按启动方式获取 bootloader 传来的信息
- 最终调用 `boot_card`

这一步不是“应用入口”，而是“让 RTEMS 有资格继续初始化”的最低保障阶段。

学习这个文件时，重点看下面几件事：

- 栈是怎么切换的
- 为什么要在这里处理 GDT
- multiboot2 信息是怎么保存下来的
- 为什么最终不是调用 `main()`，而是调用 `boot_card()`

## 8. `boot_card()` 的角色

文件：

- `bsps/shared/start/bootcard.c`

`boot_card()` 是 RTEMS BSP 初始化的总入口框架。

它的核心工作非常少，但非常关键：

- 直接关闭中断
- 保存命令行
- 调用 `rtems_initialize_executive()`

这个文件很值得反复看，因为它能帮助你建立一个非常重要的观念：

> BSP 自己并不手写一个庞大的初始化主函数，真正的大规模初始化由 RTEMS 的 sysinit 框架完成。

也就是说：

- `boot_card()` 像一个总开关
- `bsp_start()` 是 BSP 插入系统初始化流程的一个关键节点
- 其它很多初始化工作是通过 `RTEMS_SYSINIT_ITEM()` 串起来的

## 9. `bsp_start()` 做了什么

文件：

- `bsps/x86_64/amd64/start/bspstart.c`

这个函数是 x86_64 BSP 早期初始化的核心入口。

它做的主要事情有：

- 在需要时处理 multiboot2 信息
- 建立分页
- 初始化 ACPI 表
- 初始化中断框架

对应调用关系大致是：

- `paging_init()`
- `acpi_tables_initialize()`
- `bsp_interrupt_initialize()`

这能非常清楚地体现 BSP 的职责：

- 平台基础内存环境
- 平台硬件描述信息
- 平台中断能力

## 10. 分页初始化怎么理解

文件：

- `bsps/x86_64/amd64/start/page.c`

这个文件不是在实现完整的虚拟内存子系统，而是在做“足够让内核跑起来”的早期页表初始化。

它的特点：

- 使用 1 GiB huge page
- 做 identity mapping
- 静态构造 PML4 和 PDPT
- 最终写入 `CR3`

你可以把它理解成：

- 目标不是复杂内存管理
- 目标是尽快建立稳定、简单、可预测的地址空间环境

阅读时重点看：

- `paging_1gib_pages_supported()`
- `get_maxphysaddr()`
- `create_cr3_entry()`
- `create_pml4_entry()`
- `create_pdpt_entry()`
- `paging_init()`

## 11. ACPI 在这里扮演什么角色

文件：

- `bsps/x86_64/amd64/acpi/acpi.c`

### 早期阶段

在 `bsp_start()` 里，BSP 会先调用：

- `acpi_tables_initialize()`

这样做的目的是：

- 让 BSP 尽早能读取 ACPI 表
- 尤其是为了后续解析 MADT

### 正常初始化阶段

稍后 `acpi.c` 又通过：

- `RTEMS_SYSINIT_ITEM(initialize_acpi, RTEMS_SYSINIT_DEVICE_DRIVERS, RTEMS_SYSINIT_ORDER_MIDDLE)`

把完整 ACPI 初始化接入了系统初始化流程。

这体现了一个 RTEMS 常见设计：

- 早期只做最必要的初始化
- 完整子系统初始化延后到 sysinit 中合适的阶段

## 12. 中断框架怎么理解

中断相关最关键的文件有：

- `bsps/x86_64/include/bsp/irq.h`
- `bsps/include/bsp/irq-generic.h`
- `bsps/x86_64/amd64/interrupts/idt.c`
- `bsps/x86_64/amd64/interrupts/apic.c`
- `cpukit/score/cpu/x86_64/include/rtems/score/idt.h`

### 先看边界

最重要的边界是：

- `irq-generic` 负责 RTEMS 的通用中断管理框架
- x86_64 BSP 负责把硬件中断入口接到这个框架上

也就是说：

- 安装 handler 链
- handler 唯一/共享规则
- 分发表管理

这些大部分是共享框架做的。

而 BSP 自己要做的是：

- 建 IDT
- 配中断门
- 初始化 LAPIC/PIC
- 把硬件向量送进 `bsp_interrupt_handler_dispatch()`

### x86_64 BSP 当前的向量情况

在当前代码里：

- `BSP_IRQ_VECTOR_NUMBER = 34`
- `BSP_INTERRUPT_VECTOR_COUNT = 35`

同时在 `cpukit/score/cpu/x86_64/include/rtems/score/idt.h` 中定义了：

- `BSP_VECTOR_APIC_TIMER = 32`
- `BSP_VECTOR_IPI = 33`
- `BSP_VECTOR_SPURIOUS = 0xFF`

这说明：

- 32 号向量留给 APIC timer
- 33 号向量留给 IPI
- spurious interrupt 使用 0xFF

### `idt.c` 的职责

文件：

- `bsps/x86_64/amd64/interrupts/idt.c`

重点看这些函数：

- `amd64_create_interrupt_descriptor()`
- `amd64_install_raw_interrupt()`
- `amd64_dispatch_isr()`
- `bsp_interrupt_facility_initialize()`

你应该重点理解：

- IDT 表项是如何构造的
- RTEMS IRQ prologue 是如何和向量绑定的
- 为什么最终分发到 `bsp_interrupt_handler_dispatch()`

### `apic.c` 的职责

文件：

- `bsps/x86_64/amd64/interrupts/apic.c`

重点看：

- `has_lapic_support()`
- `parse_madt()`
- `lapic_initialize()`
- `lapic_timer_calc_ticks()`
- `lapic_timer_enable()`

这里最重要的学习点是：

- BSP 通过 ACPI 的 MADT 找到 LAPIC 信息
- 通过 MSR 确保 APIC 硬件启用
- 通过设置 spurious vector 完成 APIC 软件启用
- 通过 PIT 校准 LAPIC timer

这里也有一个现实判断要记住：

- 当前 x86_64 BSP 在 `idt.c` 里对 `bsp_interrupt_vector_enable()` 和 `bsp_interrupt_vector_disable()` 的实现还比较简化
- 注释里已经明确写了，真正的 I/O APIC 支持还没有补完整

所以它更像一个正在成长中的 x86_64 BSP，而不是一个成熟完整的 PC 平台 BSP。

## 13. 时钟驱动怎么理解

文件：

- `bsps/x86_64/amd64/clock/clock.c`
- `bsps/shared/dev/clock/clockimpl.h`

这是学习 RTEMS 驱动分层的一个绝佳例子。

### BSP 自己做什么

`clock.c` 里主要做这几件事：

- 安装 LAPIC timer 中断 handler
- 计算 tick 频率对应的 LAPIC reload 值
- 打开 LAPIC timer
- 注册 timecounter

### 共享框架做什么

`clockimpl.h` 里负责：

- 统一的 `Clock_isr()`
- tick 计数
- 调用 `rtems_timecounter_tick()` 或相关逻辑
- 把 BSP 的“硬件时钟源”接入 RTEMS 的时间系统

你应该建立这样一个理解：

- BSP 提供“时钟硬件怎么响”
- RTEMS 通用层提供“时钟响了以后系统怎么推进时间”

如果你能读懂这组文件，后面再看别的 BSP 时钟驱动会轻松很多。

## 14. 控制台驱动怎么理解

文件：

- `bsps/x86_64/amd64/console/console.c`
- `bsps/shared/dev/serial/ns16550-context.c`
- `cpukit/include/rtems/bspIo.h`

这个控制台驱动特别适合拿来学习“BSP 不一定从零写驱动”。

### x86_64 BSP 自己做了什么

`console.c` 里做的事情并不多：

- 定义 `get_reg` / `set_reg`
- 指定 UART 端口基地址
- 指定初始波特率
- 选择 `ns16550_handler_polled`
- 实现 `uart0_output_char()`
- 把 `BSP_output_char` 指向 `uart0_output_char`

### 真正复用的是谁

真正的串口逻辑大多来自共享驱动：

- `ns16550`
- `termios`

也就是说：

- x86_64 BSP 提供硬件接线细节
- 共享串口驱动提供协议和大部分行为

这就是 RTEMS BSP 驱动代码常见的写法：薄 BSP，厚共享框架。

### 为什么这里先用 polled 模式

`console.c` 里有注释说明：

- 目前优先使用 `ns16550_handler_polled`
- 等中断支持更完整后，再切换到 interrupt-based handler

这说明目前这条控制台链路更偏 bring-up 阶段设计：

- 优先保证早期输出
- 先让 `printk` 和最基本串口可用
- 再逐步增强

## 15. `printk` 和普通 console 的区别

文件：

- `cpukit/include/rtems/bspIo.h`

学习 RTEMS BSP 时一定要分清这两个概念：

- kernel character I/O
- 普通 `/dev/tty*` 设备

`BSP_output_char` 这条链路是为了：

- 早期输出
- 内核调试输出
- `printk`

它要求：

- 非阻塞
- 尽量简单
- 在很早的阶段就能工作

而完整 console 驱动则还要考虑：

- termios
- 文件描述符
- `/dev/*`
- 应用层读写

这两条输出路径在设计目标上并不完全相同。

## 16. SMP 这条线该怎么切入

文件：

- `bsps/x86_64/amd64/start/bspsmp.c`
- `bsps/x86_64/amd64/include/smp.h`

建议你把 SMP 放在单核启动、中断、时钟、串口都读顺之后再看。

SMP 这里最重要的学习点有：

- AP trampoline 的复制
- 如何发送 INIT/SIPI
- 如何等待 AP 启动
- 如何建立 CPU index 和 LAPIC ID 的映射
- 如何安装 IPI handler

重点函数：

- `_CPU_SMP_Initialize()`
- `_CPU_SMP_Start_processor()`
- `_CPU_SMP_Get_current_processor()`
- `_CPU_SMP_Finalize_initialization()`
- `_CPU_SMP_Send_interrupt()`
- `smp_init_ap()`

这部分会把你从“BSP bring-up”带到“RTEMS SMP 架构”。

## 17. 这份 x86_64 BSP 目前能教会你什么

它非常适合学习这些主题：

- x86_64 BSP 的基本结构
- 启动代码怎么衔接 RTEMS
- ACPI/MADT 在 BSP 中怎么被使用
- IDT/LAPIC/PIC 的最小接入方式
- LAPIC timer 如何接到 RTEMS clock framework
- ns16550 串口如何接到 RTEMS console framework
- SMP AP bring-up 的基本流程

它暂时不适合当作“完整 PC 外设支持大全”来学习。

例如从当前代码能直接看出来：

- I/O APIC 相关支持还不完整
- 串口仍优先用 polled 模式
- 更复杂的平台设备生态还没有完全铺开

这并不是坏事，反而很适合学习，因为代码路径更短、更聚焦。

## 18. 推荐的源码阅读顺序

下面这条顺序是专门为“想真正吃透 x86_64 BSP”的学习者设计的。

### 第一阶段：先看系统骨架

1. `bsps/x86_64/amd64/config/amd64.cfg`
2. `bsps/x86_64/amd64/include/bsp.h`
3. `bsps/x86_64/amd64/include/start.h`
4. `bsps/shared/start/bootcard.c`

目标：

- 知道 BSP 是怎么接进系统的
- 知道构建配置里有哪些体系结构约束

### 第二阶段：看启动路径

1. `bsps/x86_64/amd64/start/start.S`
2. `bsps/x86_64/amd64/start/bspstart.c`
3. `bsps/x86_64/amd64/start/page.c`
4. `bsps/x86_64/amd64/acpi/acpi.c`

目标：

- 搞清楚系统如何从 bootloader 进入 RTEMS
- 搞清楚分页和 ACPI 为什么必须在早期阶段处理

### 第三阶段：看中断路径

1. `bsps/x86_64/include/bsp/irq.h`
2. `bsps/include/bsp/irq-generic.h`
3. `cpukit/score/cpu/x86_64/include/rtems/score/idt.h`
4. `bsps/x86_64/amd64/interrupts/idt.c`
5. `bsps/x86_64/amd64/interrupts/apic.c`

目标：

- 搞清楚 RTEMS 中断通用框架和 x86_64 BSP 的边界
- 搞清楚 vector、IDT、LAPIC、dispatch 的关系

### 第四阶段：看时钟和控制台

1. `bsps/x86_64/amd64/clock/clock.c`
2. `bsps/shared/dev/clock/clockimpl.h`
3. `bsps/x86_64/amd64/console/console.c`
4. `bsps/shared/dev/serial/ns16550-context.c`
5. `cpukit/include/rtems/bspIo.h`

目标：

- 学会识别“BSP 薄封装 + 共享驱动框架”的模式

### 第五阶段：看 SMP

1. `bsps/x86_64/amd64/include/smp.h`
2. `bsps/x86_64/amd64/start/bspsmp.c`
3. `bsps/x86_64/amd64/start/ap_trampoline.S`

目标：

- 理解 AP 启动和 IPI 的整体过程

### 第六阶段：回到 CPU port

1. `cpukit/score/cpu/x86_64/include/rtems/score/cpu.h`
2. `cpukit/score/cpu/x86_64/include/rtems/score/cpuimpl.h`
3. `cpukit/score/cpu/x86_64/x86_64-context-initialize.c`
4. `cpukit/score/cpu/x86_64/x86_64-context-switch.S`

目标：

- 分清 CPU port 和 BSP 的边界
- 进入 RTEMS 内核层真正的“线程上下文切换”部分

## 19. 一条适合深入学习 RTEMS 的总路线

如果你的长期目标不是只看 BSP，而是“真正深入理解 RTEMS”，建议按下面路线推进：

### 阶段 A：先吃透 x86_64 BSP

你现在最适合做的事：

- 读通启动链
- 读通中断链
- 读通时钟链
- 读通控制台链

### 阶段 B：转入 RTEMS 系统初始化和配置

重点主题：

- `RTEMS_SYSINIT_ITEM`
- `rtems_initialize_executive()`
- `confdefs.h`
- 初始化线程
- 驱动初始化顺序

### 阶段 C：转入 Classic API

重点主题：

- 任务
- 信号量
- 事件
- 消息队列
- 时钟

### 阶段 D：转入 `cpukit/score`

重点主题：

- Thread
- Scheduler
- Watchdog
- Timecounter
- Interrupt extension

### 阶段 E：再回头看更复杂 BSP

到这时你再去看：

- `amd64efi`
- ARM BSP
- RISC-V BSP

你的理解会扎实很多。

## 20. 建议你立刻做的 6 个练习

### 练习 1

手动画出 `start.S -> boot_card() -> rtems_initialize_executive() -> bsp_start()` 的调用链。

### 练习 2

把 `bsp_start()` 中的每一步列成表格，写明：

- 调用函数
- 它解决什么问题
- 为什么必须在这个阶段做

### 练习 3

顺着 `clock.c` 和 `clockimpl.h` 追一遍：

- LAPIC timer 中断如何变成 RTEMS tick

### 练习 4

顺着 `console.c` 和 `ns16550-context.c` 追一遍：

- `printk` 最终如何落到串口寄存器写操作

### 练习 5

阅读 `idt.c`，回答下面 3 个问题：

- RTEMS 管理的向量有哪些
- 原始中断入口和 RTEMS handler 链在哪里衔接
- 当前 BSP 哪些中断能力还没做完整

### 练习 6

阅读 `bspsmp.c`，回答：

- AP 是如何被启动的
- IPI 是如何发出去的
- 次级 CPU 启动后第一批关键动作是什么

## 21. 官方文档建议搭配读

建议你同时配合 RTEMS 6.1 官方文档阅读：

- 文档入口：<https://docs.rtems.org/docs/6.1/>
- User Manual：<https://docs.rtems.org/docs/6.1/user/>
- Classic API Guide：<https://docs.rtems.org/docs/6.1/c-user/>
- BSP and Driver Guide：<https://docs.rtems.org/docs/6.1/bsp-howto/>
- Doxygen：<https://docs.rtems.org/doxygen/6.1/>

建议配合方式如下：

- 读本地源码时，以这份讲义作为路线图
- 遇到概念不清时，再去翻官方文档对应章节
- 不要一上来通读所有官方文档

## 22. 一句话总结

如果你只记住一句话，请记住这句：

> 理解 RTEMS x86_64 BSP 的关键，不是把每个文件都背下来，而是始终分清 CPU port、BSP、共享驱动框架、RTEMS 通用内核这 4 层，并沿着启动链、中断链、时钟链、控制台链去追代码。

---

如果你继续往下学，下一份最适合补充的材料应该是：

- “x86_64 BSP 启动路径逐行精读”
- 或者
- “x86_64 BSP 中断与时钟源码详解”

目前仓库里已经补充了一份启动路径材料：

- `docs/amd64-startup-path.md`
