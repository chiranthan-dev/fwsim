/*
 * sim.h - discrete-event simulation of a single drive with a request queue.
 */
#ifndef DISKSIM_SIM_H
#define DISKSIM_SIM_H

#include <stddef.h>
#include <stdint.h>

#include "disksim.h"
#include "request.h"
#include "scheduler.h"

typedef struct {
    sched_policy_t policy;
    double   age_limit_ms;   /* for sptf-aged                                 */
    size_t   queue_depth;    /* max commands the drive can see (NCQ = 32);
                                0 = unlimited (the whole host queue)          */
} sim_config_t;

typedef struct {
    uint64_t head_movement;     /* cylinders travelled, incl. sweeps/remaps  */
    uint64_t sweeps;            /* SCAN/C-SCAN moves with no transfer        */
    double   sweep_ms;          /* time spent on those moves                 */
    double   busy_ms;           /* time the mechanism was doing something    */
    double   end_ms;            /* completion time of the last request       */
    size_t   max_queue_len;     /* deepest the drive queue got               */
    uint64_t remapped_sectors;  /* sectors read from the G-list spare area   */
} sim_stats_t;

void sim_config_default(sim_config_t *sc);

/* Replay `reqs` (sorted by arrival) through the drive. Fills each request's
 * start/finish/timing and the aggregate stats. Returns DISK_OK or error. */
int sim_run(disk_t *d, io_request_t *reqs, size_t n, const sim_config_t *sc,
            sim_stats_t *st);

/* Inject `count` distinct random grown defects (G-list reallocations). */
int sim_inject_grown_defects(disk_t *d, uint32_t count, uint64_t seed);

/* Make `count` distinct random factory defects (P-list) within the data
 * area of a disk described by cfg. Caller frees *out. */
int sim_random_plist(const disk_config_t *cfg, uint32_t count, uint64_t seed,
                     uint64_t **out);

#endif
