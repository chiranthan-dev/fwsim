/*
 * ui/server.c - fwsim-ui: the live web dashboard's backend.
 *
 * One process, plain POSIX sockets + pthreads (no dependencies):
 *
 *   browser  <--- Server-Sent Events (/events) ---  event bus  <-- trace callback
 *            ---- GET /api/... (start, stop, cut, ...) --> runner thread --> fw_*
 *
 * Threads:
 *   - main:     accept() loop, one detached thread per HTTP connection.
 *   - runner:   the ONLY thread that calls fw_submit/fw_poll (the firmware's
 *               queues are single-producer/single-consumer). It runs the host
 *               workload, handles power cuts and the reboot-and-verify step.
 *   - ticker:   every 250 ms publishes a stats snapshot.
 *   - firmware: fwsim's own thread; calls our trace callback for every media
 *               access and command start/finish.
 *
 * Shared state lives in `S`, guarded by S.mu. Lock order: S.mu, then B.mu
 * (the event bus); never the reverse. No fw_* call that can block on the
 * firmware thread (power cut, shutdown, destroy) is ever made while holding
 * S.mu, because the firmware thread may itself be waiting for S.mu inside the
 * trace callback.
 *
 * The visual model: the platter is drawn as RINGS x CELLS cells. Ring =
 * cylinder band, cell = angular slot (sector / sectors-per-track, plus the
 * cylinder skew), exactly the angle disk-sim uses for rotational latency, so
 * the browser can show the target sector arriving under the head at the
 * moment disk-sim says the rotational wait ends.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "fwsim.h"
#include "latency.h"
#include "pattern.h"
#include "util.h"

#define RINGS 40
#define CELLS 120
#define NCELL (RINGS * CELLS)
#define BLK 8                 /* sectors per tracked 4 KiB block   */
#define MAX_XFER 128          /* largest command the UI issues     */
#define LAT_WIN 256           /* rolling latency window (commands) */
#define DONE_WIN 1024         /* rolling throughput window         */
#define FLUSH_EVERY 32

/* ---- small string builder ---------------------------------------------------- */

typedef struct {
    char  *p;
    size_t n, cap;
} sb_t;

static void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *b, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0)
            return;
        if (b->p && b->n + (size_t)k < b->cap) {
            b->n += (size_t)k;
            return;
        }
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + (size_t)k + 1)
            nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np)
            return;
        b->p = np;
        b->cap = nc;
    }
}

/* ---- event bus (fan-out to every SSE client) ------------------------------------- */

#define BUS_N 8192
static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    char           *ev[BUS_N];
    uint64_t        seq;          /* events ever published */
} B = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, { 0 }, 0 };

static void bus_push(const char *json)
{
    char *copy = strdup(json);
    if (!copy)
        return;
    pthread_mutex_lock(&B.mu);
    free(B.ev[B.seq % BUS_N]);
    B.ev[B.seq % BUS_N] = copy;
    B.seq++;
    pthread_cond_broadcast(&B.cv);
    pthread_mutex_unlock(&B.mu);
}

/* ---- the tracked blocks (for dirty/clean/lost colouring and verification) -------- */

enum { ST_EMPTY = 0, ST_CLEAN = 1, ST_DIRTY = 2, ST_LOST = 3 };

typedef struct {
    uint64_t blk;                /* LBA / BLK                         */
    uint32_t next_ver;           /* last version submitted            */
    uint32_t acked;              /* latest acknowledged version       */
    uint32_t durable;            /* latest version promised durable   */
    uint32_t snap;               /* acked at the last FLUSH submit    */
    uint16_t cell;
    uint8_t  state;              /* ST_*                              */
    uint8_t  inflight;
    uint8_t  used;
} blk_t;

typedef struct {
    int    workload;   /* 0 randwrite 1 randread 2 seqread 3 seqwrite 4 mixed */
    int    mode;       /* 0 write-back 1 write-through 2 WB+FUA 3 WB+FLUSH     */
    int    qd;
    int    sorted;
    int    region;     /* 0 whole disk 1 1 GiB 2 32 MiB                        */
    double flip;
    double scale;      /* time_scale: wall seconds per drive second            */
    int    env;        /* vibration environment: 0 off, env_mix_t 1..4         */
    int    policy;     /* write protection: FW_VIB_*                           */
} run_params_t;

typedef struct {
    fw_cmd_t cmd;
    uint8_t  buf[MAX_XFER * 512];
    int      busy;
    uint32_t nblk;
    uint64_t blk0;
    uint32_t ver[MAX_XFER / BLK];
} slot_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;            /* wakes the runner */

    /* drive geometry (immutable after init) */
    fw_config_t   base;
    disk_t       *map;             /* our own disk-sim handle: LBA -> location */
    uint32_t      cylinders, nzones;
    disk_zone_t   zones[DISK_MAX_ZONES];
    double        rev_ms, skew;
    uint64_t      capacity;
    char          media[256];

    /* live drive */
    fw_t         *fw;              /* NULL while rebooting                    */
    int           phase;           /* 0 idle 1 running 2 stopping 3 off 4 verifying */
    run_params_t  rp;              /* parameters of the live drive / run      */

    /* requests from HTTP handlers, consumed by the runner */
    int           req_start, req_stop, req_cut, req_flush, req_reset;
    run_params_t  req_rp;

    /* blocks + grid */
    blk_t        *blk;
    size_t        blk_cap, blk_n;
    uint32_t      cnt[NCELL][4];   /* per cell: blocks in each state */
    uint8_t       grid[NCELL];

    /* host metrics */
    char          tags[FW_MAX_TAGS + 1];
    double        lat[LAT_WIN];
    size_t        lat_n, lat_pos;
    uint64_t      done_ns[DONE_WIN];
    size_t        done_n, done_pos;
    uint64_t      cmds_done, acked_writes, errors, flushes_done;
    uint64_t      last_event_ns;
    double        prev_busy_ms, prev_sim_ms, busy_pct;
    char          verify[512];
} S;

