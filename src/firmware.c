/*
 * firmware.c - the firmware main loop, opcode dispatch, write-back cache
 * policy (destaging, read-ahead) and the host-facing API.
 *
 * Execution model: ONE firmware thread does all the work - dequeue, decode,
 * cache, media, completion - like the single real-time core that runs the
 * command layer on many drive controllers. Everything the firmware thread
 * owns (cache, media, read-ahead state) therefore needs no locks. The only
 * shared state is:
 *   - the two queues (their own synchronisation, see ring_*.h),
 *   - power_on / stop_req flags (atomics),
 *   - the tag-busy bitmap (atomic fetch_or / fetch_and),
 *   - the statistics counters (single-writer atomics, see util.h).
 */
#include "fwsim.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "clock.h"
#include "media.h"
#include "squeue.h"

/* Largest transfer a command may request (like MDTS / max sectors per
 * command). Bounds the staging buffer and keeps one command from
 * monopolising the drive. */
#define FW_MAX_XFER_SECTORS 2048u

/* Queue sizes. The submission queue can never overflow because at most
 * FW_MAX_TAGS commands are outstanding (the tag bitmap enforces it); the
 * completion queue gets the same bound. Powers of two for the SPSC ring. */
#define FW_SQ_DEPTH FW_MAX_TAGS
#define FW_CQ_DEPTH FW_MAX_TAGS

struct fw {
    fw_config_t   cfg;
    char         *media_path;      /* owned copy of cfg.media_path */
    media_t       media;
    cache_t       cache;
    squeue_t      sq;              /* host -> firmware */
    squeue_t      cq;              /* firmware -> host */
    pthread_t     thread;
    int           thread_started;
    int           thread_joined;

    atomic_int    power_on;        /* 1 while the drive has power            */
    atomic_int    stop_req;        /* clean shutdown requested               */
    _Atomic uint32_t tags_busy;    /* bit t set while tag t is outstanding   */

    /* --- owned by the firmware thread --- */
    int             wce;            /* write cache enabled (write-back)      */
    int             ra_enabled;
    uint64_t        ra_next_lba;    /* where a sequential stream would continue */
    cache_entry_t **dirty_buf;      /* scratch for destaging                 */
    uint8_t        *xfer;           /* staging buffer for coalesced writes   */
    uint64_t        capacity;
    uint32_t        ss;             /* sector size                           */
    size_t          high_wm, low_wm;

    fw_counters_t   ctr;
};

typedef fw_status_t (*fw_handler_t)(fw_t *fw, fw_cmd_t *cmd);

/* ---- configuration -------------------------------------------------------- */

void fw_config_default(fw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->media_path = "fwsim.media";
    cfg->media_create = 1;
    disk_config_default(&cfg->disk);
    cfg->capacity_lbas = 0;
    cfg->time_scale = 0.0;
    cfg->cache_entries = 4096;        /* 2 MiB of 512-byte sectors          */
    cfg->write_cache = 1;
    cfg->destage_high = 0.75;
    cfg->destage_low = 0.50;
    cfg->destage_sorted = 1;
    cfg->idle_destage_ms = 50.0;
    cfg->idle_destage_batch = 64;
    cfg->read_ahead_sectors = 256;    /* 128 KiB                            */
    cfg->read_retries = 3;
    cfg->fault_read_flip_prob = 0.0;
    cfg->seed = 1;
}

/* ---- helpers --------------------------------------------------------------- */

static int powered(fw_t *fw)
{
    return atomic_load_explicit(&fw->power_on, memory_order_acquire) != 0;
}

static void update_cache_gauges(fw_t *fw)
{
    ctr_set(&fw->ctr.cache_dirty, fw->cache.dirty_count);
    ctr_set(&fw->ctr.cache_used, fw->cache.count);
}

static fw_status_t check_range(fw_t *fw, fw_cmd_t *cmd)
{
    if (cmd->count == 0 || cmd->count > FW_MAX_XFER_SECTORS || cmd->buf == NULL)
        return FW_ERR_INVALID_FIELD;
    if (cmd->lba >= fw->capacity || cmd->count > fw->capacity - cmd->lba) {
        cmd->err_lba = cmd->lba;
        return FW_ERR_INVALID_LBA;
    }
    return FW_OK;
}

