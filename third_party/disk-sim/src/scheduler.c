/*
 * scheduler.c - FCFS, SSTF, SCAN, C-SCAN, LOOK, C-LOOK, SPTF, SPTF+aging.
 *
 * Every policy breaks ties by arrival time (earliest first). Without a
 * deterministic tie-break, results would depend on queue layout details,
 * and equal-cost requests would be served in an unfair order.
 *
 * The policies fall into three families:
 *   1. FCFS            - no reordering. Fair, simple, slow under load.
 *   2. Seek-only       - SSTF, SCAN, C-SCAN, LOOK, C-LOOK. They only know the
 *                        cylinder of each request (what an OS elevator
 *                        approximates by sorting LBAs). They minimise arm
 *                        travel but are blind to rotation.
 *   3. Positioning-aware - SPTF. Minimises seek + rotational latency using
 *                        the head's position AND the platter's angle. Only
 *                        the drive itself knows the angle, which is why
 *                        NCQ moved scheduling into the drive firmware.
 */
#include "scheduler.h"

#include <string.h>

static const char *names[SCHED_COUNT] = {
    "fcfs", "sstf", "scan", "cscan", "look", "clook", "sptf", "sptf-aged"
};

void scheduler_init(scheduler_t *s, sched_policy_t p, double age_limit_ms)
{
    s->policy = p;
    s->direction = +1;   /* start sweeping inward, from cylinder 0 */
    s->age_limit_ms = age_limit_ms;
}

int scheduler_parse(const char *name, sched_policy_t *out)
{
    for (int i = 0; i < SCHED_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            *out = (sched_policy_t)i;
            return 0;
        }
    }
    return -1;
}

const char *scheduler_name(sched_policy_t p)
{
    return (unsigned)p < SCHED_COUNT ? names[p] : "?";
}

/* Cylinder where a request's first sector lives (remap-aware). */
static uint32_t req_cyl(const disk_t *d, const io_request_t *r)
{
    uint32_t c = 0;
    disk_lba_cylinder(d, r->lba, &c);
    return c;
}

/* Is (cand) a better pick than (best) when both have the same key? */
static int earlier(const io_request_t *cand, const io_request_t *best)
{
    if (cand->arrival_ms != best->arrival_ms)
        return cand->arrival_ms < best->arrival_ms;
    return cand->id < best->id;
}

static size_t pick_oldest(io_request_t *const *q, size_t n)
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++)
        if (earlier(q[i], q[best]))
            best = i;
    return best;
}

/*
 * Nearest request in the given direction, counting the head's own cylinder
 * as "ahead" in both directions. Returns n if there is none.
 */
static size_t pick_ahead(io_request_t *const *q, size_t n, const disk_t *d,
                         uint32_t head, int dir)
{
    size_t best = n;
    uint32_t best_dist = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = req_cyl(d, q[i]);
        if ((dir > 0 && c < head) || (dir < 0 && c > head))
            continue;
        uint32_t dist = dir > 0 ? c - head : head - c;
        if (best == n || dist < best_dist ||
            (dist == best_dist && earlier(q[i], q[best]))) {
            best = i;
            best_dist = dist;
        }
    }
    return best;
}

static sched_decision_t serve(size_t i)
{
    sched_decision_t dec = { SCHED_SERVE, i, 0 };
    return dec;
}

static sched_decision_t sweep(uint32_t cyl)
{
    sched_decision_t dec = { SCHED_SWEEP, 0, cyl };
    return dec;
}

sched_decision_t scheduler_pick(scheduler_t *s, io_request_t *const *q,
                                size_t n, const disk_t *d, double now_ms)
{
    uint32_t head = disk_head_cylinder(d);
    disk_info_t info;
    disk_get_info(d, &info);
    uint32_t last_cyl = info.cylinders - 1;

    switch (s->policy) {
    case SCHED_FCFS:
        return serve(pick_oldest(q, n));

    case SCHED_SSTF: {
        /* Greedy: closest cylinder. Can starve far-away requests forever
         * if new nearby requests keep arriving. */
        size_t best = 0;
        uint32_t best_dist = UINT32_MAX;
        for (size_t i = 0; i < n; i++) {
            uint32_t c = req_cyl(d, q[i]);
            uint32_t dist = c > head ? c - head : head - c;
            if (dist < best_dist || (dist == best_dist && earlier(q[i], q[best]))) {
                best = i;
                best_dist = dist;
            }
        }
        return serve(best);
    }

    case SCHED_SCAN: {
        /* Elevator: keep going in one direction, all the way to the edge,
         * then reverse. Bounded wait: at most two full sweeps. */
        size_t i = pick_ahead(q, n, d, head, s->direction);
        if (i < n)
            return serve(i);
        uint32_t edge = s->direction > 0 ? last_cyl : 0;
        if (head != edge)
            return sweep(edge);
        s->direction = -s->direction;
        return serve(pick_ahead(q, n, d, head, s->direction));
    }

    case SCHED_LOOK: {
        /* SCAN, but reverse at the last request instead of the edge. */
        size_t i = pick_ahead(q, n, d, head, s->direction);
        if (i < n)
            return serve(i);
        s->direction = -s->direction;
        return serve(pick_ahead(q, n, d, head, s->direction));
    }

    case SCHED_CSCAN: {
        /* Circular SCAN: serve only on the inward pass, then fly back to
         * cylinder 0. Every cylinder gets the same treatment, so waits are
         * more uniform than SCAN (which serves the middle twice as often). */
        size_t i = pick_ahead(q, n, d, head, +1);
        if (i < n)
            return serve(i);
        return sweep(head != last_cyl ? last_cyl : 0);
    }

    case SCHED_CLOOK: {
        /* C-SCAN, but jump straight back to the lowest pending request. */
        size_t i = pick_ahead(q, n, d, head, +1);
        if (i < n)
            return serve(i);
        return serve(pick_ahead(q, n, d, 0, +1));
    }

    case SCHED_SPTF_AGED: {
        /* Real firmware never runs pure SPTF: a request at an awkward
         * position could wait indefinitely. Past the age limit, the oldest
         * request jumps the line. */
        size_t old = pick_oldest(q, n);
        if (now_ms - q[old]->arrival_ms > s->age_limit_ms)
            return serve(old);
    }
        /* fall through */
    case SCHED_SPTF: {
        /* Shortest Positioning Time First (a.k.a. rotational position
         * ordering): minimise seek + rotational latency, which is what the
         * drive actually waits on. Transfer time is excluded because it is
         * paid regardless of order. */
        size_t best = 0;
        double best_t = 0.0;
        for (size_t i = 0; i < n; i++) {
            double t = 0.0;
            disk_estimate_access(d, q[i]->lba, now_ms, &t);
            if (i == 0 || t < best_t || (t == best_t && earlier(q[i], q[best]))) {
                best = i;
                best_t = t;
            }
        }
        return serve(best);
    }

    case SCHED_COUNT:
        break;
    }
    return serve(0);
}
