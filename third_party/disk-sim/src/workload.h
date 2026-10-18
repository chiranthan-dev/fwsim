/*
 * workload.h - generate or load the stream of host requests.
 *
 * Every workload is materialised up front as an array sorted by arrival
 * time. The simulator then replays it. Generating ahead (instead of on the
 * fly) means every scheduler sees the exact same requests at the exact same
 * times, so differences in the results come from the scheduler alone.
 */
#ifndef DISKSIM_WORKLOAD_H
#define DISKSIM_WORKLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "request.h"

typedef enum {
    WL_SEQ,       /* one stream reading consecutive LBAs                    */
    WL_RANDOM,    /* uniform over the whole LBA space                       */
    WL_HOTCOLD,   /* hot_prob of requests land in the first hot_frac of LBAs */
    WL_TRACE      /* replay a CSV: arrival_time_ms,op,lba,length            */
} workload_kind_t;

typedef struct {
    workload_kind_t kind;
    size_t      num_requests;
    double      rate_iops;      /* mean Poisson arrival rate; <= 0: all at t=0 */
    uint32_t    req_sectors;    /* size of each generated request              */
    double      start_frac;     /* WL_SEQ: start at this fraction of capacity  */
    double      hot_frac;       /* WL_HOTCOLD: size of hot region (0.2)        */
    double      hot_prob;       /* WL_HOTCOLD: share of requests that hit it   */
    double      write_frac;     /* share of writes (timing is identical)       */
    uint64_t    seed;
    const char *trace_path;     /* WL_TRACE                                    */
} workload_config_t;

void workload_config_default(workload_config_t *wc);

int         workload_parse_kind(const char *s, workload_kind_t *out);
const char *workload_kind_name(workload_kind_t k);

/* Build the request array (caller frees *out). Returns 0, or -1 with a
 * message in err. */
int workload_build(const workload_config_t *wc, uint64_t capacity,
                   io_request_t **out, size_t *n, char *err, size_t errlen);

int workload_load_trace(const char *path, uint64_t capacity,
                        io_request_t **out, size_t *n, char *err, size_t errlen);

/* Write requests in the trace CSV format (round-trips with load_trace). */
int workload_write_trace(const char *path, const io_request_t *reqs, size_t n);

#endif