/* ---- destaging --------------------------------------------------------------
 *
 * Destaging = writing dirty cache entries back to the media. Three triggers
 * (threshold, FLUSH, idle) share this one function. Two optimisations:
 *
 *  1. Sort the batch by LBA. Dirty data arrives in random order; writing it
 *     in LBA order turns N random seeks into one sweep across the disk
 *     (the elevator / SCAN idea). The bench measures sorted vs age order.
 *  2. Coalesce adjacent LBAs into one media write: one seek + rotation for
 *     the whole run instead of one per sector.
 */

static int cmp_entry_lba(const void *a, const void *b)
{
    const cache_entry_t *x = *(cache_entry_t *const *)a;
    const cache_entry_t *y = *(cache_entry_t *const *)b;
    return (x->lba > y->lba) - (x->lba < y->lba);
}

static fw_status_t destage(fw_t *fw, size_t max)
{
    size_t n = cache_collect_dirty(&fw->cache, fw->dirty_buf, max);
    if (n == 0)
        return FW_OK;
    if (fw->cfg.destage_sorted)
        qsort(fw->dirty_buf, n, sizeof(*fw->dirty_buf), cmp_entry_lba);

    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && j - i < FW_MAX_XFER_SECTORS &&
               fw->dirty_buf[j]->lba == fw->dirty_buf[j - 1]->lba + 1)
            j++;
        for (size_t k = i; k < j; k++)
            memcpy(fw->xfer + (k - i) * fw->ss, fw->dirty_buf[k]->data, fw->ss);

        fw_status_t st = media_write(&fw->media, fw->dirty_buf[i]->lba,
                                     (uint32_t)(j - i), fw->xfer);
        if (st != FW_OK)
            return st;     /* power cut or write fault: entries stay dirty */

        for (size_t k = i; k < j; k++)
            cache_mark_clean(&fw->cache, fw->dirty_buf[k]);
        ctr_add(&fw->ctr.destage_runs, 1);
        ctr_add(&fw->ctr.destaged_sectors, j - i);
        i = j;
    }
    update_cache_gauges(fw);
    return FW_OK;
}

static fw_status_t destage_all(fw_t *fw)
{
    while (fw->cache.dirty_count > 0) {
        fw_status_t st = destage(fw, fw->cache.dirty_count);
        if (st != FW_OK)
            return st;
    }
    return FW_OK;
}

/* ---- command handlers (the dispatch table targets) ------------------------ */

static fw_status_t h_identify(fw_t *fw, fw_cmd_t *cmd)
{
    if (cmd->buf == NULL)
        return FW_ERR_INVALID_FIELD;
    fw_identify_t id;
    memset(&id, 0, sizeof(id));
    snprintf(id.model, sizeof(id.model), "FWSIM simulated HDD (disk-sim media)");
    snprintf(id.serial, sizeof(id.serial), "FWSIM-%08llX",
             (unsigned long long)(fw->cfg.seed & 0xFFFFFFFFull));
    snprintf(id.firmware_rev, sizeof(id.firmware_rev), "1.0");
    id.capacity_lbas = fw->capacity;
    id.sector_size = fw->ss;
    id.queue_depth = FW_MAX_TAGS;
    id.cache_entries = (uint32_t)fw->cfg.cache_entries;
    id.write_cache_enabled = (uint8_t)fw->wce;
    id.read_ahead_enabled = (uint8_t)fw->ra_enabled;
    id.fua_supported = 1;
    snprintf(id.queue_impl, sizeof(id.queue_impl), "%s", SQUEUE_IMPL);
    /* IDENTIFY always transfers exactly one sector, zero-padded. */
    memset(cmd->buf, 0, fw->ss);
    memcpy(cmd->buf, &id, sizeof(id));
    return FW_OK;
}

