/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  histogram.c — 延迟直方图实现
 *
 *  从 rt-tests-2.10 (Red Hat, 2023) 移植。
 *  纯 C 数学实现 — 无平台依赖，在 Linux 和 RTEMS 上完全一致。
 *
 *  功能概述：
 *    1. 统计延迟分布：将每次测量的延迟值归入对应的时间区间（桶）
 *    2. 溢出处理：当延迟超出直方图范围时，记录溢出事件号供后续分析
 *    3. 多线程聚合：支持多个线程各自维护独立直方图，统一输出
 *
 *  使用示例：
 *    struct histogram h;
 *    hist_init(&h, 5, 100);        // 每个桶 5us，共 100 桶（覆盖 0~500us）
 *    hist_init_oflow(&h, 1000);    // 最多记录 1000 次溢出
 *    hist_sample(&h, 42);           // 记录 42us 延迟（落入桶 8）
 *    hist_sample(&h, 600);          // 记录 600us 延迟（溢出！）
 *    hist_print_oflows(&h, stdout); // 打印溢出事件列表
 *    hist_destroy(&h);
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "histogram.h"

/*
 * hist_init — 初始化直方图
 *
 * 分配 num 个桶，每个桶用于统计延迟值在 [bucket*width, (bucket+1)*width)
 * 范围内的样本数量。
 *
 * @h:     直方图指针（调用者分配结构体内存）
 * @width: 每个桶的时间宽度（微秒），决定统计精度
 * @num:   桶的数量，决定了直方图覆盖范围 = width * num
 * 返回: 0 成功, -ENOMEM 内存分配失败
 */
int hist_init(struct histogram *h, unsigned long width, unsigned long num)
{
  /* 先清零整个结构体，确保所有字段有初始值 */
  memset(h, 0, sizeof(*h));
  h->width = width;
  h->num   = num;

  /* 分配桶数组，calloc 自动初始化为 0 */
  h->buckets = calloc(num, sizeof(unsigned long));
  if (!h->buckets)
    return -ENOMEM;     /* 内存不足 */

  return 0;
}

/*
 * hist_init_oflow — 初始化溢出事件记录
 *
 * 当延迟值超过直方图覆盖范围时，记录下"第几个周期"发生了溢出。
 * 这帮助开发者定位哪些特定的测量周期出现了异常大的延迟。
 *
 * @h:   直方图指针
 * @num: 溢出记录数组大小（例如 1000 表示最多记录 1000 次溢出的周期号）
 * 返回: 0 成功, -ENOMEM 内存分配失败
 */
int hist_init_oflow(struct histogram *h, unsigned long num)
{
  h->oflow_bufsize = num;
  h->oflows = calloc(num, sizeof(unsigned long));
  if (!h->oflows)
    return -ENOMEM;

  return 0;
}

/*
 * hist_destroy — 释放直方图占用的所有堆内存
 *
 * 注意：不释放 histogram 结构体本身（由调用者管理），
 * 只释放内部分配的 buckets 和 oflows 数组。
 */
void hist_destroy(struct histogram *h)
{
  free(h->oflows);
  h->oflows = NULL;
  free(h->buckets);
  h->buckets = NULL;
}

/*
 * hist_sample — 记录一个延迟样本
 *
 * 这是直方图最核心的函数，每次测量完成后调用。
 *
 * 处理逻辑：
 *   1. 根据 sample / width 计算该延迟属于哪个桶
 *   2. 如果 bucket < num：正常范围，桶计数 +1，返回 0
 *   3. 如果 bucket >= num：溢出！
 *      - 记录溢出事件号（当前 events 计数）
 *      - 累加溢出幅度（超出多少个桶）
 *      - 更新 oflow_count
 *      - 返回 HIST_OVERFLOW（及可能的其他溢出标志位）
 *
 * @h:      直方图指针
 * @sample: 延迟值（单位：微秒，与 width 一致）
 * 返回: 0 表示正常，非 0 表示溢出（可包含多个标志位）
 */
int hist_sample(struct histogram *h, uint64_t sample)
{
  /* 计算样本落入哪个桶 */
  unsigned long bucket = sample / h->width;
  unsigned long extra;            /* 超出多少个桶 */
  unsigned long event = h->events++;  /* 当前事件号，同时递增总计数 */
  int ret;

  /* 正常范围：桶计数 +1 */
  if (bucket < h->num) {
    h->buckets[bucket]++;
    return 0;
  }

  /* ===== 溢出处理 ===== */
  ret = HIST_OVERFLOW;

  /* 计算溢出幅度并累加 */
  extra = bucket - h->num;
  if (h->oflow_magnitude + extra > h->oflow_magnitude)  /* 防溢出回绕检查 */
    h->oflow_magnitude += extra;
  else
    ret |= HIST_OVERFLOW_MAG;   /* 累加器自身溢出（极其罕见，需极大延迟值） */

  /* 记录溢出事件号（用于定位哪个周期出了问题） */
  if (h->oflows) {
    if (h->oflow_count < h->oflow_bufsize)
      h->oflows[h->oflow_count] = event;   /* 还有空间，记录事件号 */
    else
      ret |= HIST_OVERFLOW_LOG;            /* 溢出日志已满 */
  }

  h->oflow_count++;
  return ret;
}

