/*
 * latency.h - record per-command latencies and summarise them as
 * average / p50 / p95 / p99 / max.
 *
 * WHY percentiles and not just the average: storage latency distributions
 * have long tails (a cache hit is 100x faster than a seek, and a forced
 * destage stalls one unlucky command for many milliseconds). An average
 * hides that tail; p99 is what a database or a user actually notices.
 *
 * Percentiles use the nearest-rank method on the sorted samples: exact,
 * simple, and no interpolation to argue about.
 */
#ifndef FWSIM_LATENCY_H
#define FWSIM_LATENCY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t *v;
    size_t    n, cap;
} lat_rec_t;

typedef struct {
    size_t n;
    double avg_ns, p50_ns, p95_ns, p99_ns, max_ns;
} lat_summary_t;

void lat_init(lat_rec_t *r);
void lat_free(lat_rec_t *r);
int  lat_add(lat_rec_t *r, uint64_t ns);      /* 0 or -1 (ENOMEM) */
/* Sorts the samples in place. All zeros if there are no samples. */
void lat_summarize(lat_rec_t *r, lat_summary_t *s);

#endif