static fw_status_t h_read(fw_t *fw, fw_cmd_t *cmd)
{
    fw_status_t st = check_range(fw, cmd);
    if (st != FW_OK)
        return st;

    uint8_t *out = cmd->buf;
    uint32_t i = 0;
    while (i < cmd->count) {
        cache_entry_t *e = cache_lookup(&fw->cache, cmd->lba + i, true);
        if (e) {
            /* Hit - possibly a DIRTY entry: the cache holds the newest
             * data, the media an older version. Serving from cache is
             * what keeps write-back coherent. */
            memcpy(out + (size_t)i * fw->ss, e->data, fw->ss);
            ctr_add(&fw->ctr.read_hit_sectors, 1);
            i++;
            continue;
        }
        /* Miss: gather the whole run of consecutive misses into ONE media
         * access (one seek instead of one per sector). */
        uint32_t j = i + 1;
        while (j < cmd->count && !cache_lookup(&fw->cache, cmd->lba + j, false))
            j++;
        uint64_t bad = 0;
        st = media_read(&fw->media, cmd->lba + i, j - i, out + (size_t)i * fw->ss, &bad);
        if (st != FW_OK) {
            cmd->err_lba = (st == FW_ERR_MEDIUM) ? bad : cmd->lba + i;
            return st;
        }
        ctr_add(&fw->ctr.read_miss_sectors, j - i);
        for (uint32_t k = i; k < j; k++)   /* NULL (cache all dirty) = don't cache */
            (void)cache_insert(&fw->cache, cmd->lba + k, out + (size_t)k * fw->ss, false);
        i = j;
    }
    return FW_OK;
}

static fw_status_t h_write(fw_t *fw, fw_cmd_t *cmd)
{
    fw_status_t st = check_range(fw, cmd);
    if (st != FW_OK)
        return st;
    const uint8_t *in = cmd->buf;

    if (!fw->wce || (cmd->flags & FW_CMD_FUA)) {
        /*
         * Write-through, or FUA in write-back mode: the data must be on
         * the media BEFORE we acknowledge. Then keep a clean copy in the
         * cache for future reads; that also supersedes any older dirty
         * copy of the same sectors (destaging it later would overwrite
         * the newer data on the media with stale data).
         */
        st = media_write(&fw->media, cmd->lba, cmd->count, in);
        if (st != FW_OK) {
            cmd->err_lba = cmd->lba;
            return st;
        }
        for (uint32_t i = 0; i < cmd->count; i++) {
            if (cache_lookup(&fw->cache, cmd->lba + i, false))
                ctr_add(&fw->ctr.write_hit_sectors, 1);
            (void)cache_insert(&fw->cache, cmd->lba + i, in + (size_t)i * fw->ss, false);
        }
        return FW_OK;
    }

    /* Write-back: store in DRAM, mark dirty, acknowledge. The data is NOT
     * durable until destaged - exactly what the power-loss test shows. */
    for (uint32_t i = 0; i < cmd->count; i++) {
        uint64_t lba = cmd->lba + i;
        if (cache_lookup(&fw->cache, lba, false))
            ctr_add(&fw->ctr.write_hit_sectors, 1);
        while (!cache_insert(&fw->cache, lba, in + (size_t)i * fw->ss, true)) {
            /* Every entry is dirty: nothing can be evicted. Destage the
             * oldest eighth synchronously. This stalls the command - which
             * is why the threshold destage below tries never to get here. */
            ctr_add(&fw->ctr.forced_destages, 1);
            size_t batch = fw->cache.capacity / 8 ? fw->cache.capacity / 8 : 1;
            st = destage(fw, batch);
            if (st != FW_OK) {
                cmd->err_lba = lba;
                return st;
            }
        }
    }
    return FW_OK;
}

static fw_status_t h_flush(fw_t *fw, fw_cmd_t *cmd)
{
    (void)cmd;
    ctr_add(&fw->ctr.flushes, 1);
    return destage_all(fw);
}

