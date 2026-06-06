# amd64 BSP 切换到 Multiboot2 启动修改说明

这份文档记录当前仓库中对 plain `amd64` BSP 做的一次启动路径改造：

> 在不使用 `amd64efi` BSP 的前提下，让 `amd64` BSP 支持并默认走 `multiboot2` 启动，而不是仅依赖 `freebsd_loader` handoff。

## 1. 修改目标

修改目标分成 3 个层面：

- 让 plain `amd64` 的启动汇编真正走 `multiboot2` 分支
- 让 plain `amd64` 把 `multiboot2.c` 编进来并具备正确的链接参数
- 在不启用 EFI boot services 的情况下，避免 `multiboot2.c` 对 EFI 头文件和 EFI 对象的强依赖

## 2. 修改前的行为

plain `amd64` 原本的情况是：

- `start.S` 里已经预留了 `BSP_MULTIBOOT_SUPPORT` 分支
- `bspstart.c` 里也已经预留了 `process_multiboot2_info()` 的调用
- 但是 `bspamd64.yml` 并没有把 `multiboot2.c` 编进 plain `amd64`
- 也没有给 plain `amd64` 接上 `optmultiboot`
- 也没有给 plain `amd64` 接上 `optldpagesize`
- `multiboot2.c` 还无条件 `#include <efi.h>`

这意味着：

- 从代码设计上说，plain `amd64` 已经半支持 `multiboot2`
- 但从实际构建结果上说，它仍然主要依赖 `freebsd_loader.c`

## 3. 修改后的行为

修改后，plain `amd64` 的默认行为变成：

- 构建时启用 `BSP_MULTIBOOT_SUPPORT`
- `start.S` 会保存 multiboot2 的 magic 和 info pointer
- `start.S` 会在进入 `boot_card()` 之前，通过 `boot_args()` 取到 multiboot2 命令行
- `boot_card()` 可以拿到非空的命令行指针
- `bsp_start()` 阶段继续调用 `process_multiboot2_info()` 解析 ACPI RSDP 和其它 multiboot2 tag

同时：

- `freebsd_loader.c` 仍然保留在 plain `amd64` 中
- 如果后续显式关闭 `BSP_MULTIBOOT_SUPPORT`，旧路径仍然可以作为 fallback 保留

## 4. 具体修改点

### 4.1 `start.S`

文件：

- `bsps/x86_64/amd64/start/start.S`

修改点：

- 在 `BSP_MULTIBOOT_SUPPORT` 分支下，保存完 `_multiboot2_magic` 和 `_multiboot2_info_ptr` 之后，不再把 `boot_card()` 参数固定为 `NULL`
- 改为调用 `boot_args()`
- 将其返回值作为 `boot_card()` 的 `cmdline`

改动目的：

- 让 plain `amd64` 的 multiboot2 启动路径，不仅“能启动”，还能够把 multiboot2 命令行接入 RTEMS 的统一命令行入口 `bsp_boot_cmdline`

### 4.2 `multiboot2.c`

文件：

- `bsps/x86_64/amd64/start/multiboot2.c`

修改点：

- 将 `#include <efi.h>` 改为只在 `BSP_USE_EFI_BOOT_SERVICES` 下包含
- 增加轻量级的 multiboot2 信息有效性检查
- 增加 early/lazy 的命令行缓存逻辑
- 让 `boot_args()` 在首次调用时就能从 multiboot2 tag 中提取 cmdline
- 修复 `process_multiboot2_info()` 中对启动参数复制不安全的问题
- 修复 `already_processed` 标志没有置位的问题

改动目的：

- 让 plain `amd64` 能单独编译 multiboot2 逻辑，而不依赖 `amd64efi`
- 让 `boot_args()` 可以在 `boot_card()` 之前被安全调用
- 让命令行缓存既可用于早期 `boot_card()`，也可用于后续 `bsp_start()` 和 EFI 控制台相关代码

### 4.3 `bspamd64.yml`

文件：

- `spec/build/bsps/x86_64/amd64/bspamd64.yml`

修改点：

- 增加对 `../../optmultiboot` 的 build dependency
- 增加对 `optldpagesize` 的 build dependency
- 把 `bsps/x86_64/amd64/start/multiboot2.c` 加入 plain `amd64` 的 source 列表

改动目的：

- 让 plain `amd64` 构建时真正定义 `BSP_MULTIBOOT_SUPPORT`
- 让 multiboot2 解析代码参与编译
- 让 x86_64 链接时使用适合 multiboot2 头部识别的 `LD_MAX_PAGE_SIZE=4096`

### 4.4 `obj.yml`

文件：

- `spec/build/bsps/x86_64/amd64/obj.yml`

修改点：

- 增加对 `../../optmultiboot` 的 build dependency

改动目的：

- 让 `bspstart.c` 在 plain `amd64` 构建中也能拿到 `BSP_MULTIBOOT_SUPPORT`
- 让 `process_multiboot2_info()` 这条路径与启动文件保持一致

### 4.5 `start.yml`

文件：

- `spec/build/bsps/x86_64/amd64/start.yml`

修改点：

- 增加对 `../../optmultiboot` 的 build dependency

改动目的：

