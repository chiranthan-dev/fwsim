/*
 * sim.c - the event loop.
 *
 * Model: one actuator, one request in flight at a time, non-preemptive.
 * That matches a real single-actuator drive: once the head commits to a
 * seek, it finishes that command.
 *
 * Instead of a general event heap we only ever need two kinds of event,
 * "next arrival" and "drive becomes free", so the loop just advances a
 * clock between them. Simpler to read and to reason about, same answers.
 *
 *   t = 0
 *   loop:
 *     admit every request that has arrived by t (up to queue_depth)
 *     if the drive queue is empty: jump t to the next arrival
 *     else ask the scheduler which one, service it, t = completion time
 */
#include "sim.h"

#include <stdlib.h>
#include <string.h>

#include "rng.h"

void sim_config_default(sim_config_t *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->policy = SCHED_FCFS;
    sc->age_limit_ms = 500.0;
    sc->queue_depth = 0;
}

int sim_run(disk_t *d, io_request_t *reqs, size_t n, const sim_config_t *sc,
            sim_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    if (n == 0)
        return DISK_OK;

    /* The drive's command queue. Requests that have arrived but do not fit
     * (queue_depth reached) wait in the host, i.e. stay at reqs[next..]. */
    io_request_t **queue = malloc(n * sizeof(*queue));
    if (!queue)
        return DISK_ENOMEM;

    scheduler_t sched;
    scheduler_init(&sched, sc->policy, sc->age_limit_ms);

    size_t qlen = 0, next = 0, done = 0;
    double t = 0.0;
    int rc = DISK_OK;

    while (done < n) {
        while (next < n && reqs[next].arrival_ms <= t &&
               (sc->queue_depth == 0 || qlen < sc->queue_depth))
            queue[qlen++] = &reqs[next++];
        if (qlen > st->max_queue_len)
            st->max_queue_len = qlen;

        if (qlen == 0) {
            t = reqs[next].arrival_ms;   /* idle: fast-forward */
            continue;
        }

        sched_decision_t dec = scheduler_pick(&sched, queue, qlen, d, t);
        disk_timing_t tm;

        if (dec.action == SCHED_SWEEP) {
            rc = disk_seek(d, dec.sweep_to, &tm);
            if (rc != DISK_OK)
                break;
            t += tm.total_ms;
            st->sweeps++;
            st->sweep_ms += tm.total_ms;
            st->busy_ms += tm.total_ms;
            st->head_movement += tm.cylinders_moved;
            continue;   /* new requests may have arrived during the sweep */
        }

        io_request_t *r = queue[dec.index];
        /* Remove while keeping arrival order (FCFS and tie-breaks rely on
         * it being cheap to find the oldest). O(n) but n is the queue
         * length, not the workload length. */
        memmove(&queue[dec.index], &queue[dec.index + 1],
                (qlen - dec.index - 1) * sizeof(*queue));
        qlen--;

        disk_request_t dr = { r->lba, r->count, r->op };
        rc = disk_service_request(d, &dr, t, &tm);
        if (rc != DISK_OK)
            break;
        r->start_ms = t;
        r->finish_ms = t + tm.total_ms;
        r->timing = tm;
        t = r->finish_ms;

        st->busy_ms += tm.total_ms;
        st->head_movement += tm.cylinders_moved;
        st->remapped_sectors += tm.remapped_sectors;
        done++;
    }

    st->end_ms = t;
    free(queue);
    return rc;
}

int sim_inject_grown_defects(disk_t *d, uint32_t count, uint64_t seed)
{
    disk_info_t info;
    disk_get_info(d, &info);
    if (count > info.spare_total - info.spare_used)
        return DISK_ENOSPC;
    if (count > info.capacity_lbas)
        return DISK_EINVAL;

    rng_t rng;
    rng_seed(&rng, seed);
    uint32_t added = 0;
    while (added < count) {
        uint64_t lba = rng_below(&rng, info.capacity_lbas);
        if (disk_is_remapped(d, lba))
            continue;   /* want `count` distinct defective LBAs */
        int rc = disk_add_grown_defect(d, lba);
        if (rc != DISK_OK)
            return rc;
        added++;
    }
    return DISK_OK;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int sim_random_plist(const disk_config_t *cfg, uint32_t count, uint64_t seed,
                     uint64_t **out)
{
    *out = NULL;
    if (count == 0)
        return DISK_OK;

    /* Open a defect-free twin just to learn where the data area ends. */
    disk_config_t probe = *cfg;
    probe.plist = NULL;
    probe.plist_len = 0;
    int rc;
    disk_t *d = disk_open(&probe, &rc);
    if (!d)
        return rc;
    disk_info_t info;
    disk_get_info(d, &info);
    disk_close(d);

    uint64_t data_pbas = info.total_pbas - info.spare_total;
    if (count >= data_pbas)
        return DISK_EINVAL;

    uint64_t *p = malloc((size_t)count * sizeof(*p));
    if (!p)
        return DISK_ENOMEM;

    rng_t rng;
    rng_seed(&rng, seed);
    /* Draw, sort, and redraw duplicates until all are distinct. With
     * count << data_pbas this converges in one or two rounds. */
    size_t have = 0;
    while (have < count) {
        while (have < count)
            p[have++] = rng_below(&rng, data_pbas);
        qsort(p, have, sizeof(*p), cmp_u64);
        size_t u = 0;
        for (size_t i = 0; i < have; i++)
            if (u == 0 || p[u - 1] != p[i])
                p[u++] = p[i];
        have = u;
    }
    *out = p;
    return DISK_OK;
}