static fw_status_t h_set_features(fw_t *fw, fw_cmd_t *cmd)
{
    if (cmd->feature_value > 1)
        return FW_ERR_INVALID_FIELD;
    switch (cmd->feature) {
    case FW_FEAT_WRITE_CACHE:
        if (!cmd->feature_value && fw->wce) {
            /* Turning the write cache off must not strand dirty data that
             * the host was already told is written. */
            fw_status_t st = destage_all(fw);
            if (st != FW_OK)
                return st;
        }
        fw->wce = (int)cmd->feature_value;
        return FW_OK;
    case FW_FEAT_READ_AHEAD:
        fw->ra_enabled = (int)cmd->feature_value;
        return FW_OK;
    default:
        return FW_ERR_INVALID_FIELD;
    }
}

/*
 * Dispatch table indexed by opcode. WHY a table of function pointers
 * instead of a switch: adding a command is one line here plus a handler,
 * the decode cost is one bounds check + one indirect call regardless of
 * how many opcodes exist, and unsupported opcodes are simply NULL. This is
 * the usual structure of a firmware command layer.
 */
static const fw_handler_t dispatch[FW_OP_COUNT] = {
    [FW_OP_IDENTIFY]     = h_identify,
    [FW_OP_READ]         = h_read,
    [FW_OP_WRITE]        = h_write,
    [FW_OP_FLUSH]        = h_flush,
    [FW_OP_SET_FEATURES] = h_set_features,
};

/* ---- background work after a command ---------------------------------------- */

/*
 * Sequential read-ahead. If this read started exactly where the previous
 * one ended, the host is probably streaming: read the next
 * read_ahead_sectors into the cache now, while the head is already there
 * (no seek, almost no rotational wait), so the next read is a cache hit.
 * Done AFTER the completion is posted, so it never delays the host.
 */
static void read_ahead(fw_t *fw, fw_status_t st, uint64_t lba, uint32_t count)
{
    int sequential = (lba == fw->ra_next_lba);
    fw->ra_next_lba = lba + count;
    if (!sequential || !fw->ra_enabled || fw->cfg.read_ahead_sectors == 0 ||
        st != FW_OK)
        return;

    uint64_t start = lba + count;
    uint64_t end = start + fw->cfg.read_ahead_sectors;
    if (end > fw->capacity)
        end = fw->capacity;
    while (start < end && cache_lookup(&fw->cache, start, false))
        start++;                                /* already prefetched */
    uint64_t stop = start;
    while (stop < end && stop - start < FW_MAX_XFER_SECTORS &&
           !cache_lookup(&fw->cache, stop, false))
        stop++;
    if (stop == start)
        return;

    uint64_t bad;
    uint32_t n = (uint32_t)(stop - start);
    if (media_read(&fw->media, start, n, fw->xfer, &bad) != FW_OK)
        return;   /* errors surface if/when the host actually reads it */
    for (uint32_t k = 0; k < n; k++)
        (void)cache_insert(&fw->cache, start + k, fw->xfer + (size_t)k * fw->ss, false);
    ctr_add(&fw->ctr.readahead_ops, 1);
    ctr_add(&fw->ctr.readahead_sectors, n);
}

static void threshold_destage(fw_t *fw)
{
    if (fw->wce && fw->cache.dirty_count >= fw->high_wm) {
        /* Hysteresis: destage down to the LOW mark, not just below the
         * high one. Otherwise every single write would trigger a tiny
         * one-sector destage, and the media would never get a batch large
         * enough to sort into an efficient sweep. */
        ctr_add(&fw->ctr.threshold_destages, 1);
        (void)destage(fw, fw->cache.dirty_count - fw->low_wm);
    }
}

/* ---- the main loop ----------------------------------------------------------- */

static void complete(fw_t *fw, fw_cmd_t *cmd, fw_status_t st)
{
    cmd->status = st;
    cmd->sense = fw_status_sense(st);
    cmd->state = FW_CMD_COMPLETED;
    cmd->t_done_ns = now_ns();
    if (cmd->opcode < FW_OP_COUNT)
        ctr_add(&fw->ctr.cmds[cmd->opcode], 1);
    if (st != FW_OK)
        ctr_add(&fw->ctr.cmd_errors, 1);

    /* Free the tag BEFORE posting: once the host sees the completion it
     * may immediately reuse the tag, and must not get TAG_IN_USE. After the
     * push, the command belongs to the host again: do not touch it. */
    atomic_fetch_and_explicit(&fw->tags_busy, ~(1u << cmd->tag), memory_order_release);
    if (!sq_push(&fw->cq, cmd))
        fprintf(stderr, "fwsim: completion queue closed, completion dropped\n");
}