- 让 `start.S` 对应的 start-file 目标在 plain `amd64` 构建中真正定义 `BSP_MULTIBOOT_SUPPORT`
- 避免只在 BSP 主描述里接选项，但启动文件本身没有吃到宏定义

## 5. 为什么要把 `optldpagesize` 接到 plain `amd64`

文件：

- `spec/build/bsps/x86_64/amd64/optldpagesize.yml`

这项配置原本只挂在 `amd64efi` 上，但它的描述其实并不是 EFI 专属：

- x86_64 默认使用 2 MiB max page size
- 这个值会导致 multiboot2 头部不容易形成正确、可识别的二进制布局
- 将其改为 4 KiB 才更适合 multiboot2

因此：

- 只要 plain `amd64` 要真正走 multiboot2
- 这项链接参数也应该一并接过来

## 6. 为什么保留 `freebsd_loader.c`

这是一个有意保留的兼容点。

原因有 2 个：

- `start.S` 本身已经将 FreeBSD loader 路径和 multiboot2 路径用宏分开
- 保留旧实现可以让 plain `amd64` 在关闭 `BSP_MULTIBOOT_SUPPORT` 时仍有 fallback 路径

所以这次修改不是“删除 FreeBSD loader 支持”，而是：

- 将 plain `amd64` 的默认启动重心切换到 multiboot2
- 同时保留旧的 FreeBSD loader 解析实现

## 7. 这次修改解决了什么实际问题

这次修改主要解决了下面这些问题：

- plain `amd64` 之前虽然有 multiboot2 分支，但实际构建并没有接通
- plain `amd64` 之前不能在不使用 `amd64efi` 的情况下完整使用 multiboot2 启动
- plain `amd64` 之前即便强行接 multiboot2，`boot_card()` 仍然拿不到 multiboot2 cmdline
- `multiboot2.c` 之前对 EFI 头文件有不必要的强依赖

## 8. 修改后的启动路径

修改后的 plain `amd64` 默认启动路径可以概括为：

```text
multiboot2 loader
  -> _start
     -> 保存 _multiboot2_magic / _multiboot2_info_ptr
     -> boot_args() 早期解析 cmdline
     -> boot_card(cmdline)
        -> rtems_initialize_executive()
           -> sysinit
           -> bsp_start()
              -> process_multiboot2_info()
              -> paging_init()
              -> acpi_tables_initialize()
              -> bsp_interrupt_initialize()
```

这条路径与 `amd64efi` 的区别在于：

- 它不依赖 EFI boot services
- 不编译 EFI 控制台和 EFI 内存扩展逻辑
- 只使用 multiboot2 的通用 tag 能力

## 9. 仍然保留的限制

这次修改并不意味着 plain `amd64` 获得了 `amd64efi` 的全部能力。

仍然成立的边界包括：

- plain `amd64` 没有 EFI 控制台路径
- plain `amd64` 没有 EFI memory map 扩展逻辑
- plain `amd64` 仍然是串口最小 bring-up 风格 BSP
- plain `amd64` 的 I/O APIC 等能力仍旧没有因为这次启动方式切换而自动完善

也就是说，这次修改只改变：

- 启动协议
- 启动信息来源
- 命令行与 ACPI tag 的获取方式

它没有改变 BSP 的平台功能边界。

## 10. 涉及的文件

本次修改涉及：

- `bsps/x86_64/amd64/start/start.S`
- `bsps/x86_64/amd64/start/multiboot2.c`
- `spec/build/bsps/x86_64/amd64/bspamd64.yml`
- `spec/build/bsps/x86_64/amd64/obj.yml`
- `spec/build/bsps/x86_64/amd64/start.yml`

联动更新的说明文档：

- `docs/amd64-multiboot2-migration.md`
- `docs/amd64-startup-path.md`
- `docs/rtems-x86_64-bsp-guide.md`

## 11. 当前仓库中的验证状态

当前已经完成的是静态校验：

- 已确认 `start.S`、`bspstart.c`、`multiboot2.c`、`bspamd64.yml`、`obj.yml`、`start.yml` 的接线关系一致
- 已确认 plain `amd64` 构建描述现在会同时为启动文件和对象文件定义 `BSP_MULTIBOOT_SUPPORT`
- 已确认 `multiboot2.c` 在 non-EFI 情况下不再强制包含 `efi.h`

当前还没有完成的是编译和引导验证。

原因是这台机器目前没有可直接调用的 Python 运行时：

- `python` 命令不存在
- `py.exe` 存在，但没有可用的已安装 Python 解释器

因此当前无法直接运行 `waf configure` / `waf build` 做本地编译验证。

## 12. 建议的后续验证

如果后面要做完整验证，建议重点检查：

1. 生成的 plain `amd64` ELF 是否包含有效的 `.multiboot2_header`
2. multiboot2 bootloader 是否能识别并加载镜像
3. `bsp_boot_cmdline` 是否能拿到 multiboot2 cmdline
4. `acpi_rsdp_addr` 是否能从 multiboot2 ACPI tag 正确设置
5. 后续 `bsp_start()` 是否能顺利完成 ACPI 和中断初始化

## 13. 一句话总结

这次修改的本质不是“发明了一条新启动路径”，而是：

> 把 plain `amd64` 源码里原本就已经存在、但没有完全接线的 multiboot2 启动路径补齐，并让它在不依赖 `amd64efi` 的情况下真正可用。