static void sim_scale_defaults(run_params_t *rp)
{
    rp->workload = 0;
    rp->mode = 0;
    rp->qd = 4;
    rp->sorted = 1;
    rp->region = 0;
    rp->flip = 0.0;
    rp->scale = 10.0;
    rp->env = 0;
    rp->policy = 0;
}

/* ---- geometry: LBA -> cell ----------------------------------------------------------- */

static uint32_t spt_of(uint32_t cyl)
{
    for (uint32_t z = 0; z < S.nzones; z++)
        if (cyl >= S.zones[z].first_cylinder &&
            cyl < S.zones[z].first_cylinder + S.zones[z].num_cylinders)
            return S.zones[z].sectors_per_track;
    return S.nzones ? S.zones[S.nzones - 1].sectors_per_track : 1000;
}

/* Angle of a sector as a fraction of a revolution: the same formula
 * disk-sim uses for rotational position (sector start + cylinder skew). */
static double sector_angle(uint32_t cyl, uint32_t sector, uint32_t spt)
{
    double a = (double)sector / (double)spt + fmod((double)cyl * S.skew, 1.0);
    return a - floor(a);
}

static uint16_t cell_of(uint64_t lba)
{
    disk_chs_t c;
    if (disk_lba_to_chs(S.map, lba, &c) != DISK_OK)
        return 0;
    uint32_t ring = (uint32_t)((uint64_t)c.cylinder * RINGS / S.cylinders);
    if (ring >= RINGS)
        ring = RINGS - 1;
    uint32_t a = (uint32_t)(sector_angle(c.cylinder, c.sector, spt_of(c.cylinder)) * CELLS);
    if (a >= CELLS)
        a = CELLS - 1;
    return (uint16_t)(ring * CELLS + a);
}

/* ---- block table (open addressing; caller holds S.mu) --------------------------------- */

static size_t blk_slot(uint64_t key)
{
    return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 20) & (S.blk_cap - 1);
}

static blk_t *blk_find(uint64_t key)
{
    if (!S.blk_cap)
        return NULL;
    for (size_t i = blk_slot(key);; i = (i + 1) & (S.blk_cap - 1)) {
        if (!S.blk[i].used)
            return NULL;
        if (S.blk[i].blk == key)
            return &S.blk[i];
    }
}

static blk_t *blk_get(uint64_t key)
{
    blk_t *b = blk_find(key);
    if (b)
        return b;
    if ((S.blk_n + 1) * 2 > S.blk_cap) {            /* keep load <= 50% */
        size_t oc = S.blk_cap, nc = oc ? oc * 2 : 4096;
        blk_t *old = S.blk, *nb = calloc(nc, sizeof(*nb));
        if (!nb)
            return NULL;
        S.blk = nb;
        S.blk_cap = nc;
        for (size_t i = 0; i < oc; i++)
            if (old[i].used) {
                size_t j = blk_slot(old[i].blk);
                while (S.blk[j].used)
                    j = (j + 1) & (nc - 1);
                S.blk[j] = old[i];
            }
        free(old);
    }
    size_t j = blk_slot(key);
    while (S.blk[j].used)
        j = (j + 1) & (S.blk_cap - 1);
    b = &S.blk[j];
    memset(b, 0, sizeof(*b));
    b->used = 1;
    b->blk = key;
    b->cell = cell_of(key * BLK);
    S.blk_n++;
    S.cnt[b->cell][ST_EMPTY]++;
    return b;
}

static uint8_t cell_state(uint16_t c)
{
    if (S.cnt[c][ST_LOST])
        return ST_LOST;
    if (S.cnt[c][ST_DIRTY])
        return ST_DIRTY;
    if (S.cnt[c][ST_CLEAN])
        return ST_CLEAN;
    return ST_EMPTY;
}

/* Move a block to a new state; append changed cells to `u`. */
static void blk_set(blk_t *b, uint8_t st, sb_t *u, int *nu)
{
    if (b->state == st)
        return;
    S.cnt[b->cell][b->state]--;
    S.cnt[b->cell][st]++;
    b->state = st;
    uint8_t g = cell_state(b->cell);
    if (g != S.grid[b->cell]) {
        S.grid[b->cell] = g;
        sb_printf(u, "%s%u,%u", *nu ? "," : "", b->cell, g);
        (*nu)++;
    }
}

static void push_cells(double t, sb_t *u, int nu)
{
    if (!nu)
        return;
    sb_t j = { 0 };
    sb_printf(&j, "{\"type\":\"cells\",\"t\":%.3f,\"u\":[%s]}", t, u->p);
    bus_push(j.p);
    free(j.p);
}

/* ---- the trace callback (firmware thread) ------------------------------------------------ */

static const char *why_str(int r)
{
    return r == FW_IO_DESTAGE ? "destage" : r == FW_IO_READAHEAD ? "readahead" : "host";
}

static int throttled(void)
{
    /* Command start/done events are for the log; at real-time speed a
     * cache-hit workload can complete thousands per second, more than a
     * browser should render. Keep at most one per 4 ms of wall time. */
    uint64_t now = now_ns();
    if (now - S.last_event_ns < 4000000ull)
        return 1;
    S.last_event_ns = now;
    return 0;
}

