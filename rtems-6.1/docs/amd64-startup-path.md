# amd64 变体启动路径精读

这份文档只聚焦一个主题：

> 在当前仓库的 `RTEMS 6.1` 中，`x86_64/amd64` 这个 BSP 变体是如何从 bootloader 交接，进入 `boot_card()`，再进入 RTEMS sysinit 和 `bsp_start()` 的。

说明：

- 本文主要记录的是 `amd64` 最初的 `freebsd_loader` 路径
- 当前仓库已经额外补上了 plain `amd64` 的 multiboot2 默认启动改造
- 如果你想看这次改造本身，请优先阅读 `docs/amd64-multiboot2-migration.md`

本文默认讨论的是 plain `amd64` 变体，而不是 `amd64efi`。也就是说，重点路径是：

- `bsps/x86_64/amd64/start/start.S`
- `bsps/x86_64/amd64/start/freebsd_loader.c`
- `bsps/shared/start/bootcard.c`
- `cpukit/sapi/src/exinit.c`
- `bsps/x86_64/amd64/start/bspstart.c`

## 1. 先给结论

`amd64` 的启动路径可以压缩成下面这 10 步：

```text
bootloader
  -> _start (start.S)
  -> 切到 RTEMS 中断栈
  -> 安装自己的 GDT
  -> 从 FreeBSD loader 的 handoff 信息中提取 RSDP
  -> boot_card(NULL)
  -> rtems_initialize_executive()
  -> sysinit 遍历
  -> bsp_start()
  -> paging_init() + acpi_tables_initialize() + bsp_interrupt_initialize()
  -> 后续 sysinit、驱动初始化、启动多任务
```

理解这条路径时，最重要的不是把每一行汇编都死记住，而是分清下面 3 件事：

- `_start` 负责建立“最早期 CPU 执行环境”
- `boot_card()` 负责把控制权交给 RTEMS 初始化框架
- `bsp_start()` 负责完成 x86_64/amd64 平台的最小 bring-up

## 2. 这条路径和 `pc386` 最大的不同

`pc386` 的历史路线更像“老式 32 位 BIOS PC bring-up”：

- 处理 multiboot v1
- 自己重装 GDT/IDT
- 重映射 8259 PIC
- 保留了很多 32 位 PC 历史包袱

而 `amd64` 的设计更现代，也更克制：

- 假定 bootloader 已经把 CPU 带到可运行的 64 位环境
- BSP 不自己实现一整套 32 位到 64 位的过渡引导
- BSP 尽快进入 RTEMS 初始化框架
- ACPI/LAPIC 是平台 bring-up 的中心

所以看 `amd64` 时，你应该把它理解成：

> 一个“最小 64 位 bring-up BSP”，而不是一个“包打天下的传统 PC BIOS BSP”。

## 3. 启动前提：链接脚本决定入口

文件：

- `bsps/x86_64/amd64/start/linkcmds`

最关键的几行是：

- `STARTUP(start.o)`
- `ENTRY(_start)`

这说明：

- 程序入口点是 `_start`
- `_start` 来自 `start.o`
- `start.o` 对应的源文件就是 `bsps/x86_64/amd64/start/start.S`

同一个链接脚本还做了两件值得注意的事：

- 代码默认放在 `0x00100000` 附近开始执行
- 显式保留了 `.multiboot2_header` 段

这两点说明：

- 当前 `amd64` 启动路径虽然默认偏向 FreeBSD loader handoff
- 但源码层面已经预留了 multiboot2 支持能力

## 4. `_start` 之前，bootloader 需要保证什么

这里是理解 plain `amd64` 的关键。

在 `start.S` 里有一段非常重要的注释，说明 plain `amd64` 在没有 multiboot2 支持分支时，默认假定控制权来自 FreeBSD bootloader，并且栈上布局大致是：

