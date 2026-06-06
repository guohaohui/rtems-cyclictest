/* SPDX-License-Identifier: BSD-2-Clause */

/*
 *  histogram.h — latency histogram data structure
 *
 *  Ported from rt-tests-2.10.  Pure C math, fully portable.
 */

#ifndef _HISTOGRAM_H
#define _HISTOGRAM_H

#include <stdint.h>
#include <stdio.h>

struct histogram {
  unsigned long *buckets;
  unsigned long  width;           /* interval per bucket */
  unsigned long  num;             /* number of buckets */
  unsigned long  events;          /* total events logged */

  unsigned long *oflows;          /* overflow event indices */
  unsigned long  oflow_bufsize;   /* max overflow entries */
  unsigned long  oflow_count;     /* number of events that overflowed */
  uint64_t       oflow_magnitude; /* sum of how far buckets overflowed */
};

struct histoset {
  struct histogram *histos;       /* per-thread histograms */
  struct histogram *sum;          /* accumulator (optional) */
  unsigned long     num_histos;
  unsigned long     num_buckets;
};

#define HIST_OVERFLOW       1
#define HIST_OVERFLOW_MAG   2
#define HIST_OVERFLOW_LOG   4

#define HSET_PRINT_SUM      1
#define HSET_PRINT_JSON     2

int  hist_init(struct histogram *h, unsigned long width, unsigned long num);
int  hist_init_oflow(struct histogram *h, unsigned long num);
void hist_destroy(struct histogram *h);
int  hist_sample(struct histogram *h, uint64_t sample);

int  hset_init(struct histoset *hs, unsigned long histos,
               unsigned long bucket_width, unsigned long num_buckets,
               unsigned long overflow);
void hset_destroy(struct histoset *hs);
void hset_print_bucket(struct histoset *hs, FILE *f, const char *pre,
                       unsigned long bucket, unsigned long flags);
void hist_print_json(struct histogram *h, FILE *f);
void hist_print_oflows(struct histogram *h, FILE *f);

#endif /* _HISTOGRAM_H */
