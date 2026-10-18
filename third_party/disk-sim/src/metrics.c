/*
 * metrics.c
 *
 * Why percentiles and max, not just the mean: schedulers that win on
 * average (SSTF, SPTF) do it by making a few requests wait a long time.
 * The mean hides that; p99 and max wait expose it. Storage SLAs are
 * written in tail latency for exactly this reason.
 */
#include "metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double metrics_percentile(const double *sorted, size_t n, double p)
{
    if (n == 0)
        return 0.0;
    /* Nearest-rank: no interpolation, so the result is always a latency
     * that some request actually saw. */
    size_t rank = (size_t)ceil(p * (double)n);
    if (rank < 1)
        rank = 1;
    if (rank > n)
        rank = n;
    return sorted[rank - 1];
}

int metrics_compute(const io_request_t *reqs, size_t n, uint32_t sector_size,
                    const sim_stats_t *st, metrics_t *m)
{
    memset(m, 0, sizeof(*m));
    if (n == 0)
        return 0;

    double *resp = malloc(n * sizeof(*resp));
    if (!resp)
        return -1;

    double first_arrival = reqs[0].arrival_ms, last_finish = 0.0;
    double sum_resp = 0, sum_wait = 0, sum_svc = 0;
    double sum_seek = 0, sum_rot = 0, sum_xfer = 0;
    uint64_t sectors = 0;

    for (size_t i = 0; i < n; i++) {
        const io_request_t *r = &reqs[i];
        double response = r->finish_ms - r->arrival_ms;
        double wait = r->start_ms - r->arrival_ms;
        resp[i] = response;
        sum_resp += response;
        sum_wait += wait;
        sum_svc += r->finish_ms - r->start_ms;
        sum_seek += r->timing.seek_ms;
        sum_rot += r->timing.rotate_ms;
        sum_xfer += r->timing.transfer_ms;
        sectors += r->count;
        if (wait > m->max_wait_ms)
            m->max_wait_ms = wait;
        if (r->arrival_ms < first_arrival)
            first_arrival = r->arrival_ms;
        if (r->finish_ms > last_finish)
            last_finish = r->finish_ms;
    }

    qsort(resp, n, sizeof(*resp), cmp_double);

    double dn = (double)n;
    m->requests = n;
    m->makespan_ms = last_finish - first_arrival;
    m->avg_response_ms = sum_resp / dn;
    m->p50_response_ms = metrics_percentile(resp, n, 0.50);
    m->p95_response_ms = metrics_percentile(resp, n, 0.95);
    m->p99_response_ms = metrics_percentile(resp, n, 0.99);
    m->max_response_ms = resp[n - 1];
    m->avg_wait_ms = sum_wait / dn;
    m->avg_service_ms = sum_svc / dn;
    m->avg_seek_ms = sum_seek / dn;
    m->avg_rotate_ms = sum_rot / dn;
    m->avg_transfer_ms = sum_xfer / dn;
    if (m->makespan_ms > 0.0) {
        m->iops = dn / (m->makespan_ms / 1000.0);
        m->mb_per_s = ((double)sectors * sector_size / 1e6) / (m->makespan_ms / 1000.0);
    }
    if (st) {
        m->head_movement = st->head_movement;
        m->remapped_sectors = st->remapped_sectors;
        m->max_queue_len = st->max_queue_len;
    }
    m->avg_seek_distance = (double)m->head_movement / dn;

    free(resp);
    return 0;
}

void metrics_print(FILE *f, const metrics_t *m)
{
    fprintf(f, "  requests          %zu\n", m->requests);
    fprintf(f, "  makespan          %.1f ms\n", m->makespan_ms);
    fprintf(f, "  throughput        %.1f IOPS, %.2f MB/s\n", m->iops, m->mb_per_s);
    fprintf(f, "  response time     avg %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms\n",
            m->avg_response_ms, m->p50_response_ms, m->p95_response_ms,
            m->p99_response_ms, m->max_response_ms);
    fprintf(f, "  wait (queued)     avg %.2f  max %.2f ms\n", m->avg_wait_ms, m->max_wait_ms);
    fprintf(f, "  service breakdown avg %.3f ms = seek %.3f + rotate %.3f + transfer %.3f\n",
            m->avg_service_ms, m->avg_seek_ms, m->avg_rotate_ms, m->avg_transfer_ms);
    fprintf(f, "  head movement     %llu cylinders (%.1f per request)\n",
            (unsigned long long)m->head_movement, m->avg_seek_distance);
    fprintf(f, "  max queue length  %zu\n", m->max_queue_len);
    fprintf(f, "  remapped sectors  %llu\n", (unsigned long long)m->remapped_sectors);
}

void metrics_csv_header(FILE *f)
{
    fprintf(f, "tag,workload,scheduler,grown_defects,plist_defects,queue_depth,rate_iops,"
               "requests,makespan_ms,iops,mb_per_s,avg_response_ms,p50_response_ms,"
               "p95_response_ms,p99_response_ms,max_response_ms,avg_wait_ms,max_wait_ms,"
               "avg_service_ms,avg_seek_ms,avg_rotate_ms,avg_transfer_ms,"
               "head_movement_cyl,avg_seek_distance_cyl,remapped_sectors,max_queue_len\n");
}

void metrics_csv_row(FILE *f, const char *tag, const char *workload, const char *scheduler,
                     uint32_t grown_defects, uint32_t plist_defects,
                     size_t queue_depth, double rate_iops, const metrics_t *m)
{
    fprintf(f, "%s,%s,%s,%u,%u,%zu,%.3f,%zu,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
               "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%llu,%.2f,%llu,%zu\n",
            tag, workload, scheduler, grown_defects, plist_defects, queue_depth, rate_iops,
            m->requests, m->makespan_ms, m->iops, m->mb_per_s, m->avg_response_ms,
            m->p50_response_ms, m->p95_response_ms, m->p99_response_ms,
            m->max_response_ms, m->avg_wait_ms, m->max_wait_ms, m->avg_service_ms,
            m->avg_seek_ms, m->avg_rotate_ms, m->avg_transfer_ms,
            (unsigned long long)m->head_movement, m->avg_seek_distance,
            (unsigned long long)m->remapped_sectors, m->max_queue_len);
}