static void on_trace(void *ctx, const fw_trace_t *ev)
{
    (void)ctx;
    char buf[512];
    pthread_mutex_lock(&S.mu);
    switch (ev->kind) {
    case FW_TRACE_MEDIA: {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"media\",\"t\":%.3f,\"w\":%d,\"why\":\"%s\",\"lba\":%llu,\"n\":%u,"
                 "\"cyl\":%u,\"head\":%u,\"sec\":%u,\"spt\":%u,\"seek\":%.4f,\"rot\":%.4f,\"xfer\":%.4f}",
                 ev->sim_ms, ev->is_write, why_str(ev->reason), (unsigned long long)ev->lba,
                 ev->count, ev->cyl, ev->head, ev->sector, ev->spt, ev->seek_ms, ev->rot_ms,
                 ev->xfer_ms);
        bus_push(buf);
        if (ev->is_write) {
            /* Data lands as the sectors pass under the head: effective at
             * the end of the transfer. */
            double t_end = ev->sim_ms + ev->seek_ms + ev->rot_ms + ev->xfer_ms;
            sb_t u = { 0 };
            int nu = 0;
            uint64_t b0 = ev->lba / BLK, b1 = (ev->lba + ev->count - 1) / BLK;
            for (uint64_t k = b0; k <= b1; k++) {
                blk_t *b = blk_find(k);
                if (b)
                    blk_set(b, ST_CLEAN, &u, &nu);
            }
            push_cells(t_end, &u, nu);
            free(u.p);
        }
        break;
    }
    case FW_TRACE_CMD_START:
        if (ev->tag < FW_MAX_TAGS)
            S.tags[ev->tag] = 'a';
        if (!throttled()) {
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"start\",\"t\":%.3f,\"tag\":%u,\"op\":\"%s\",\"lba\":%llu,\"n\":%u,\"fua\":%d}",
                     ev->sim_ms, ev->tag, fw_opcode_str(ev->opcode), (unsigned long long)ev->lba,
                     ev->count, (ev->flags & FW_CMD_FUA) != 0);
            bus_push(buf);
        }
        break;
    case FW_TRACE_CMD_DONE:
        if (ev->opcode == FW_OP_WRITE && ev->status == FW_OK && ev->wce &&
            !(ev->flags & FW_CMD_FUA)) {
            /* Write-back: acknowledged from DRAM, dirty until destaged. */
            sb_t u = { 0 };
            int nu = 0;
            uint64_t b0 = ev->lba / BLK, b1 = (ev->lba + ev->count - 1) / BLK;
            for (uint64_t k = b0; k <= b1; k++) {
                blk_t *b = blk_find(k);
                if (b)
                    blk_set(b, ST_DIRTY, &u, &nu);
            }
            push_cells(ev->sim_ms, &u, nu);
            free(u.p);
        }
        break;
    case FW_TRACE_CRC:
        snprintf(buf, sizeof(buf), "{\"type\":\"crc\",\"t\":%.3f,\"lba\":%llu,\"ok\":%d}",
                 ev->sim_ms, (unsigned long long)ev->lba, ev->recovered);
        bus_push(buf);
        break;
    case FW_TRACE_VIBRATION: {
        /* One per classifier window (every 16 ms of drive time). Threshold
         * interrupts can fire every sample during a shock: at most one per
         * 5 ms of drive time reaches the browser. */
        static double last_irq_ms = -1e9;
        if (ev->vib_class == 255) {
            if (ev->sim_ms - last_irq_ms < 5.0)
                break;
            last_irq_ms = ev->sim_ms;
        }
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"vib\",\"t\":%.3f,\"cls\":%d,\"conf\":%u,\"rms\":%u,\"peak\":%u,"
                 "\"inh\":%d,\"prot\":%d,\"until\":%.3f}",
                 ev->sim_ms, ev->vib_class == 255 ? -1 : (int)ev->vib_class, ev->vib_conf,
                 ev->vib_rms_mg, ev->vib_peak_mg, ev->vib_inhibit, ev->vib_protect,
                 ev->vib_inhibit_until_ms);
        bus_push(buf);
        break;
    }
    }
    pthread_mutex_unlock(&S.mu);
}

/* ---- drive lifecycle (runner thread) ---------------------------------------------------------- */

static void publish_state_locked(void)
{
    static const char *names[] = { "idle", "running", "stopping", "off", "verifying" };
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"state\",\"phase\":\"%s\",\"workload\":%d,\"mode\":%d,\"qd\":%d,"
             "\"sorted\":%d,\"region\":%d,\"flip\":%g,\"scale\":%g,\"env\":%d,\"policy\":%d}",
             names[S.phase], S.rp.workload, S.rp.mode, S.rp.qd, S.rp.sorted, S.rp.region,
             S.rp.flip, S.rp.scale, S.rp.env, S.rp.policy);
    bus_push(buf);
}

static fw_t *open_drive(const run_params_t *rp, int create, int traced)
{
    fw_config_t cfg = S.base;
    cfg.media_path = S.media;
    cfg.media_create = create;
    cfg.time_scale = traced ? rp->scale : 0.0;
    cfg.destage_sorted = rp->sorted;
    cfg.fault_read_flip_prob = traced ? rp->flip : 0.0;
    cfg.write_cache = rp->mode != 1;
    cfg.trace = traced ? on_trace : NULL;
    cfg.idle_destage_ms = traced ? 50.0 : 0.0;
    /* The vibration environment only exists for the live run; the reboot
     * verifier reads back in a quiet room. */
    cfg.vib_env = traced ? rp->env : 0;
    cfg.vib_policy = traced ? rp->policy : 0;
    return fw_create(&cfg, NULL);
}

static void tags_reset_locked(void)
{
    memset(S.tags, 'f', FW_MAX_TAGS);
    S.tags[FW_MAX_TAGS] = 0;
}

/* ---- the host workload ------------------------------------------------------------------------- */

static slot_t slots[FW_MAX_TAGS];
static uint64_t host_rng = 12345, seq_cursor;
static uint64_t acked_since_flush;
static int flush_outstanding;

static uint64_t region_lbas(const run_params_t *rp)
{
    uint64_t r = rp->region == 1 ? 2097152ull : rp->region == 2 ? 65536ull : S.capacity;
    return r > S.capacity ? S.capacity : r;
}

