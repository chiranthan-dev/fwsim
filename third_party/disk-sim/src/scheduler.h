/*
 * scheduler.h - choose which queued request the drive serves next.
 *
 * Interface choice: a scheduler is a pure "pick one" function over the
 * current queue plus a tiny bit of state (sweep direction). The simulator
 * owns the queue. That keeps every policy ~20 lines, makes them trivially
 * unit-testable with a hand-built queue, and mirrors firmware, where the
 * command queue is shared and only the selection policy differs.
 *
 * SCAN and C-SCAN can also answer "don't serve anything yet, first move the
 * arm to cylinder X" (SCHED_SWEEP). That is how they differ from LOOK and
 * C-LOOK: they travel all the way to the edge of the disk even when no
 * request is waiting there.
 */
#ifndef DISKSIM_SCHEDULER_H
#define DISKSIM_SCHEDULER_H

#include <stddef.h>

#include "disksim.h"
#include "request.h"

typedef enum {
    SCHED_FCFS,
    SCHED_SSTF,
    SCHED_SCAN,
    SCHED_CSCAN,
    SCHED_LOOK,
    SCHED_CLOOK,
    SCHED_SPTF,
    SCHED_SPTF_AGED,
    SCHED_COUNT
} sched_policy_t;

typedef struct {
    sched_policy_t policy;
    int    direction;      /* +1 = toward higher cylinders (inner), -1 = outer */
    double age_limit_ms;   /* SCHED_SPTF_AGED: oldest request waiting longer
                              than this is served next, whatever its cost   */
} scheduler_t;

typedef enum { SCHED_SERVE, SCHED_SWEEP } sched_action_t;

typedef struct {
    sched_action_t action;
    size_t   index;      /* SCHED_SERVE: position in the queue  */
    uint32_t sweep_to;   /* SCHED_SWEEP: cylinder to move to    */
} sched_decision_t;

void        scheduler_init(scheduler_t *s, sched_policy_t p, double age_limit_ms);
int         scheduler_parse(const char *name, sched_policy_t *out);
const char *scheduler_name(sched_policy_t p);

/* queue must be non-empty. */
sched_decision_t scheduler_pick(scheduler_t *s, io_request_t *const *queue,
                                size_t n, const disk_t *d, double now_ms);

#endif
