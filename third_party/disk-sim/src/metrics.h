/*
 * metrics.h - turn per-request timings into the numbers we report.
 */
#ifndef DISKSIM_METRICS_H
#define DISKSIM_METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "request.h"
#include "sim.h"

typedef struct {
    size_t   requests;
    double   makespan_ms;      /* first arrival -> last completion         */
    double   avg_response_ms;  /* response = finish - arrival              */
    double   p50_response_ms;
    double   p95_response_ms;
    double   p99_response_ms;
    double   max_response_ms;
    double   avg_wait_ms;      /* wait = start - arrival (time queued)     */
    double   max_wait_ms;      /* starvation indicator                     */
    double   avg_service_ms;   /* finish - start                           */
    double   avg_seek_ms;
    double   avg_rotate_ms;
    double   avg_transfer_ms;
    double   iops;
    double   mb_per_s;         /* MB = 10^6 bytes, like drive datasheets   */
    uint64_t head_movement;    /* cylinders                                */
    double   avg_seek_distance;/* head_movement / requests                 */
    uint64_t remapped_sectors;
    size_t   max_queue_len;
} metrics_t;

/* Nearest-rank percentile of an ascending array: value at rank ceil(p*n). */
double metrics_percentile(const double *sorted, size_t n, double p);

int  metrics_compute(const io_request_t *reqs, size_t n, uint32_t sector_size,
                     const sim_stats_t *st, metrics_t *m);
void metrics_print(FILE *f, const metrics_t *m);

/* CSV: label columns describing the run (tag = experiment name), then
 * every metric. */
void metrics_csv_header(FILE *f);
void metrics_csv_row(FILE *f, const char *tag, const char *workload, const char *scheduler,
                     uint32_t grown_defects, uint32_t plist_defects,
                     size_t queue_depth, double rate_iops, const metrics_t *m);

#endif