static void record_done(fw_cmd_t *c, double scale)
{
    uint64_t now = now_ns();
    double lat_ms = (double)(now - c->t_submit_ns) / 1e6 / (scale > 0 ? scale : 1);
    S.lat[S.lat_pos] = lat_ms;
    S.lat_pos = (S.lat_pos + 1) % LAT_WIN;
    if (S.lat_n < LAT_WIN)
        S.lat_n++;
    S.done_ns[S.done_pos] = now;
    S.done_pos = (S.done_pos + 1) % DONE_WIN;
    if (S.done_n < DONE_WIN)
        S.done_n++;
    S.cmds_done++;
    if (c->status != FW_OK)
        S.errors++;
}

static void on_completion(fw_cmd_t *c, int live)
{
    slot_t *sl = &slots[c->tag];
    pthread_mutex_lock(&S.mu);
    if (c->opcode == FW_OP_FLUSH) {
        flush_outstanding = 0;
        if (c->status == FW_OK) {
            for (size_t i = 0; i < S.blk_cap; i++)
                if (S.blk[i].used && S.blk[i].snap > S.blk[i].durable)
                    S.blk[i].durable = S.blk[i].snap;
            S.flushes_done++;
        }
    } else if (c->opcode == FW_OP_WRITE) {
        for (uint32_t k = 0; k < sl->nblk; k++) {
            blk_t *b = blk_find(sl->blk0 + k);
            if (!b)
                continue;
            b->inflight = 0;
            if (c->status == FW_OK && sl->ver[k] > b->acked) {
                b->acked = sl->ver[k];
                if (S.rp.mode == 1 || S.rp.mode == 2)
                    b->durable = sl->ver[k];
            }
        }
        if (c->status == FW_OK) {
            S.acked_writes++;
            acked_since_flush++;
        }
    }
    record_done(c, S.rp.scale);
    S.tags[c->tag] = 'f';
    if (live && !throttled()) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"done\",\"tag\":%u,\"op\":\"%s\",\"lba\":%llu,\"st\":\"%s\",\"lat\":%.4f}",
                 c->tag, fw_opcode_str(c->opcode), (unsigned long long)c->lba,
                 fw_status_str(c->status), S.lat[(S.lat_pos + LAT_WIN - 1) % LAT_WIN]);
        bus_push(buf);
    }
    pthread_mutex_unlock(&S.mu);
    sl->busy = 0;
}

/* Prepare the next command in slot `i`. Returns 1 if one was prepared. */
static int prepare(int i, const run_params_t *rp)
{
    slot_t *sl = &slots[i];
    fw_cmd_t *c = &sl->cmd;
    memset(c, 0, sizeof(*c));
    c->tag = (uint8_t)i;

    if (rp->mode == 3 && !flush_outstanding && acked_since_flush >= FLUSH_EVERY) {
        pthread_mutex_lock(&S.mu);
        for (size_t k = 0; k < S.blk_cap; k++)
            if (S.blk[k].used)
                S.blk[k].snap = S.blk[k].acked;
        pthread_mutex_unlock(&S.mu);
        c->opcode = FW_OP_FLUSH;
        flush_outstanding = 1;
        acked_since_flush = 0;
        sl->nblk = 0;
        return 1;
    }

    uint64_t region = region_lbas(rp);
    int is_write, seq = rp->workload == 2 || rp->workload == 3;
    uint32_t n = rp->workload == 2 ? 128 : rp->workload == 3 ? 64 : 8;
    switch (rp->workload) {
    case 0: case 3: is_write = 1; break;
    case 4: is_write = rng_unit64(&host_rng) >= 0.7; break;
    default: is_write = 0; break;
    }
    uint64_t units = region / n, u;
    if (seq) {
        u = seq_cursor++ % units;
    } else if (rp->workload == 4 && rng_unit64(&host_rng) < 0.9) {
        uint64_t hot = units / 20 ? units / 20 : 1;    /* hot 5% */
        u = rng_below64(&host_rng, hot);
    } else {
        u = rng_below64(&host_rng, units);
    }
    c->lba = u * n;
    c->count = n;
    c->buf = sl->buf;
    c->opcode = is_write ? FW_OP_WRITE : FW_OP_READ;
    c->flags = (is_write && rp->mode == 2) ? FW_CMD_FUA : 0;
    sl->nblk = 0;
    if (!is_write)
        return 1;

    /* Writes: one outstanding write per block keeps version order exact. */
    pthread_mutex_lock(&S.mu);
    uint64_t b0 = c->lba / BLK;
    uint32_t nb = n / BLK;
    for (uint32_t k = 0; k < nb; k++) {
        blk_t *b = blk_get(b0 + k);
        if (!b || b->inflight) {
            pthread_mutex_unlock(&S.mu);
            if (seq)
                seq_cursor--;
            return 0;
        }
    }
    for (uint32_t k = 0; k < nb; k++) {
        blk_t *b = blk_find(b0 + k);
        b->inflight = 1;
        sl->ver[k] = ++b->next_ver;
    }
    pthread_mutex_unlock(&S.mu);
    sl->blk0 = b0;
    sl->nblk = nb;
    for (uint32_t k = 0; k < n; k++)
        pattern_fill(sl->buf + (size_t)k * 512, 512, c->lba + k, sl->ver[k / BLK]);
    return 1;
}

/* ---- power cut, reboot, verify --------------------------------------------------------------- */

