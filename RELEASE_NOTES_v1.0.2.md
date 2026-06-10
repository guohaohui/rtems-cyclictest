## RTEMS Cyclictest v1.0.2

Bug 修复版本，主要解决 v1.0.1 中发现的 3 个问题，并完整实现了 `MODE_SYS_NANOSLEEP`。

### Bug 修复

| # | 问题 | 影响 | 修复 |
|---|------|------|------|
| 1 | `-s` (sys_nanosleep) **卡死** | 线程命中 `default: goto out` 直接退出，`shutdown` 未置位导致主循环死等 | 实现完整 `MODE_SYS_NANOSLEEP` case + `shutdown++` 无条件置位 |
| 2 | `--help` **不退出** | 打印帮助后继续执行测试循环 | 新增 `help_printed` 标志，`process_options` 返回后检查并 `return EXIT_SUCCESS` |
| 3 | `-R` (clock resolution) **垃圾值** | `clock_getres` 失败时打印未初始化栈变量 (~2.4×10¹⁸ ns) | 新增 `res_ok` 标志，失败时跳过 reported resolution |

### 新增功能

- **`MODE_SYS_NANOSLEEP` (`-s`) 完整实现**：使用 POSIX `nanosleep()` 进行 tick-based 相对睡眠测量。与默认 `clock_nanosleep(TIMER_ABSTIME)` 的关键区别：延迟逐周期漂移（`next = now + interval`，无绝对时间锚点），适合测量系统原生定时器的漂移特性

### 文档更新

- `CYCLICTEST_DIFF_ANALYSIS.md` 新增第 8.5 部分：命令行参数完整对照表（原版 40 个 vs RTEMS 28 个，逐一说明每个参数功能、行为差异、移除原因）

### 版本标识

- Help 文本显示 `cyclictest V 1.0.2 (RTEMS port)`
