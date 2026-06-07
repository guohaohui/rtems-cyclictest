/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  histogram.h — 延迟直方图数据结构
 *
 *  从 rt-tests-2.10 移植，用于统计延迟分布。
 *  纯 C 数学实现，完全可移植，无平台依赖。
 *
 *  核心概念：
 *    - 将延迟值按区间(bucket)分组统计
 *    - 每个 bucket 记录落入该区间的样本数量
 *    - 支持溢出处理（延迟超过直方图范围时记录溢出事件号）
 *    - 支持多线程聚合（histoset），每个线程独立统计，也可汇总
 */

#ifndef _HISTOGRAM_H
#define _HISTOGRAM_H

#include <stdint.h>
#include <stdio.h>

/* ===== 直方图结构：单个线程的延迟分布统计 ===== */
struct histogram {
  /*
   * buckets — 桶数组，每个桶记录落入该延迟区间的样本数。
   *   buckets[0] 记录延迟在 [0, width) 的样本数
   *   buckets[1] 记录延迟在 [width, 2*width) 的样本数
   *   ...
   *   buckets[num-1] 记录延迟在 [(num-1)*width, num*width) 的样本数
   */
  unsigned long *buckets;

  unsigned long  width;           /* 每个桶覆盖的时间宽度（单位：微秒） */
  unsigned long  num;             /* 桶的总数量 */
  unsigned long  events;          /* 已记录的事件总数 */

  /*
   * oflows — 溢出事件数组，记录哪些事件（事件序号）超过了直方图范围。
   *   用于后续分析哪些特定周期发生了异常大的延迟。
   */
  unsigned long *oflows;

  unsigned long  oflow_bufsize;   /* 溢出数组的最大容量 */
  unsigned long  oflow_count;     /* 实际发生的溢出事件数量 */
  uint64_t       oflow_magnitude; /* 溢出幅度累计（超出多少个桶） */
};

/* ===== 直方图集合：多个线程的直方图聚合 ===== */
struct histoset {
  struct histogram *histos;       /* 每个线程独立的直方图数组 */
  struct histogram *sum;          /* 汇总直方图（可选，当前未使用） */
  unsigned long     num_histos;   /* 线程数（不含汇总） */
  unsigned long     num_buckets;  /* 每个直方图的桶数 */
};

/* 返回值标志位，用于 hist_sample() 返回多个条件 */
#define HIST_OVERFLOW       1     /* 发生溢出 */
#define HIST_OVERFLOW_MAG   2     /* 溢出幅度计数器溢出（极其罕见） */
#define HIST_OVERFLOW_LOG   4     /* 溢出事件日志已满 */

/* hset_print_bucket() 的标志位 */
#define HSET_PRINT_SUM      1     /* 同时打印汇总列 */
#define HSET_PRINT_JSON     2     /* JSON 格式输出 */

/*
 * hist_init — 初始化直方图
 * @h:     直方图指针
 * @width: 每个桶的宽度（微秒）
 * @num:   桶的数量
 * 返回: 0 成功, -ENOMEM 分配失败
 *
 * 分配 num 个桶，每个桶初始值为 0。
 * 直方图覆盖范围: [0, width * num) 微秒。
 * 例如: width=5, num=100 → 覆盖 0~500us，精度 5us。
 */
int  hist_init(struct histogram *h, unsigned long width, unsigned long num);

/*
 * hist_init_oflow — 初始化溢出记录
 * @h:   直方图指针
 * @num: 溢出记录的最大条数
 * 返回: 0 成功, -ENOMEM 分配失败
 *
 * 分配溢出记录数组，用于记录哪些事件号发生了溢出。
 * 例如 num=1000 表示最多记录 1000 次溢出事件的具体周期号。
 */
int  hist_init_oflow(struct histogram *h, unsigned long num);

/*
 * hist_destroy — 销毁直方图，释放内存
 */
void hist_destroy(struct histogram *h);

/*
 * hist_sample — 记录一个延迟样本
 * @h:      直方图指针
 * @sample: 延迟值（微秒）
 * 返回: 0 正常, 或 HIST_OVERFLOW 等标志位的组合
 *
 * 将 sample 归入对应的桶: bucket = sample / width。
 * 如果 bucket >= num，说明延迟超过了直方图范围，记录溢出。
 */
int  hist_sample(struct histogram *h, uint64_t sample);

/*
 * hset_init — 初始化直方图集合（为多个线程各分配一个直方图）
 * @hs:           直方图集合指针
 * @histos:       线程数
 * @bucket_width: 每个桶的宽度（微秒）
 * @num_buckets:  桶数量
 * @overflow:     溢出记录容量
 * 返回: 0 成功, 负值失败
 */
int  hset_init(struct histoset *hs, unsigned long histos,
               unsigned long bucket_width, unsigned long num_buckets,
               unsigned long overflow);

/*
 * hset_destroy — 销毁直方图集合，释放所有内存
 */
void hset_destroy(struct histoset *hs);

/*
 * hset_print_bucket — 打印一个桶的统计数据
 * @hs:     直方图集合
 * @f:      输出文件指针
 * @pre:    行前缀（如桶编号字符串）
 * @bucket: 桶索引
 * @flags:  HSET_PRINT_SUM 等标志
 *
 * 打印格式: "前缀 线程0值 \t 线程1值 \t ... [\t 汇总值]"
 * 如果该桶在所有线程中都没有样本，则跳过不输出（节省输出量）。
 */
void hset_print_bucket(struct histoset *hs, FILE *f, const char *pre,
                       unsigned long bucket, unsigned long flags);

/*
 * hist_print_json — 以 JSON 格式输出直方图
 * @h: 直方图指针
 * @f: 输出文件指针
 *
 * 只输出非零桶，格式: "bucket值": 样本数
 */
void hist_print_json(struct histogram *h, FILE *f);

/*
 * hist_print_oflows — 打印溢出事件列表
 * @h: 直方图指针
 * @f: 输出文件指针
 *
 * 打印所有溢出事件的事件号（即第几个周期发生了溢出）。
 * 如果溢出数超过缓冲区容量，末尾显示截断信息。
 */
void hist_print_oflows(struct histogram *h, FILE *f);

#endif /* _HISTOGRAM_H */