static void reboot_and_verify(fw_t *dead, const run_params_t *rp)
{
    /* Completions already posted before the cut count as acknowledged. */
    for (fw_cmd_t *c; (c = fw_poll(dead, 0)) != NULL;)
        on_completion(c, 0);
    fw_stats_t st;
    fw_get_stats(dead, &st);
    fw_destroy(dead);                     /* DRAM cache contents: gone */
    for (int i = 0; i < FW_MAX_TAGS; i++)
        slots[i].busy = 0;
    flush_outstanding = 0;

    pthread_mutex_lock(&S.mu);
    S.phase = 4;
    tags_reset_locked();
    publish_state_locked();
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"type\":\"power\",\"on\":0,\"dirty\":%zu,\"torn\":%llu}",
             st.cache_dirty, (unsigned long long)st.torn_writes);
    bus_push(buf);
    /* Snapshot the blocks that need checking. */
    size_t n = 0;
    uint64_t *keys = malloc((S.blk_n + 1) * sizeof(*keys));
    for (size_t i = 0; keys && i < S.blk_cap; i++)
        if (S.blk[i].used && (S.blk[i].acked || S.blk[i].durable || S.blk[i].next_ver))
            keys[n++] = S.blk[i].blk;
    pthread_mutex_unlock(&S.mu);

    /* Reboot: a fresh firmware on the same platters, cold cache, no
     * media delay (verification is not what we are animating). */
    fw_t *v = open_drive(rp, 0, 0);
    uint64_t checked = 0, lost = 0, violations = 0, survived = 0;
    uint8_t rb[BLK * 512];
    sb_t u = { 0 };
    int nu = 0;
    for (size_t i = 0; v && keys && i < n; i++) {
        fw_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = FW_OP_READ;
        c.lba = keys[i] * BLK;
        c.count = BLK;
        c.buf = rb;
        uint32_t minver = UINT32_MAX;
        int bad = fw_exec(v, &c) != FW_OK;
        for (uint32_t k = 0; k < BLK && !bad; k++) {
            uint32_t ver = 0;
            int pc = pattern_check(rb + k * 512, 512, c.lba + k, &ver);
            if (pc == PAT_BAD)
                bad = 1;
            if (pc == PAT_ZERO)
                ver = 0;
            if (ver < minver)
                minver = ver;
        }
        if (bad)
            minver = 0;
        pthread_mutex_lock(&S.mu);
        blk_t *b = blk_find(keys[i]);
        if (b && b->acked) {
            checked++;
            if (minver < b->acked) {
                lost++;
                blk_set(b, ST_LOST, &u, &nu);
            } else {
                survived++;
                blk_set(b, ST_CLEAN, &u, &nu);
            }
            if (minver < b->durable)
                violations++;
        } else if (b) {
            blk_set(b, minver ? ST_CLEAN : ST_EMPTY, &u, &nu);
        }
        if (b) {   /* the media is now the truth */
            b->acked = b->durable = b->snap = minver;
            b->inflight = 0;
        }
        pthread_mutex_unlock(&S.mu);
    }
    free(keys);
    if (v) {
        fw_shutdown(v);
        fw_destroy(v);
    }

    pthread_mutex_lock(&S.mu);
    push_cells(0, &u, nu);
    static const char *modes[] = { "write-back", "write-through", "write-back + FUA",
                                   "write-back + FLUSH every 32" };
    snprintf(S.verify, sizeof(S.verify),
             "{\"type\":\"verify\",\"mode\":\"%s\",\"checked\":%llu,\"lost\":%llu,"
             "\"survived\":%llu,\"violations\":%llu,\"dirty_at_cut\":%zu}",
             modes[rp->mode], (unsigned long long)checked, (unsigned long long)lost,
             (unsigned long long)survived, (unsigned long long)violations, st.cache_dirty);
    bus_push(S.verify);
    pthread_mutex_unlock(&S.mu);
    free(u.p);

    fw_t *fresh = open_drive(rp, 0, 1);
    pthread_mutex_lock(&S.mu);
    S.fw = fresh;
    S.phase = fresh ? 0 : 3;
    S.prev_busy_ms = 0;
    S.prev_sim_ms = 0;
    bus_push("{\"type\":\"power\",\"on\":1}");
    publish_state_locked();
    pthread_mutex_unlock(&S.mu);
}

static void reset_all_locked(void)
{
    free(S.blk);
    S.blk = NULL;
    S.blk_cap = S.blk_n = 0;
    memset(S.cnt, 0, sizeof(S.cnt));
    memset(S.grid, 0, sizeof(S.grid));
    S.lat_n = S.lat_pos = S.done_n = S.done_pos = 0;
    S.cmds_done = S.acked_writes = S.errors = S.flushes_done = 0;
    S.verify[0] = 0;
    S.prev_busy_ms = S.prev_sim_ms = 0;
    tags_reset_locked();
}