/*
 * hset_init — 初始化直方图集合
 *
 * 为多个测量线程各分配一个独立的直方图。
 * 每个线程的直方图配置相同（相同的桶宽度、桶数量、溢出容量）。
 *
 * @hs:           直方图集合指针
 * @num_histos:   线程数量（每个线程一个直方图）
 * @bucket_width: 桶宽度（微秒）
 * @num_buckets:  每个直方图的桶数量
 * @overflow:     溢出记录容量（0 表示不记录溢出）
 * 返回: 0 成功, -EINVAL 参数无效, -ENOMEM 内存分配失败
 */
int hset_init(struct histoset *hs, unsigned long num_histos,
              unsigned long bucket_width, unsigned long num_buckets,
              unsigned long overflow)
{
  unsigned long i;

  /* 至少需要一个直方图 */
  if (num_histos == 0)
    return -EINVAL;

  hs->num_histos  = num_histos;
  hs->num_buckets = num_buckets;

  /* 分配 num_histos 个 histogram 结构体数组 */
  hs->histos = calloc(num_histos, sizeof(struct histogram));
  if (!hs->histos)
    return -ENOMEM;

  /* 逐个初始化每个线程的直方图 */
  for (i = 0; i < num_histos; i++) {
    if (hist_init(&hs->histos[i], bucket_width, num_buckets))
      goto fail;      /* 初始化失败，跳转到清理 */
    if (overflow && hist_init_oflow(&hs->histos[i], overflow))
      goto fail;      /* 溢出记录初始化失败 */
  }

  return 0;

fail:
  /* 出错时释放已分配的资源，保证无内存泄漏 */
  hset_destroy(hs);
  return -ENOMEM;
}

/*
 * hset_destroy — 销毁直方图集合
 *
 * 遍历所有直方图，逐个销毁并释放内存。
 */
void hset_destroy(struct histoset *hs)
{
  unsigned long i;

  if (hs->histos) {
    for (i = 0; i < hs->num_histos; i++)
      hist_destroy(&hs->histos[i]);   /* 释放每个直方图的 buckets/oflows */
  }

  free(hs->histos);     /* 释放直方图结构体数组 */
  hs->histos = NULL;    /* 防止悬空指针 */
}

/*
 * hset_print_bucket — 打印一个时间区间的统计数据
 *
 * 按线程逐列输出指定桶的样本数，格式为：
 *   "前缀 线程0值 \t 线程1值 \t ... \t 汇总值"
 *
 * 如果该桶在所有线程中都没有样本（sum==0），则不输出任何内容，
 * 这样可以大幅减少直方图输出量。
 *
 * @hs:     直方图集合
 * @f:      输出目标（stdout 或文件）
 * @pre:    行前缀字符串（通常是桶编号，如 "000042"）
 * @bucket: 要打印的桶索引
 * @flags:  输出选项（HSET_PRINT_SUM 等）
 */
void hset_print_bucket(struct histoset *hs, FILE *f, const char *pre,
                       unsigned long bucket, unsigned long flags)
{
  unsigned long long sum = 0;
  unsigned long i;

  /* 桶索引越界检查 */
  if (bucket >= hs->num_buckets)
    return;

  /* 先计算该桶在所有线程中的样本总数 */
  for (i = 0; i < hs->num_histos; i++)
    sum += hs->histos[i].buckets[bucket];

  /* 如果所有线程在该桶都没有样本，跳过不输出 */
  if (sum == 0)
    return;

  /* 输出行前缀 */
  if (pre)
    fprintf(f, "%s", pre);

  /* 逐线程输出该桶的样本数（制表符分隔） */
  for (i = 0; i < hs->num_histos; i++) {
    unsigned long val = hs->histos[i].buckets[bucket];
    if (i != 0)
      fprintf(f, "\t");
    fprintf(f, "%06lu", val);       /* 6 位宽度，前置补零 */
  }

  /* 如果要求显示汇总列 */
  if (flags & HSET_PRINT_SUM)
    fprintf(f, "\t%06llu", sum);

  fprintf(f, "\n");
}

/*
 * hist_print_json — 以 JSON 格式输出单个直方图
 *
 * 输出格式：
 *   "bucket_index": count,
 *   "bucket_index": count,
 *   ...
 *
 * 只输出非零桶，减少 JSON 体积。
 */
void hist_print_json(struct histogram *h, FILE *f)
{
  unsigned long i;
  bool comma = false;     /* 控制 JSON 逗号分隔 */

  for (i = 0; i < h->num; i++) {
    unsigned long val = h->buckets[i];
    if (val != 0) {
      if (comma)
        fprintf(f, ",");          /* 前一条目后加逗号 */
      fprintf(f, "\n        \"%lu\": %lu", i, val);
      comma = true;
    }
  }

  fprintf(f, "\n");
}

/*
 * hist_print_oflows — 打印溢出事件列表
 *
 * 输出发生溢出的事件序号（即第几个测量周期），
 * 用于定位哪些特定周期出现了异常大的延迟。
 *
 * 输出示例：
 *   00042 00137 00892 # 00010 others
 *   ↑     ↑     ↑      ↑
 *   事件号         截断提示（还有10次溢出未显示）
 */
void hist_print_oflows(struct histogram *h, FILE *f)
{
  unsigned long i;

  for (i = 0; i < h->oflow_count; i++) {
    if (i >= h->oflow_bufsize)
      break;              /* 达到缓冲区上限，停止输出 */
    if (i != 0)
      fprintf(f, " ");    /* 空格分隔各事件号 */
    fprintf(f, "%05lu", h->oflows[i]);
  }

  /* 如果溢出次数超过缓冲区容量，显示截断信息 */
  if (i >= h->oflow_bufsize)
    fprintf(f, " # %05lu others", h->oflow_count - h->oflow_bufsize);
}
