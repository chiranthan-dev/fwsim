#include "powerloss.h"

#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "pattern.h"
#include "util.h"

const char *pl_mode_str(pl_mode_t m)
{
    switch (m) {
    case PL_WB_NOFLUSH: return "wb_noflush";
    case PL_WB_FLUSH:   return "wb_flush";
    case PL_WB_FUA:     return "wb_fua";
    case PL_WT:         return "write_through";
    default:            return "?";
    }
}

void pl_params_default(pl_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->mode = PL_WB_NOFLUSH;
    fw_config_default(&p->fw);
    p->blocks = 4096;
    p->block_sectors = 8;
    p->qd = 4;
    p->flush_every = 32;
    p->cut_min_ms = 5.0;
    p->cut_max_ms = 40.0;
    p->seed = 1;
}

typedef struct {
    fw_cmd_t cmd;
    uint8_t *buf;
    uint32_t block;
    uint32_t version;
    int      busy;
} pl_slot_t;

/* Host-side bookkeeping for one trial. */
typedef struct {
    const pl_params_t *p;
    pl_result_t       *r;
    uint32_t *next_ver;   /* last version submitted per block          */
    uint32_t *acked;      /* latest acknowledged version per block     */
    uint32_t *durable;    /* latest version promised durable           */
    uint32_t *snap;       /* acked[] at the moment the FLUSH was issued */
    uint8_t  *inflight;   /* a write to this block is outstanding      */
    int       flush_outstanding;
    uint64_t  acked_since_flush;
} pl_state_t;

static void on_completion(pl_state_t *s, pl_slot_t *slot)
{
    fw_cmd_t *c = &slot->cmd;
    slot->busy = 0;
    if (c->opcode == FW_OP_FLUSH) {
        s->flush_outstanding = 0;
        if (c->status == FW_OK) {
            /* FLUSH guarantees everything acknowledged BEFORE it was
             * issued - not writes acknowledged while it was running. */
            for (uint32_t b = 0; b < s->p->blocks; b++)
                if (s->snap[b] > s->durable[b])
                    s->durable[b] = s->snap[b];
            s->r->flushes_acked++;
        }
        return;
    }
    s->inflight[slot->block] = 0;
    if (c->status != FW_OK)
        return;
    /* Only one write per block is ever outstanding and the firmware is
     * FIFO, so acknowledgements per block arrive in version order. */
    s->acked[slot->block] = slot->version;
    s->r->writes_acked++;
    s->acked_since_flush++;
    if (s->p->mode == PL_WT || s->p->mode == PL_WB_FUA)
        s->durable[slot->block] = slot->version;
}

static int verify_after_reboot(pl_state_t *s)
{
    const pl_params_t *p = s->p;
    fw_config_t cfg = p->fw;
    cfg.media_create = 0;          /* same platters */
    cfg.time_scale = 0.0;          /* verification needs no realism */
    cfg.fault_read_flip_prob = 0.0;
    cfg.idle_destage_ms = 0.0;
    fw_t *fw = fw_create(&cfg, NULL);
    if (!fw)
        return -1;
    uint32_t ss = fw_sector_size(fw);
    uint8_t *buf = malloc((size_t)p->block_sectors * ss);
    if (!buf) {
        fw_destroy(fw);
        return -1;
    }

    for (uint32_t b = 0; b < p->blocks; b++) {
        if (s->acked[b] == 0 && s->durable[b] == 0)
            continue;
        s->r->blocks_checked++;
        fw_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = FW_OP_READ;
        c.lba = (uint64_t)b * p->block_sectors;
        c.count = p->block_sectors;
        c.buf = buf;

        /* The block's version on media = the OLDEST version among its
         * sectors: it only counts as surviving if every sector does. */
        uint32_t minver = UINT32_MAX;
        int bad = fw_exec(fw, &c) != FW_OK;
        for (uint32_t k = 0; k < p->block_sectors && !bad; k++) {
            uint32_t v = 0;
            int pc = pattern_check(buf + (size_t)k * ss, ss, c.lba + k, &v);
            if (pc == PAT_BAD)
                bad = 1;
            else if (pc == PAT_ZERO)
                v = 0;
            if (v < minver)
                minver = v;
        }
        if (bad) {
            s->r->unreadable++;
            minver = 0;
        }
        if (minver < s->acked[b])
            s->r->lost++;
        if (minver < s->durable[b])
            s->r->violations++;
        if (minver > s->acked[b])
            s->r->newer_than_ack++;
    }
    free(buf);
    fw_shutdown(fw);
    fw_destroy(fw);
    return 0;
}