static void *runner_main(void *arg)
{
    (void)arg;
    int outstanding = 0;
    for (;;) {
        pthread_mutex_lock(&S.mu);
        while (!S.req_start && !S.req_stop && !S.req_cut && !S.req_flush && !S.req_reset &&
               S.phase != 1 && S.phase != 2 && outstanding == 0)
            pthread_cond_wait(&S.cv, &S.mu);
        int start = S.req_start, stop = S.req_stop, cut = S.req_cut, flush = S.req_flush,
            reset = S.req_reset;
        run_params_t want = S.req_rp, cur = S.rp;
        S.req_start = S.req_stop = S.req_cut = S.req_flush = S.req_reset = 0;
        fw_t *fw = S.fw;
        pthread_mutex_unlock(&S.mu);

        if (reset) {
            pthread_mutex_lock(&S.mu);
            S.fw = NULL;
            pthread_mutex_unlock(&S.mu);
            if (fw)
                fw_destroy(fw);
            outstanding = 0;
            for (int i = 0; i < FW_MAX_TAGS; i++)
                slots[i].busy = 0;
            flush_outstanding = 0;
            acked_since_flush = 0;
            fw_t *fresh = open_drive(&cur, 1, 1);
            pthread_mutex_lock(&S.mu);
            reset_all_locked();
            S.fw = fresh;
            S.phase = 0;
            bus_push("{\"type\":\"reset\"}");
            publish_state_locked();
            pthread_mutex_unlock(&S.mu);
            continue;
        }
        if (!fw)
            continue;

        if (cut) {
            pthread_mutex_lock(&S.mu);
            S.fw = NULL;
            S.phase = 3;
            publish_state_locked();
            pthread_mutex_unlock(&S.mu);
            fw_power_cut(fw);              /* NOT holding S.mu: see header */
            outstanding = 0;
            reboot_and_verify(fw, &cur);
            continue;
        }

        if (start && outstanding > 0) {
            /* Still draining a previous run: try again next round. */
            pthread_mutex_lock(&S.mu);
            S.req_start = 1;
            pthread_mutex_unlock(&S.mu);
            start = 0;
        }
        if (start) {
            /* Settings that live in the drive's config need a power cycle
             * (clean: everything is destaged first); the write cache mode
             * is switched with a real SET FEATURES command instead. */
            if (want.scale != cur.scale || want.sorted != cur.sorted || want.flip != cur.flip ||
                want.env != cur.env || want.policy != cur.policy) {
                pthread_mutex_lock(&S.mu);
                S.fw = NULL;
                pthread_mutex_unlock(&S.mu);
                fw_shutdown(fw);
                fw_destroy(fw);
                fw = open_drive(&want, 0, 1);
                pthread_mutex_lock(&S.mu);
                S.fw = fw;
                S.prev_busy_ms = S.prev_sim_ms = 0;
                pthread_mutex_unlock(&S.mu);
                if (!fw)
                    continue;
            } else if ((want.mode == 1) != (cur.mode == 1)) {
                fw_cmd_t sf;
                memset(&sf, 0, sizeof(sf));
                sf.opcode = FW_OP_SET_FEATURES;
                sf.feature = FW_FEAT_WRITE_CACHE;
                sf.feature_value = want.mode != 1;
                fw_exec(fw, &sf);
            }
            acked_since_flush = 0;
            pthread_mutex_lock(&S.mu);
            S.rp = want;
            cur = want;
            S.phase = 1;
            publish_state_locked();
            pthread_mutex_unlock(&S.mu);
        }
        if (stop) {
            pthread_mutex_lock(&S.mu);
            if (S.phase == 1)
                S.phase = 2;
            publish_state_locked();
            pthread_mutex_unlock(&S.mu);
        }

        pthread_mutex_lock(&S.mu);
        int phase = S.phase;
        pthread_mutex_unlock(&S.mu);

        /* Keep the queue full while running; a FLUSH request from the UI
         * takes the next free slot. */
        for (int i = 0; i < cur.qd && i < FW_MAX_TAGS; i++) {
            if (slots[i].busy)
                continue;
            if (flush) {
                memset(&slots[i].cmd, 0, sizeof(slots[i].cmd));
                slots[i].cmd.opcode = FW_OP_FLUSH;
                slots[i].cmd.tag = (uint8_t)i;
                slots[i].nblk = 0;
                pthread_mutex_lock(&S.mu);
                for (size_t k = 0; k < S.blk_cap; k++)
                    if (S.blk[k].used)
                        S.blk[k].snap = S.blk[k].acked;
                pthread_mutex_unlock(&S.mu);
                flush_outstanding = 1;
                flush = 0;
            } else if (phase != 1 || !prepare(i, &cur)) {
                continue;
            }
            pthread_mutex_lock(&S.mu);
            S.tags[i] = 'q';
            pthread_mutex_unlock(&S.mu);
            if (fw_submit(fw, &slots[i].cmd) == FW_OK) {
                slots[i].busy = 1;
                outstanding++;
            }
        }

        if (flush) {               /* no free slot this round: keep it */
            pthread_mutex_lock(&S.mu);
            S.req_flush = 1;
            pthread_mutex_unlock(&S.mu);
        }
        if (outstanding > 0) {
            fw_cmd_t *c = fw_poll(fw, 2000);
            if (c) {
                on_completion(c, 1);
                outstanding--;
            }
        } else if (phase == 2) {
            pthread_mutex_lock(&S.mu);
            S.phase = 0;
            publish_state_locked();
            pthread_mutex_unlock(&S.mu);
        }
    }
    return NULL;
}