static void process(fw_t *fw, fw_cmd_t *cmd)
{
    cmd->state = FW_CMD_ACTIVE;
    cmd->t_start_ns = now_ns();
    cmd->err_lba = 0;

    /* Copy what background work needs: after complete() the host owns cmd. */
    uint8_t op = cmd->opcode;
    uint64_t lba = cmd->lba;
    uint32_t count = cmd->count;

    fw_status_t st;
    if (op >= FW_OP_COUNT || dispatch[op] == NULL)
        st = FW_ERR_INVALID_OPCODE;
    else
        st = dispatch[op](fw, cmd);

    /* Power died while this command was in flight: it is never completed.
     * The host will never see an acknowledgement for it. */
    if (!powered(fw))
        return;

    update_cache_gauges(fw);
    complete(fw, cmd, st);

    if (op == FW_OP_READ)
        read_ahead(fw, st, lba, count);
    else if (op == FW_OP_WRITE)
        threshold_destage(fw);
    update_cache_gauges(fw);
}

static uint64_t idle_wait_ns(fw_t *fw)
{
    /* Wake up periodically to notice shutdown even with nothing to do. */
    uint64_t wait = 1000000;  /* 1 ms */
    if (fw->wce && fw->cfg.idle_destage_ms > 0.0 && fw->cache.dirty_count > 0) {
        wait = (uint64_t)(fw->cfg.idle_destage_ms * 1e6 * fw->cfg.time_scale);
        if (wait < 20000)
            wait = 20000;     /* 20 us floor so time_scale 0 still idles */
    }
    return wait;
}

static void *fw_main(void *arg)
{
    fw_t *fw = arg;
    for (;;) {
        if (!powered(fw))
            break;
        void *item;
        if (sq_pop_timed(&fw->sq, &item, idle_wait_ns(fw))) {
            if (!powered(fw))
                break;        /* dequeued at the instant of power loss */
            process(fw, item);
            continue;
        }
        if (!powered(fw))
            break;
        if (atomic_load_explicit(&fw->stop_req, memory_order_acquire)) {
            void *late;
            if (sq_try_pop(&fw->sq, &late)) {   /* drain before stopping */
                process(fw, late);
                continue;
            }
            (void)destage_all(fw);   /* like STANDBY IMMEDIATE */
            break;
        }
        /* Idle: the host has gone quiet. Use the time to destage a batch,
         * shrinking the window in which a power cut could lose data. */
        if (fw->wce && fw->cfg.idle_destage_ms > 0.0 && fw->cache.dirty_count > 0) {
            ctr_add(&fw->ctr.idle_destages, 1);
            (void)destage(fw, fw->cfg.idle_destage_batch);
        }
    }
    return NULL;
}

/* ---- public API ---------------------------------------------------------------- */