int pl_run_trial(const pl_params_t *p, pl_result_t *r)
{
    memset(r, 0, sizeof(*r));
    if (p->qd < 1 || p->qd > FW_MAX_TAGS || p->blocks == 0 || p->block_sectors == 0)
        return -1;

    fw_config_t cfg = p->fw;
    cfg.media_create = 1;
    cfg.write_cache = (p->mode != PL_WT);

    pl_state_t s;
    memset(&s, 0, sizeof(s));
    s.p = p;
    s.r = r;
    s.next_ver = calloc(p->blocks, sizeof(uint32_t));
    s.acked = calloc(p->blocks, sizeof(uint32_t));
    s.durable = calloc(p->blocks, sizeof(uint32_t));
    s.snap = calloc(p->blocks, sizeof(uint32_t));
    s.inflight = calloc(p->blocks, 1);
    pl_slot_t *slots = calloc(p->qd, sizeof(*slots));
    fw_t *fw = NULL;
    int rc = -1;
    if (!s.next_ver || !s.acked || !s.durable || !s.snap || !s.inflight || !slots)
        goto out;

    fw = fw_create(&cfg, NULL);
    if (!fw)
        goto out;
    uint32_t ss = fw_sector_size(fw);
    if ((uint64_t)p->blocks * p->block_sectors > fw_capacity(fw))
        goto out;
    for (uint32_t i = 0; i < p->qd; i++) {
        slots[i].buf = malloc((size_t)p->block_sectors * ss);
        if (!slots[i].buf)
            goto out;
    }

    uint64_t rng = p->seed * 0x9E3779B97F4A7C15ull + 7;
    double span = p->cut_max_ms - p->cut_min_ms;
    double cut_ms = p->cut_min_ms + (span > 0 ? span * rng_unit64(&rng) : 0.0);
    uint64_t t0 = now_ns();
    uint64_t cut_at = t0 + (uint64_t)(cut_ms * 1e6);

    for (;;) {
        uint64_t t = now_ns();
        if (t >= cut_at)
            break;
        for (uint32_t i = 0; i < p->qd; i++) {
            pl_slot_t *sl = &slots[i];
            if (sl->busy)
                continue;
            fw_cmd_t *c = &sl->cmd;
            memset(c, 0, sizeof(*c));
            c->tag = (uint8_t)i;
            if (p->mode == PL_WB_FLUSH && !s.flush_outstanding &&
                s.acked_since_flush >= p->flush_every) {
                memcpy(s.snap, s.acked, p->blocks * sizeof(uint32_t));
                c->opcode = FW_OP_FLUSH;
                if (fw_submit(fw, c) != FW_OK)
                    goto cut;
                s.flush_outstanding = 1;
                s.acked_since_flush = 0;
                sl->busy = 1;
                continue;
            }
            /* Pick a block with no write in flight (keeps per-block
             * version order unambiguous). */
            uint32_t b = 0;
            int found = 0;
            for (int tries = 0; tries < 16 && !found; tries++) {
                b = (uint32_t)rng_below64(&rng, p->blocks);
                found = !s.inflight[b];
            }
            if (!found)
                continue;
            uint32_t ver = ++s.next_ver[b];
            uint64_t lba = (uint64_t)b * p->block_sectors;
            for (uint32_t k = 0; k < p->block_sectors; k++)
                pattern_fill(sl->buf + (size_t)k * ss, ss, lba + k, ver);
            c->opcode = FW_OP_WRITE;
            c->flags = (p->mode == PL_WB_FUA) ? FW_CMD_FUA : 0;
            c->lba = lba;
            c->count = p->block_sectors;
            c->buf = sl->buf;
            if (fw_submit(fw, c) != FW_OK)
                goto cut;
            sl->block = b;
            sl->version = ver;
            sl->busy = 1;
            s.inflight[b] = 1;
            r->writes_submitted++;
        }
        uint64_t left_us = (cut_at - t) / 1000;
        fw_cmd_t *c = fw_poll(fw, left_us < 200 ? left_us : 200);
        if (c)
            on_completion(&s, &slots[c->tag]);
    }

cut:
    r->run_wall_ms = (double)(now_ns() - t0) / 1e6;
    fw_power_cut(fw);
    {
        fw_stats_t st;
        fw_get_stats(fw, &st);
        r->dirty_at_cut = st.cache_dirty;
        r->torn_writes = st.torn_writes;
    }
    /* Completions posted before the cut reached the host: they count as
     * acknowledged. Anything still in the submission queue never will. */
    for (fw_cmd_t *c; (c = fw_poll(fw, 0)) != NULL;)
        on_completion(&s, &slots[c->tag]);
    fw_destroy(fw);       /* DRAM contents are gone */
    fw = NULL;

    rc = verify_after_reboot(&s);

out:
    if (fw)
        fw_destroy(fw);
    if (slots)
        for (uint32_t i = 0; i < p->qd; i++)
            free(slots[i].buf);
    free(slots);
    free(s.next_ver);
    free(s.acked);
    free(s.durable);
    free(s.snap);
    free(s.inflight);
    return rc;
}