- `0(%rsp)`：32 位返回地址，不能使用
- `4(%rsp)`：32 位 `modulep`
- `8(%rsp)`：32 位 `kernend`

也就是说，RTEMS 自己并不负责：

- 从实模式进入保护模式
- 再从保护模式进入 long mode
- 再建立一套完整可运行的 64 位 boot ABI

这些工作它默认认为 loader 已经完成了。

这正是 plain `amd64` 和 `pc386` 思路上的根本差异。

## 5. 阶段一：`_start` 先保存旧栈，再切 RTEMS 的中断栈

文件：

- `bsps/x86_64/amd64/start/start.S`

### 5.1 保存 bootloader 留下来的栈指针

一开始做的是：

- `movq %rsp, %rbp`

这一步非常重要。原因是：

- 稍后 BSP 会立刻切换到 RTEMS 的栈
- 但是在切栈之后，它还需要回过头从 bootloader 留下来的旧栈里取参数
- 所以先把旧 `rsp` 保存在 `rbp`

### 5.2 切换到 RTEMS 的 ISR 栈

紧接着：

- 把 `$_ISR_Stack_area_begin` 装到 `%rsp`
- 再加上 `$_ISR_Stack_size`

效果就是：

- 当前执行栈被切到 RTEMS 自己准备的中断栈
- 从这一步开始，后续 C 代码不再依赖 loader 留下来的临时栈

这一步的设计意图是：

- 尽快摆脱 bootloader 的执行环境
- 尽快进入 RTEMS 自己可控的内存区域

## 6. 阶段二：如果不是 EFI boot services，装自己的 GDT

文件：

- `bsps/x86_64/amd64/start/start.S`
- `bsps/x86_64/amd64/start/gdt.c`

plain `amd64` 路径下，会走这段逻辑：

- `lgdt amd64_gdt_descriptor`
- 重新加载 `ds`、`es`、`ss`、`fs`
- 用 `retfq` 切换到新的代码段

对应的 GDT 在 `gdt.c` 里非常简单：

- 空段
- 代码段
- 数据段

你可以把这一步理解成：

- 即便 loader 已经把 CPU 带到了 long mode
- RTEMS 仍然希望后续执行依赖的是自己定义的最小 GDT，而不是 loader 留下来的 GDT

这是一种典型的“尽快收回执行环境控制权”的做法。

## 7. 阶段三：解析 FreeBSD loader 交接数据

文件：

- `bsps/x86_64/amd64/start/start.S`
- `bsps/x86_64/amd64/start/freebsd_loader.c`

在 plain `amd64` 路径下，`start.S` 接下来会做：

- 从旧栈 `4(%rbp)` 取出 `modulep`
- 放进 `%edi`
- 调用 `retrieve_info_from_freebsd_loader()`

### 7.1 为什么切了新栈还要看旧栈

因为 loader 交接的数据还在旧栈关联的上下文里。

前面保存 `%rbp` 的目的，就是为了这一刻：

- 你已经切到 RTEMS 栈了
- 但你还保留了访问 bootloader 参数的入口

### 7.2 `freebsd_loader.c` 真正取的是什么

这个文件做了很多 metadata 解析，但对于当前 BSP 最关键的结果只有一个：

- 从 loader 提供的环境变量里找到 `acpi.rsdp`
- 解析出 RSDP 物理地址
- 保存到全局变量 `acpi_rsdp_addr`

这是 plain `amd64` 启动路径最核心的一点之一。

因为后面 ACPICA 的 OSL 实现：

- `bsps/x86_64/amd64/acpi/osl/osl_tables.c`

里的 `AcpiOsGetRootPointer()` 直接返回的就是：

- `acpi_rsdp_addr`

换句话说：

> plain `amd64` 能顺利早期接上 ACPI，关键依赖之一就是 loader 把 RSDP 信息交给了它。

## 8. 阶段四：调用 `boot_card(NULL)`

文件：

- `bsps/x86_64/amd64/start/start.S`
- `bsps/shared/start/bootcard.c`