/* ---- stats ticker ------------------------------------------------------------------------------- */

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void *ticker_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = { 0, 250000000 };
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&S.mu);
        if (!S.fw) {
            pthread_mutex_unlock(&S.mu);
            continue;
        }
        fw_stats_t st;
        fw_get_stats(S.fw, &st);
        double sim = fw_sim_now_ms(S.fw);
        if (S.prev_sim_ms > 0 && sim > S.prev_sim_ms)
            S.busy_pct = 100.0 * (st.media_busy_ms - S.prev_busy_ms) / (sim - S.prev_sim_ms);
        if (S.busy_pct > 100)
            S.busy_pct = 100;
        if (S.busy_pct < 0)
            S.busy_pct = 0;
        S.prev_busy_ms = st.media_busy_ms;
        S.prev_sim_ms = sim;

        double l[LAT_WIN], avg = 0;
        size_t n = S.lat_n;
        memcpy(l, S.lat, sizeof(l));
        for (size_t i = 0; i < n; i++)
            avg += l[i];
        qsort(l, n, sizeof(double), cmp_d);
        double p50 = n ? l[(n - 1) / 2] : 0, p95 = n ? l[(size_t)((n - 1) * 0.95)] : 0,
               p99 = n ? l[(size_t)((n - 1) * 0.99)] : 0;
        /* Throughput: completions in the last 2 s of wall time, in drive time. */
        uint64_t now = now_ns(), cnt = 0;
        for (size_t i = 0; i < S.done_n; i++)
            if (now - S.done_ns[i] < 2000000000ull)
                cnt++;
        double iops = (double)cnt / (2.0 / (S.rp.scale > 0 ? S.rp.scale : 1));
        uint64_t rd = st.read_hit_sectors + st.read_miss_sectors;
        sb_t j = { 0 };
        sb_printf(&j,
                  "{\"type\":\"stats\",\"t\":%.3f,\"iops\":%.2f,\"avg\":%.4f,\"p50\":%.4f,\"p95\":%.4f,"
                  "\"p99\":%.4f,\"hit\":%.2f,\"dirty\":%zu,\"cache\":%zu,\"destage_runs\":%llu,"
                  "\"destaged\":%llu,\"mreads\":%llu,\"mwrites\":%llu,\"busy\":%.1f,\"crc\":%llu,"
                  "\"crc_ok\":%llu,\"crc_bad\":%llu,\"flips\":%llu,\"cmds\":%llu,\"acked\":%llu,"
                  "\"errors\":%llu,\"flushes\":%llu,\"realloc\":%llu,\"tags\":\"%s\","
                  "\"offtrack\":%llu,\"vwin\":%llu,\"vcls\":[%llu,%llu,%llu,%llu],\"virq\":%llu,"
                  "\"vinh\":%llu,\"vstall\":%.1f,\"vfalse\":%.1f,\"vverify\":%llu,"
                  "\"vrewrite\":%llu,\"vabort\":%llu,\"vrel\":%llu}",
                  sim, iops, n ? avg / (double)n : 0, p50, p95, p99,
                  rd ? 100.0 * (double)st.read_hit_sectors / (double)rd : -1.0, st.cache_dirty,
                  S.base.cache_entries, (unsigned long long)st.destage_runs,
                  (unsigned long long)st.destaged_sectors, (unsigned long long)st.media_reads,
                  (unsigned long long)st.media_writes, S.busy_pct,
                  (unsigned long long)st.crc_errors, (unsigned long long)st.crc_recovered,
                  (unsigned long long)st.crc_unrecovered, (unsigned long long)st.transient_flips,
                  (unsigned long long)S.cmds_done, (unsigned long long)S.acked_writes,
                  (unsigned long long)S.errors, (unsigned long long)S.flushes_done,
                  (unsigned long long)st.reallocations, S.tags,
                  (unsigned long long)st.vib_offtrack_sectors, (unsigned long long)st.vib_windows,
                  (unsigned long long)st.vib_class_windows[0],
                  (unsigned long long)st.vib_class_windows[1],
                  (unsigned long long)st.vib_class_windows[2],
                  (unsigned long long)st.vib_class_windows[3],
                  (unsigned long long)st.vib_shock_irqs, (unsigned long long)st.vib_inhibits,
                  st.vib_stall_ms, st.vib_false_stall_ms,
                  (unsigned long long)st.vib_verified_writes,
                  (unsigned long long)st.vib_verify_rewrites,
                  (unsigned long long)st.vib_write_aborts,
                  (unsigned long long)st.vib_early_releases);
        bus_push(j.p);
        free(j.p);
        pthread_mutex_unlock(&S.mu);
    }
    return NULL;
}

/* ---- HTTP ---------------------------------------------------------------------------------------- */

static const char *static_dir = "ui/static";

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static void respond(int fd, const char *status, const char *type, const char *body, size_t len)
{
    char hdr[256];
    int k = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, len);
    if (write_all(fd, hdr, (size_t)k) == 0)
        write_all(fd, body, len);
}

static void serve_file(int fd, const char *name)
{
    /* Only plain file names from the static directory: no paths. */
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_') ||
            (p == name && *p == '.')) {
            respond(fd, "404 Not Found", "text/plain", "not found", 9);
            return;
        }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", static_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        respond(fd, "404 Not Found", "text/plain", "not found", 9);
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *body = malloc(len > 0 ? (size_t)len : 1);
    size_t got = body ? fread(body, 1, (size_t)len, f) : 0;
    fclose(f);
    const char *ext = strrchr(name, '.');
    const char *type = !ext ? "application/octet-stream"
                       : !strcmp(ext, ".html") ? "text/html; charset=utf-8"
                       : !strcmp(ext, ".js")   ? "text/javascript; charset=utf-8"
                       : !strcmp(ext, ".css")  ? "text/css; charset=utf-8"
                       : !strcmp(ext, ".svg")  ? "image/svg+xml"
                                               : "application/octet-stream";
    respond(fd, "200 OK", type, body ? body : "", got);
    free(body);
}

static const char *qget(const char *q, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = q; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < n)
                out[i++] = *p++;
            out[i] = 0;
            return out;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return NULL;
}

static double qnum(const char *q, const char *key, double def, double lo, double hi)
{
    char v[32];
    if (!qget(q, key, v, sizeof(v)))
        return def;
    double x = strtod(v, NULL);
    return x < lo ? lo : x > hi ? hi : x;
}

static void hello(int fd)
{
    sb_t j = { 0 };
    pthread_mutex_lock(&S.mu);
    sb_printf(&j, "data: {\"type\":\"hello\",\"rings\":%d,\"cells\":%d,\"cylinders\":%u,"
                  "\"rev_ms\":%.6f,\"skew\":%.6f,\"capacity\":%llu,\"cache\":%zu,\"queue\":\"%s\","
                  "\"sim\":%.3f,\"grid\":\"",
              RINGS, CELLS, S.cylinders, S.rev_ms, S.skew, (unsigned long long)S.capacity,
              S.base.cache_entries, fw_queue_impl(), S.fw ? fw_sim_now_ms(S.fw) : 0.0);
    for (int i = 0; i < NCELL; i++)
        sb_printf(&j, "%c", '0' + S.grid[i]);
    sb_printf(&j, "\"}\n\n");
    pthread_mutex_unlock(&S.mu);
    write_all(fd, j.p, j.n);
    free(j.p);
}