fw_t *fw_create(const fw_config_t *cfg, int *err)
{
    int rc = -ENOMEM;
    fw_t *fw = calloc(1, sizeof(*fw));
    if (!fw)
        goto out;
    fw->cfg = *cfg;
    fw->media_path = strdup(cfg->media_path ? cfg->media_path : "fwsim.media");
    if (!fw->media_path)
        goto fail_free;
    fw->cfg.media_path = fw->media_path;

    rc = -EINVAL;
    if (cfg->cache_entries == 0 || cfg->destage_low < 0.0 ||
        cfg->destage_high > 1.0 || cfg->destage_low > cfg->destage_high ||
        cfg->time_scale < 0.0)
        goto fail_free;

    atomic_init(&fw->power_on, 1);
    atomic_init(&fw->stop_req, 0);
    atomic_init(&fw->tags_busy, 0);

    rc = media_open(&fw->media, &fw->cfg, &fw->power_on, &fw->ctr);
    if (rc != 0)
        goto fail_free;
    fw->capacity = fw->media.capacity;
    fw->ss = fw->media.sector_size;

    rc = -ENOMEM;
    if (cache_init(&fw->cache, cfg->cache_entries, fw->ss) != 0)
        goto fail_media;
    fw->dirty_buf = calloc(cfg->cache_entries, sizeof(*fw->dirty_buf));
    fw->xfer = malloc((size_t)FW_MAX_XFER_SECTORS * fw->ss);
    if (!fw->dirty_buf || !fw->xfer)
        goto fail_cache;
    if (sq_init(&fw->sq, FW_SQ_DEPTH) != 0)
        goto fail_cache;
    if (sq_init(&fw->cq, FW_CQ_DEPTH) != 0)
        goto fail_sq;

    fw->wce = cfg->write_cache != 0;
    fw->ra_enabled = cfg->read_ahead_sectors > 0;
    fw->ra_next_lba = UINT64_MAX;
    fw->high_wm = (size_t)(cfg->destage_high * (double)cfg->cache_entries);
    fw->low_wm = (size_t)(cfg->destage_low * (double)cfg->cache_entries);
    if (fw->high_wm == 0)
        fw->high_wm = 1;

    if (pthread_create(&fw->thread, NULL, fw_main, fw) != 0) {
        rc = -EAGAIN;
        goto fail_cq;
    }
    fw->thread_started = 1;
    if (err)
        *err = 0;
    return fw;

fail_cq:
    sq_destroy(&fw->cq);
fail_sq:
    sq_destroy(&fw->sq);
fail_cache:
    free(fw->dirty_buf);
    free(fw->xfer);
    cache_free(&fw->cache);
fail_media:
    media_close(&fw->media);
fail_free:
    if (fw)
        free(fw->media_path);
    free(fw);
    fw = NULL;
out:
    if (err)
        *err = rc;
    return NULL;
}

fw_status_t fw_submit(fw_t *fw, fw_cmd_t *cmd)
{
    if (!cmd || cmd->tag >= FW_MAX_TAGS)
        return FW_ERR_INVALID_FIELD;
    if (!powered(fw) || atomic_load_explicit(&fw->stop_req, memory_order_acquire))
        return FW_ERR_ABORTED;

    /* Atomically claim the tag. fetch_or returns the OLD bitmap: if our bit
     * was already set, someone else holds this tag. In NCQ, reusing an
     * outstanding tag is a protocol violation the drive must reject. */
    uint32_t bit = 1u << cmd->tag;
    uint32_t old = atomic_fetch_or_explicit(&fw->tags_busy, bit, memory_order_acq_rel);
    if (old & bit)
        return FW_ERR_TAG_IN_USE;

    cmd->status = FW_OK;
    cmd->state = FW_CMD_QUEUED;
    cmd->t_submit_ns = now_ns();
    cmd->t_start_ns = cmd->t_done_ns = 0;
    /* After this push the firmware owns cmd. The queue's release store
     * publishes every field written above. */
    if (!sq_try_push(&fw->sq, cmd)) {
        /* Cannot happen while tags bound the queue depth; defensive. */
        atomic_fetch_and_explicit(&fw->tags_busy, ~bit, memory_order_release);
        return FW_ERR_HARDWARE;
    }
    return FW_OK;
}

fw_cmd_t *fw_poll(fw_t *fw, uint64_t timeout_us)
{
    void *item;
    if (sq_pop_timed(&fw->cq, &item, timeout_us * 1000))
        return item;
    return NULL;
}

fw_status_t fw_exec(fw_t *fw, fw_cmd_t *cmd)
{
    fw_status_t st = fw_submit(fw, cmd);
    if (st != FW_OK)
        return st;
    for (;;) {
        fw_cmd_t *done = fw_poll(fw, 1000);
        if (done == cmd)
            return cmd->status;
        if (done)
            return FW_ERR_HARDWARE;      /* caller broke the "no others" rule */
        if (!powered(fw)) {
            /* The firmware has stopped. Our completion either made it into
             * the queue just before the cut, or it never will. */
            done = fw_poll(fw, 0);
            if (done == cmd)
                return cmd->status;
            return done ? FW_ERR_HARDWARE : FW_ERR_ABORTED;
        }
    }
}