在 `start.S` 里，完成 loader 信息解析之后，会做两步：

- `xorl %edi, %edi`
- 调用 `boot_card`

这意味着：

- 传给 `boot_card()` 的 `cmdline` 是 `NULL`

这和 `pc386` 很不一样。`pc386` 会把 multiboot command line 复制出来，再把它传下去。plain `amd64` 并不依赖这条机制。

## 9. 阶段五：`boot_card()` 不是“调用 BSP”，而是“调用 RTEMS”

文件：

- `bsps/shared/start/bootcard.c`
- `bsps/include/bsp/bootcard.h`

`boot_card()` 的实现很短，但要反复读。

它只做三件核心工作：

1. 关闭中断
2. 保存 `cmdline` 到全局变量 `bsp_boot_cmdline`
3. 调用 `rtems_initialize_executive()`

这说明一个很重要的设计事实：

> `boot_card()` 不是直接把执行流程跳到 `bsp_start()`，而是把整个系统交给 RTEMS 的 sysinit 框架。

这也是 RTEMS BSP 和很多裸机 BSP 的一个本质差别。

## 10. 阶段六：`bsp_start()` 不是直接被 `boot_card()` 调的

文件：

- `bsps/shared/start/bootcard.c`
- `cpukit/include/rtems/sysinit.h`
- `cpukit/sapi/src/exinit.c`

在 `bootcard.c` 里，有这一项注册：

- `RTEMS_SYSINIT_ITEM(bsp_start, RTEMS_SYSINIT_BSP_START, RTEMS_SYSINIT_ORDER_MIDDLE)`

这意味着：

- `bsp_start()` 被放进了 `_Sysinit` 链接集
- 它是 RTEMS 系统初始化阶段中的一个有序初始化项
- 它所在的模块阶段是 `RTEMS_SYSINIT_BSP_START`

而在 `rtems_initialize_executive()` 中，RTEMS 会遍历整个 `_Sysinit` 链接集：

- 依次调用每个 handler

所以更准确的调用关系应该写成：

```text
_start
  -> boot_card()
  -> rtems_initialize_executive()
  -> 遍历 _Sysinit
  -> 在 BSP_START 阶段执行 bsp_start()
```

## 11. 阶段七：`rtems_initialize_executive()` 先做什么

文件：

- `cpukit/sapi/src/exinit.c`

这里最重要的是两层含义。

### 11.1 第一层：它会遍历 sysinit

`rtems_initialize_executive()` 的主体非常直接：

- 遍历 `_Sysinit`
- 调每个初始化 handler
- 设置系统状态为 `SYSTEM_STATE_UP`
- 请求 SMP 开始多任务
- `_Thread_Start_multitasking()`

所以系统启动本质上是：

- 一个按阶段、按顺序遍历的初始化流水线

### 11.2 第二层：在 `bsp_start()` 之前，RTEMS 已经开始建自己的核心数据结构

例如同一个文件里还注册了：

- `rtems_initialize_data_structures`
- `_Thread_Create_idle`

这意味着：

- `bsp_start()` 不是在一个“完全空白”的系统里运行
- 它运行在 RTEMS 初始化流程中一个明确的位置上

## 12. 阶段八：`bsp_start()` 是 plain `amd64` 的最小平台 bring-up

文件：

- `bsps/x86_64/amd64/start/bspstart.c`

plain `amd64` 路径下，这个函数最核心的逻辑可以压缩成：

```c
paging_init();
acpi_tables_initialize();
bsp_interrupt_initialize();
```

这 3 步就是理解 `amd64` BSP 的核心。

### 12.1 `paging_init()`

文件：

- `bsps/x86_64/amd64/start/page.c`

作用：

- 建最小页表
- 使用 1 GiB huge page
- identity map
- 写 `CR3`

目的不是做复杂内存管理，而是：

- 尽快建立 RTEMS 自己可控的页表环境

