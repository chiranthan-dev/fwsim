#include "workload.h"

#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "pattern.h"
#include "util.h"

void wl_params_default(wl_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->kind = WL_RAND_WRITE;
    p->ops = 1000;
    p->xfer_sectors = 8;     /* 4 KiB */
    p->qd = 4;
    p->read_frac = 0.7;
    p->seed = 42;
}

const char *wl_kind_str(wl_kind_t k)
{
    switch (k) {
    case WL_RAND_WRITE: return "randwrite";
    case WL_RAND_READ:  return "randread";
    case WL_SEQ_READ:   return "seqread";
    case WL_SEQ_WRITE:  return "seqwrite";
    case WL_MIXED:      return "mixed";
    }
    return "?";
}

typedef struct {
    fw_cmd_t cmd;
    uint8_t *buf;
    int      busy;
} slot_t;

static uint64_t pick_lba(const wl_params_t *p, uint64_t region, uint64_t i,
                         uint64_t *rng)
{
    uint64_t units = region / p->xfer_sectors;   /* aligned positions */
    if (units == 0)
        units = 1;
    uint64_t u;
    if (p->kind == WL_SEQ_READ || p->kind == WL_SEQ_WRITE) {
        u = i % units;
    } else if (p->hot_prob > 0.0 && p->hot_frac > 0.0) {
        uint64_t hot = (uint64_t)((double)units * p->hot_frac);
        if (hot == 0)
            hot = 1;
        if (rng_unit64(rng) < p->hot_prob)
            u = rng_below64(rng, hot);
        else
            u = rng_below64(rng, units);
    } else {
        u = rng_below64(rng, units);
    }
    return p->region_start + u * p->xfer_sectors;
}

static int is_write(const wl_params_t *p, uint64_t *rng)
{
    switch (p->kind) {
    case WL_RAND_WRITE:
    case WL_SEQ_WRITE:
        return 1;
    case WL_MIXED:
        return rng_unit64(rng) >= p->read_frac;
    default:
        return 0;
    }
}

int wl_run(fw_t *fw, const wl_params_t *p, wl_result_t *r)
{
    memset(r, 0, sizeof(*r));
    lat_init(&r->lat_all);
    lat_init(&r->lat_read);
    lat_init(&r->lat_write);

    uint32_t qd = p->qd < 1 ? 1 : (p->qd > FW_MAX_TAGS ? FW_MAX_TAGS : p->qd);
    uint32_t ss = fw_sector_size(fw);
    uint64_t cap = fw_capacity(fw);
    uint64_t region = p->region_lbas ? p->region_lbas : cap - p->region_start;
    if (p->region_start >= cap || region > cap - p->region_start || region < p->xfer_sectors)
        return -1;

    slot_t *slots = calloc(qd, sizeof(*slots));
    if (!slots)
        return -1;
    int rc = 0;
    for (uint32_t s = 0; s < qd; s++) {
        slots[s].buf = malloc((size_t)p->xfer_sectors * ss);
        if (!slots[s].buf) {
            rc = -1;
            goto out;
        }
        /* Fill once: the throughput runs do not verify data, and refilling
         * per command would add host CPU time to every measurement. */
        for (uint32_t k = 0; k < p->xfer_sectors; k++)
            pattern_fill(slots[s].buf + (size_t)k * ss, ss, k, s + 1);
    }

    uint64_t rng = p->seed;
    uint64_t issued = 0, done = 0, outstanding = 0;
    uint64_t t0 = now_ns(), t_last = t0;

    while (done < p->ops) {
        /* Keep the queue full. */
        for (uint32_t s = 0; s < qd && issued < p->ops; s++) {
            if (slots[s].busy)
                continue;
            fw_cmd_t *c = &slots[s].cmd;
            memset(c, 0, sizeof(*c));
            c->tag = (uint8_t)s;
            c->opcode = is_write(p, &rng) ? FW_OP_WRITE : FW_OP_READ;
            c->flags = (c->opcode == FW_OP_WRITE && p->fua) ? FW_CMD_FUA : 0;
            c->lba = pick_lba(p, region, issued, &rng);
            c->count = p->xfer_sectors;
            c->buf = slots[s].buf;
            if (fw_submit(fw, c) != FW_OK) {
                rc = -1;
                goto drain;
            }
            slots[s].busy = 1;
            issued++;
            outstanding++;
        }
        fw_cmd_t *c = fw_poll(fw, 100000);
        if (!c)
            continue;
        uint64_t t = now_ns();
        uint64_t lat = t - c->t_submit_ns;
        slots[c->tag].busy = 0;
        outstanding--;
        done++;
        t_last = t;
        r->ops++;
        if (c->status != FW_OK)
            r->errors++;
        lat_add(&r->lat_all, lat);
        if (c->opcode == FW_OP_WRITE) {
            r->writes++;
            lat_add(&r->lat_write, lat);
        } else {
            r->reads++;
            lat_add(&r->lat_read, lat);
        }
    }
    r->wall_ns = t_last - t0;
    r->wall_ns_durable = r->wall_ns;

    if (p->final_flush) {
        fw_cmd_t f;
        memset(&f, 0, sizeof(f));
        f.opcode = FW_OP_FLUSH;
        f.tag = 0;
        if (fw_exec(fw, &f) != FW_OK)
            rc = -1;
        r->wall_ns_durable = now_ns() - t0;
    }

drain:
    /* Collect what is still in flight (the buffers must not be freed under
     * the firmware). Give up after ~5 s of silence: the drive is dead. */
    for (int idle = 0; outstanding > 0 && idle < 50;) {
        fw_cmd_t *c = fw_poll(fw, 100000);
        if (c) {
            slots[c->tag].busy = 0;
            outstanding--;
            idle = 0;
        } else {
            idle++;
        }
    }
out:
    for (uint32_t s = 0; s < qd; s++)
        free(slots[s].buf);
    free(slots);
    return rc;
}

void wl_result_free(wl_result_t *r)
{
    lat_free(&r->lat_all);
    lat_free(&r->lat_read);
    lat_free(&r->lat_write);
}