static void sse(int fd)
{
    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                      "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
    if (write_all(fd, hdr, strlen(hdr)) != 0)
        return;
    pthread_mutex_lock(&B.mu);
    uint64_t my = B.seq;
    pthread_mutex_unlock(&B.mu);
    hello(fd);
    pthread_mutex_lock(&S.mu);       /* current state + last verify result */
    publish_state_locked();
    if (S.verify[0])
        bus_push(S.verify);
    pthread_mutex_unlock(&S.mu);

    sb_t out = { 0 };
    for (;;) {
        out.n = 0;
        pthread_mutex_lock(&B.mu);
        if (B.seq == my) {
            struct timespec dl;
            clock_gettime(CLOCK_REALTIME, &dl);
            dl.tv_sec += 10;
            pthread_cond_timedwait(&B.cv, &B.mu, &dl);
        }
        if (B.seq - my > BUS_N)
            my = B.seq - BUS_N;          /* too slow: skip what we lost */
        while (my < B.seq) {
            sb_printf(&out, "data: %s\n\n", B.ev[my % BUS_N]);
            my++;
        }
        pthread_mutex_unlock(&B.mu);
        if (out.n == 0)
            sb_printf(&out, ": keep-alive\n\n");
        if (write_all(fd, out.p, out.n) != 0)
            break;
    }
    free(out.p);
}

static void api(int fd, const char *what, const char *q)
{
    pthread_mutex_lock(&S.mu);
    if (!strcmp(what, "start")) {
        run_params_t rp = S.rp;
        rp.workload = (int)qnum(q, "workload", rp.workload, 0, 4);
        rp.mode = (int)qnum(q, "mode", rp.mode, 0, 3);
        rp.qd = (int)qnum(q, "qd", rp.qd, 1, FW_MAX_TAGS);
        rp.sorted = (int)qnum(q, "sorted", rp.sorted, 0, 1);
        rp.region = (int)qnum(q, "region", rp.region, 0, 2);
        rp.flip = qnum(q, "flip", rp.flip, 0, 0.2);
        rp.scale = qnum(q, "scale", rp.scale, 1, 200);
        rp.env = (int)qnum(q, "env", rp.env, 0, 4);
        rp.policy = (int)qnum(q, "policy", rp.policy, 0, 3);
        S.req_rp = rp;
        S.req_start = 1;
    } else if (!strcmp(what, "stop")) {
        S.req_stop = 1;
    } else if (!strcmp(what, "cut")) {
        S.req_cut = 1;
    } else if (!strcmp(what, "flush")) {
        S.req_flush = 1;
    } else if (!strcmp(what, "reset")) {
        S.req_reset = 1;
    } else {
        pthread_mutex_unlock(&S.mu);
        respond(fd, "404 Not Found", "text/plain", "unknown", 7);
        return;
    }
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.mu);
    respond(fd, "200 OK", "application/json", "{\"ok\":true}", 11);
}

static void *conn_main(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[4096];
    size_t n = 0;
    while (n + 1 < sizeof(req)) {
        ssize_t k = recv(fd, req + n, sizeof(req) - 1 - n, 0);
        if (k <= 0)
            break;
        n += (size_t)k;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    req[n] = 0;
    char method[8] = { 0 }, target[1024] = { 0 };
    if (sscanf(req, "%7s %1023s", method, target) == 2 && !strcmp(method, "GET")) {
        char *q = strchr(target, '?');
        if (q)
            *q++ = 0;
        if (!strcmp(target, "/"))
            serve_file(fd, "index.html");
        else if (!strcmp(target, "/events"))
            sse(fd);
        else if (!strncmp(target, "/api/", 5))
            api(fd, target + 5, q ? q : "");
        else
            serve_file(fd, target + 1);
    } else {
        respond(fd, "400 Bad Request", "text/plain", "bad request", 11);
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    int port = 8080;
    const char *media = "/tmp/fwsim-ui.media";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--static") && i + 1 < argc)
            static_dir = argv[++i];
        else if (!strcmp(argv[i], "--media") && i + 1 < argc)
            media = argv[++i];
        else {
            fprintf(stderr, "usage: fwsim-ui [--port N] [--static DIR] [--media PATH]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    pthread_mutex_init(&S.mu, NULL);
    pthread_cond_init(&S.cv, NULL);
    fw_config_default(&S.base);
    snprintf(S.media, sizeof(S.media), "%s", media);
    S.map = disk_open(&S.base.disk, NULL);
    if (!S.map) {
        fprintf(stderr, "fwsim-ui: disk_open failed\n");
        return 1;
    }
    disk_info_t info;
    disk_get_info(S.map, &info);
    S.cylinders = info.cylinders;
    S.rev_ms = info.rev_ms;
    S.skew = S.base.disk.cyl_skew;
    S.capacity = info.capacity_lbas;
    for (uint32_t z = 0; z < info.num_zones && z < DISK_MAX_ZONES; z++)
        if (disk_get_zone(S.map, z, &S.zones[z]) == DISK_OK)
            S.nzones = z + 1;
    sim_scale_defaults(&S.rp);
    S.req_rp = S.rp;
    tags_reset_locked();
    S.fw = open_drive(&S.rp, 1, 1);
    if (!S.fw) {
        fprintf(stderr, "fwsim-ui: cannot create drive on %s\n", S.media);
        return 1;
    }

    pthread_t runner, ticker;
    pthread_create(&runner, NULL, runner_main, NULL);
    pthread_create(&ticker, NULL, ticker_main, NULL);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* local only */
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 64) != 0) {
        perror("fwsim-ui: bind/listen");
        return 1;
    }
    printf("fwsim-ui: http://localhost:%d  (queue=%s, media=%s)\n", port, fw_queue_impl(), S.media);
    fflush(stdout);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, conn_main, (void *)(intptr_t)fd) != 0)
            close(fd);
        pthread_attr_destroy(&at);
    }
}