### 12.2 `acpi_tables_initialize()`

文件：

- `bsps/x86_64/amd64/acpi/acpi.c`

作用：

- 让 ACPICA 能开始使用 ACPI 表

这一步之所以能成功，前提就是：

- `freebsd_loader.c` 已经提前把 `acpi_rsdp_addr` 设置好了

### 12.3 `bsp_interrupt_initialize()`

文件：

- `bsps/shared/irq/irq-generic.c`
- `bsps/x86_64/amd64/interrupts/idt.c`

`bsp_interrupt_initialize()` 不是“简单开中断”，它做的是：

- 初始化 RTEMS 的通用中断框架
- 然后调用 BSP 自己的 `bsp_interrupt_facility_initialize()`

在 `amd64` 这里，对应的实现就在 `idt.c`。

## 13. 阶段九：中断 bring-up 的内部路径

文件：

- `bsps/x86_64/amd64/interrupts/idt.c`
- `bsps/x86_64/amd64/interrupts/apic.c`
- `cpukit/score/cpu/x86_64/include/rtems/score/idt.h`

`bsp_interrupt_facility_initialize()` 做了 3 件关键事情：

1. 把每个 RTEMS 管理的 vector 绑定到 `rtems_irq_prologue_n`
2. `lidt(&amd64_idtr)` 装入 IDTR
3. 调 `lapic_initialize()`

### 13.1 为什么要先装 IDT

因为从这一步开始：

- BSP 要把硬件中断真正接进 RTEMS 中断分发路径

如果没有自己的 IDT，中断入口还不属于 RTEMS。

### 13.2 `lapic_initialize()` 又做了什么

文件：

- `bsps/x86_64/amd64/interrupts/apic.c`

核心动作：

- 检查 CPU 是否支持 LAPIC
- 从 ACPI MADT 里找 LAPIC 信息
- 必要时修正 LAPIC 基址
- 通过 MSR 启用 APIC
- 设置 spurious vector
- 重映射并关闭传统 PIC

到这一步，plain `amd64` 的平台中断基础设施才算真正接起来。

## 14. 启动路径到这里，哪些事还没有发生

这也是读代码时很容易误判的地方。

在 `bsp_start()` 执行完之后，并不意味着：

- console 已经完成初始化
- clock driver 已经完成初始化
- ACPI 完整子系统已经完全展开
- 应用线程已经开始跑

这些事情大多发生在后续 sysinit 阶段。

## 15. 阶段十：console 和 clock 是后续 sysinit 才接上的

文件：

- `cpukit/include/rtems/confdefs/console.h`
- `cpukit/include/rtems/confdefs/clock.h`
- `bsps/x86_64/amd64/console/console.c`
- `bsps/x86_64/amd64/clock/clock.c`

如果应用配置里启用了 console 和 clock driver，那么：

- console 会在 `RTEMS_SYSINIT_DEVICE_DRIVERS` 阶段较早初始化
- clock 也会在 `RTEMS_SYSINIT_DEVICE_DRIVERS` 阶段初始化

这说明 plain `amd64` 的启动链是分层的：

- `start.S` 负责最早期 CPU 环境
- `bsp_start()` 负责最小平台 bring-up
- 设备驱动在更后面的 sysinit 阶段接入

这也是 RTEMS 很典型的设计风格。

## 16. 启动路径里的关键数据流

如果你想快速建立脑图，建议你只盯住这几条数据流。

### 16.1 栈指针

- bootloader 原始栈 `rsp`
- `_start` 先保存到 `rbp`
- 再切换到 RTEMS 的 `_ISR_Stack`

### 16.2 ACPI RSDP

- loader metadata / env
- `retrieve_info_from_freebsd_loader()`
- `acpi_rsdp_addr`
- `AcpiOsGetRootPointer()`
- ACPICA

### 16.3 中断入口

