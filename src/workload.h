/*
 * workload.h - a host that drives the firmware with a synthetic workload
 * at a fixed queue depth and measures what it sees.
 *
 * Closed loop: the host keeps exactly `qd` commands outstanding (one per
 * NCQ tag), submitting a new one each time one completes. Latency is
 * measured from fw_submit() until the host dequeues the completion, i.e.
 * what an application would observe.
 */
#ifndef FWSIM_WORKLOAD_H
#define FWSIM_WORKLOAD_H

#include <stdint.h>

#include "fwsim.h"
#include "latency.h"

typedef enum {
    WL_RAND_WRITE,
    WL_RAND_READ,
    WL_SEQ_READ,
    WL_SEQ_WRITE,
    WL_MIXED            /* random, read_frac reads, rest writes */
} wl_kind_t;

typedef struct {
    wl_kind_t kind;
    uint64_t  ops;
    uint32_t  xfer_sectors;   /* per command                                 */
    uint32_t  qd;             /* 1 .. FW_MAX_TAGS                            */
    uint64_t  region_start;   /* LBA range the workload touches              */
    uint64_t  region_lbas;    /* 0 = whole drive                             */
    double    read_frac;      /* WL_MIXED                                    */
    double    hot_frac;       /* locality: hot_prob of accesses go to the    */
    double    hot_prob;       /* first hot_frac of the region (0 = uniform)  */
    int       fua;            /* set FUA on every write                      */
    int       final_flush;    /* issue FLUSH at the end and time it          */
    uint64_t  seed;
} wl_params_t;

typedef struct {
    uint64_t  ops, reads, writes, errors;
    uint64_t  wall_ns;          /* first submit -> last completion            */
    uint64_t  wall_ns_durable;  /* ... + the final FLUSH, if requested        */
    lat_rec_t lat_all, lat_read, lat_write;
} wl_result_t;

void wl_params_default(wl_params_t *p);
int  wl_run(fw_t *fw, const wl_params_t *p, wl_result_t *r);  /* 0 or -1 */
void wl_result_free(wl_result_t *r);
const char *wl_kind_str(wl_kind_t k);

#endif