static void join_thread(fw_t *fw)
{
    if (fw->thread_started && !fw->thread_joined) {
        pthread_join(fw->thread, NULL);
        fw->thread_joined = 1;
    }
}

void fw_shutdown(fw_t *fw)
{
    if (!powered(fw)) {
        join_thread(fw);
        return;
    }
    atomic_store_explicit(&fw->stop_req, 1, memory_order_release);
    sq_close(&fw->sq);         /* wake it now instead of at the next tick */
    join_thread(fw);
    atomic_store_explicit(&fw->power_on, 0, memory_order_release);
}

void fw_power_cut(fw_t *fw)
{
    /* release: pairs with the acquire loads in powered(). Everything after
     * this point in the firmware is "after the power died". */
    atomic_store_explicit(&fw->power_on, 0, memory_order_release);
    sq_close(&fw->sq);         /* wake the firmware if it is waiting */
    join_thread(fw);
}

void fw_destroy(fw_t *fw)
{
    if (!fw)
        return;
    fw_power_cut(fw);          /* no flush: destroying == pulling the plug */
    cache_discard_all(&fw->cache);
    sq_destroy(&fw->cq);
    sq_destroy(&fw->sq);
    free(fw->dirty_buf);
    free(fw->xfer);
    cache_free(&fw->cache);
    media_close(&fw->media);
    free(fw->media_path);
    free(fw);
}

void fw_get_stats(fw_t *fw, fw_stats_t *o)
{
    fw_counters_t *c = &fw->ctr;
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < FW_OP_COUNT; i++)
        o->cmds[i] = ctr_get(&c->cmds[i]);
    o->cmd_errors = ctr_get(&c->cmd_errors);
    o->read_hit_sectors = ctr_get(&c->read_hit_sectors);
    o->read_miss_sectors = ctr_get(&c->read_miss_sectors);
    o->write_hit_sectors = ctr_get(&c->write_hit_sectors);
    o->destage_runs = ctr_get(&c->destage_runs);
    o->destaged_sectors = ctr_get(&c->destaged_sectors);
    o->threshold_destages = ctr_get(&c->threshold_destages);
    o->forced_destages = ctr_get(&c->forced_destages);
    o->idle_destages = ctr_get(&c->idle_destages);
    o->flushes = ctr_get(&c->flushes);
    o->readahead_ops = ctr_get(&c->readahead_ops);
    o->readahead_sectors = ctr_get(&c->readahead_sectors);
    o->media_reads = ctr_get(&c->media_reads);
    o->media_writes = ctr_get(&c->media_writes);
    o->media_sectors_read = ctr_get(&c->media_sectors_read);
    o->media_sectors_written = ctr_get(&c->media_sectors_written);
    o->media_busy_ms = (double)ctr_get(&c->media_busy_us) / 1000.0;
    o->crc_errors = ctr_get(&c->crc_errors);
    o->crc_retries = ctr_get(&c->crc_retries);
    o->crc_recovered = ctr_get(&c->crc_recovered);
    o->crc_unrecovered = ctr_get(&c->crc_unrecovered);
    o->transient_flips = ctr_get(&c->transient_flips);
    o->reallocations = ctr_get(&c->reallocations);
    o->torn_writes = ctr_get(&c->torn_writes);
    o->cache_dirty = (size_t)ctr_get(&c->cache_dirty);
    o->cache_used = (size_t)ctr_get(&c->cache_used);
}

uint64_t fw_capacity(const fw_t *fw)    { return fw->capacity; }
uint32_t fw_sector_size(const fw_t *fw) { return fw->ss; }
double   fw_time_scale(const fw_t *fw)  { return fw->cfg.time_scale; }

int fw_inject_media_bitflip(fw_t *fw, uint64_t lba, int bit)
{
    return media_inject_bitflip(&fw->media, lba, bit);
}