- `rtems_irq_prologue_n`
- IDT 表项
- `amd64_dispatch_isr()`
- `bsp_interrupt_handler_dispatch()`

### 16.4 平台时钟

- PIT 仅用于校准
- LAPIC timer 作为正式 tick 源
- `Clock_isr()`
- RTEMS timecounter / watchdog

## 17. 把启动路径背成一句话

如果你要用一句话向别人解释 `amd64` 的启动路径，可以这样说：

> plain `amd64` 假定 loader 已经把系统带入 64 位可运行环境，RTEMS 在 `_start` 中切换到自己的 ISR 栈、收回 GDT 控制权、从 FreeBSD loader handoff 中取出 ACPI RSDP，然后通过 `boot_card()` 进入 RTEMS sysinit，在 `bsp_start()` 中完成页表、ACPI 和 LAPIC/IDT 这一组最小平台 bring-up。

## 18. 为什么这条路径很适合学习

相比老 `pc386`，这条路径的优点是：

- 更聚焦 64 位 bring-up 的核心问题
- 更少 BIOS/实模式历史包袱
- ACPI/LAPIC 关系更清楚
- 更容易看出 BSP、通用中断框架、RTEMS sysinit 的边界

所以如果你的目标是“快速理解 RTEMS x86_64 BSP 的骨架”，`amd64` 其实比 `pc386` 更适合入门。

## 19. 读这条路径时最该带着的 5 个问题

1. `_start` 为什么要先保存旧 `rsp`，再切新栈？
2. plain `amd64` 为什么需要 `freebsd_loader.c`，它到底给了什么？
3. `boot_card()` 为什么不直接调用 `bsp_start()`？
4. `bsp_start()` 为什么只做页表、ACPI、中断这三件大事？
5. clock 和 console 为什么不在 `bsp_start()` 里直接初始化？

如果你能把这 5 个问题回答清楚，`amd64` 这条启动路径你就已经掌握得很扎实了。

## 20. 推荐的跟读顺序

如果你准备对着源码精读，推荐顺序是：

1. `bsps/x86_64/amd64/start/linkcmds`
2. `bsps/x86_64/amd64/start/start.S`
3. `bsps/x86_64/amd64/start/freebsd_loader.c`
4. `bsps/shared/start/bootcard.c`
5. `cpukit/sapi/src/exinit.c`
6. `bsps/x86_64/amd64/start/bspstart.c`
7. `bsps/x86_64/amd64/start/page.c`
8. `bsps/x86_64/amd64/acpi/acpi.c`
9. `bsps/x86_64/amd64/interrupts/idt.c`
10. `bsps/x86_64/amd64/interrupts/apic.c`

## 21. 一张总图

```text
loader
  -> _start
     -> 保存旧 rsp 到 rbp
     -> 切换到 _ISR_Stack
     -> 装自己的 GDT
     -> retrieve_info_from_freebsd_loader(modulep)
     -> boot_card(NULL)
        -> 关中断
        -> rtems_initialize_executive()
           -> sysinit: 数据结构初始化
           -> sysinit: bsp_start()
              -> paging_init()
              -> acpi_tables_initialize()
              -> bsp_interrupt_initialize()
                 -> 安装 RTEMS IRQ prologue
                 -> lidt
                 -> lapic_initialize()
                    -> parse MADT
                    -> enable APIC
                    -> disable PIC
           -> 后续 sysinit: console/clock/ACPI 完整初始化等
           -> _Thread_Start_multitasking()
```

## 22. 下一步最值得补的主题

看完这份启动路径精读，最自然的下一步有两个：

- `amd64` 中断路径精读：从 `isr_handler.S` 到 `bsp_interrupt_handler_dispatch()`
- `amd64` 时钟路径精读：从 LAPIC timer 校准到 `Clock_isr()`

如果你继续往下学，这两个主题会把你从“看懂启动”推进到“看懂 RTEMS 是怎么真正跑起来的”。
