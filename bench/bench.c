/*
 * bench.c - every experiment behind the README's results, written as rows
 * of a long-format CSV:  experiment,variant,metric,value,unit
 *
 *   fwbench --csv results/results.csv [--append] [--all | --only NAME]...
 *           [--quick] [--trials N] [--time-scale X]
 *
 * Experiments (NAME):
 *   cache_mode     write-back vs write-through on four workloads
 *   destage_order  LBA-sorted vs age-ordered destaging
 *   readahead      sequential reads with and without read-ahead
 *   queue_micro    mutex ring vs lock-free SPSC ring, raw handoff rate
 *   queue_e2e      firmware command rate with THIS build's queue
 *   powerloss      the durability experiment, many trials per mode
 *   integrity      CRC detection, retries and MEDIUM_ERROR under faults
 *
 * Units: unless a metric says "wall", times are DRIVE time: measured wall
 * time divided by the time scale (media time from disk-sim is played back
 * at time_scale x real time). See README "How time works".
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clock.h"
#include "crc32.h"
#include "fwsim.h"
#include "pattern.h"
#include "powerloss.h"
#include "ring_mutex.h"
#include "ring_spsc.h"
#include "util.h"
#include "workload.h"

static FILE  *csv;
static FILE  *trials_csv;
static double g_scale = 0.25;
static int    g_quick;
static int    g_trials = 100;
static char   g_media[256];

static void row(const char *exp, const char *variant, const char *metric,
                double value, const char *unit)
{
    fprintf(csv, "%s,%s,%s,%.6g,%s\n", exp, variant, metric, value, unit);
    fflush(csv);
}

static void base_config(fw_config_t *cfg)
{
    fw_config_default(cfg);
    cfg->media_path = g_media;
    cfg->media_create = 1;
    cfg->time_scale = g_scale;
}

static uint64_t scaled(uint64_t n)
{
    return g_quick ? (n / 5 ? n / 5 : 1) : n;
}

static double ms_drive(double ns)
{
    return ns / 1e6 / g_scale;
}

/* Run one workload on a fresh drive and emit its metrics. */
static void run_workload(const char *exp, const char *variant, const fw_config_t *cfg,
                         const wl_params_t *wl, int preload_region)
{
    fw_config_t live = *cfg;
    if (preload_region) {
        /* Fill the region with real (CRC-protected) data first, on a drive
         * with media time switched off so the preload is instant, then
         * power-cycle cleanly and reopen the same media with real timing
         * and a cold cache. */
        fw_config_t pre_cfg = *cfg;
        pre_cfg.time_scale = 0.0;
        fw_t *pre_fw = fw_create(&pre_cfg, NULL);
        wl_params_t pre = *wl;
        pre.kind = WL_SEQ_WRITE;
        pre.xfer_sectors = 256;
        pre.qd = 1;
        pre.ops = (wl->region_lbas ? wl->region_lbas : 4096) / 256;
        pre.final_flush = 1;
        wl_result_t pr;
        if (!pre_fw || wl_run(pre_fw, &pre, &pr) != 0) {
            fprintf(stderr, "bench: preload failed\n");
            exit(1);
        }
        wl_result_free(&pr);
        fw_shutdown(pre_fw);
        fw_destroy(pre_fw);
        live.media_create = 0;
    }
    fw_t *fw = fw_create(&live, NULL);
    if (!fw) {
        fprintf(stderr, "bench: fw_create failed\n");
        exit(1);
    }
    fw_stats_t before;
    fw_get_stats(fw, &before);

    wl_result_t r;
    if (wl_run(fw, wl, &r) != 0)
        fprintf(stderr, "bench: workload %s/%s failed\n", exp, variant);
    fw_stats_t st;
    fw_get_stats(fw, &st);
    fw_destroy(fw);

    double secs = (double)r.wall_ns / 1e9 / g_scale;
    double secs_dur = (double)r.wall_ns_durable / 1e9 / g_scale;
    lat_summary_t a, rd, wr;
    lat_summarize(&r.lat_all, &a);
    lat_summarize(&r.lat_read, &rd);
    lat_summarize(&r.lat_write, &wr);

    row(exp, variant, "ops", (double)r.ops, "count");
    row(exp, variant, "errors", (double)r.errors, "count");
    row(exp, variant, "iops", (double)r.ops / secs, "IOPS");
    row(exp, variant, "iops_durable", (double)r.ops / secs_dur, "IOPS");
    row(exp, variant, "mbps", (double)r.ops * wl->xfer_sectors * 512 / 1e6 / secs, "MB/s");
    row(exp, variant, "lat_avg", ms_drive(a.avg_ns), "ms");
    row(exp, variant, "lat_p50", ms_drive(a.p50_ns), "ms");
    row(exp, variant, "lat_p95", ms_drive(a.p95_ns), "ms");
    row(exp, variant, "lat_p99", ms_drive(a.p99_ns), "ms");
    if (wr.n)
        row(exp, variant, "write_lat_p99", ms_drive(wr.p99_ns), "ms");
    if (rd.n)
        row(exp, variant, "read_lat_p99", ms_drive(rd.p99_ns), "ms");
    uint64_t hits = st.read_hit_sectors - before.read_hit_sectors;
    uint64_t miss = st.read_miss_sectors - before.read_miss_sectors;
    if (hits + miss)
        row(exp, variant, "read_hit_rate", 100.0 * (double)hits / (double)(hits + miss), "%");
    row(exp, variant, "media_busy", st.media_busy_ms - before.media_busy_ms, "ms");
    row(exp, variant, "destage_runs", (double)(st.destage_runs - before.destage_runs), "count");
    row(exp, variant, "forced_destages",
        (double)(st.forced_destages - before.forced_destages), "count");
    printf("  %-14s %-22s %8.1f IOPS (%8.1f durable)  p50 %7.2f  p99 %7.2f ms\n", exp,
           variant, (double)r.ops / secs, (double)r.ops / secs_dur, ms_drive(a.p50_ns),
           ms_drive(a.p99_ns));
    wl_result_free(&r);
}

/* ---- cache_mode ------------------------------------------------------------ */

static void exp_cache_mode(void)
{
    printf("cache_mode\n");
    struct {
        const char *name;
        wl_kind_t   kind;
        uint64_t    ops;
        uint64_t    region;
        double      hot_frac, hot_prob;
        int         preload;
    } w[] = {
        /* sustained random 4K writes over the whole 100 GB disk */
        { "randwrite_full", WL_RAND_WRITE, 1500, 0, 0, 0, 0 },
        /* random 4K writes confined to 1 GiB */
        { "randwrite_1g", WL_RAND_WRITE, 1500, 2097152, 0, 0, 0 },
        /* 70% read / 30% write, 90% of accesses to a hot 5% (1.6 MiB) of
         * a 32 MiB region: the hot set fits in the 2 MiB cache */
        { "mixed_hot", WL_MIXED, 2000, 65536, 0.05, 0.90, 1 },
        /* random 4K reads over the whole disk (cache cannot help) */
        { "randread_full", WL_RAND_READ, 800, 0, 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
        for (int wb = 0; wb <= 1; wb++) {
            fw_config_t cfg;
            base_config(&cfg);
            cfg.write_cache = wb;
            wl_params_t p;
            wl_params_default(&p);
            p.kind = w[i].kind;
            p.ops = scaled(w[i].ops);
            p.qd = 4;
            p.region_lbas = w[i].region;
            p.hot_frac = w[i].hot_frac;
            p.hot_prob = w[i].hot_prob;
            p.read_frac = 0.7;
            p.final_flush = 1;
            p.seed = 1000 + i;
            char variant[64];
            snprintf(variant, sizeof(variant), "%s/%s", w[i].name,
                     wb ? "write_back" : "write_through");
            run_workload("cache_mode", variant, &cfg, &p, w[i].preload);
        }
    }
}

/* ---- destage_order ------------------------------------------------------------ */

static void exp_destage_order(void)
{
    printf("destage_order\n");
    for (int sorted = 1; sorted >= 0; sorted--) {
        fw_config_t cfg;
        base_config(&cfg);
        cfg.destage_sorted = sorted;
        wl_params_t p;
        wl_params_default(&p);
        p.kind = WL_RAND_WRITE;
        p.ops = scaled(1500);
        p.qd = 4;
        p.final_flush = 1;
        p.seed = 77;
        run_workload("destage_order", sorted ? "lba_sorted" : "age_order", &cfg, &p, 0);
    }
}

/* ---- readahead ---------------------------------------------------------------- */

static void exp_readahead(void)
{
    printf("readahead\n");
    for (int ra = 1; ra >= 0; ra--) {
        fw_config_t cfg;
        base_config(&cfg);
        cfg.read_ahead_sectors = ra ? 256 : 0;
        wl_params_t p;
        wl_params_default(&p);
        p.kind = WL_SEQ_READ;
        p.ops = scaled(2000);
        p.xfer_sectors = 128;     /* 64 KiB */
        p.qd = 1;
        p.region_lbas = 2000 * 128;
        p.seed = 5;
        run_workload("readahead", ra ? "on" : "off", &cfg, &p, 0);
    }
}

/* ---- queue_micro ---------------------------------------------------------------- */

typedef struct {
    int      lockfree;
    void    *ring;
    uint64_t n;
} qarg_t;

static void *q_producer(void *arg)
{
    qarg_t *a = arg;
    for (uint64_t i = 1; i <= a->n; i++) {
        void *it = (void *)(uintptr_t)i;
        if (a->lockfree)
            spsc_push(a->ring, it);
        else
            rbm_push(a->ring, it);
    }
    return NULL;
}

/* Returns million items per second for one producer -> one consumer. */
static double queue_handoff_rate(int lockfree, uint64_t n)
{
    ring_spsc_t rs;
    ring_mutex_t rm;
    qarg_t a = { lockfree, lockfree ? (void *)&rs : (void *)&rm, n };
    if (lockfree)
        spsc_init(&rs, 32);
    else
        rbm_init(&rm, 32);
    pthread_t p;
    uint64_t t0 = now_ns();
    pthread_create(&p, NULL, q_producer, &a);
    uint64_t got = 0, sum = 0;
    while (got < n) {
        void *it;
        int ok = lockfree ? spsc_pop_timed(&rs, &it, 1000000000ull)
                          : rbm_pop_timed(&rm, &it, 1000000000ull);
        if (!ok)
            break;
        sum += (uintptr_t)it;
        got++;
    }
    uint64_t dt = now_ns() - t0;
    pthread_join(p, NULL);
    if (lockfree)
        spsc_destroy(&rs);
    else
        rbm_destroy(&rm);
    if (got != n || sum != n * (n + 1) / 2) {
        fprintf(stderr, "bench: queue lost items!\n");
        exit(1);
    }
    return (double)n / ((double)dt / 1e9) / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void exp_queue_micro(void)
{
    printf("queue_micro\n");
    for (int lf = 1; lf >= 0; lf--) {
        double v[5];
        uint64_t n = scaled(lf ? 20000000 : 4000000);
        for (int k = 0; k < 5; k++)
            v[k] = queue_handoff_rate(lf, n);
        qsort(v, 5, sizeof(v[0]), cmp_double);
        const char *name = lf ? "lockfree_spsc" : "mutex_condvar";
        row("queue_micro", name, "handoff_rate_median", v[2], "Mitems/s(wall)");
        row("queue_micro", name, "handoff_rate_min", v[0], "Mitems/s(wall)");
        row("queue_micro", name, "handoff_rate_max", v[4], "Mitems/s(wall)");
        printf("  %-14s median %.2f M items/s (min %.2f, max %.2f)\n", name, v[2], v[0], v[4]);
    }
}

/* ---- queue_e2e ---------------------------------------------------------------- */

static void exp_queue_e2e(void)
{
    printf("queue_e2e (queue=%s)\n", fw_queue_impl());
    for (int qd = 1; qd <= 32; qd *= 32) {
        fw_config_t cfg;
        base_config(&cfg);
        cfg.time_scale = 0.0;       /* pure firmware + queue overhead */
        fw_t *fw = fw_create(&cfg, NULL);
        /* Warm 2048 sectors into the cache so every read is a hit. */
        wl_params_t pre;
        wl_params_default(&pre);
        pre.kind = WL_SEQ_WRITE;
        pre.ops = 8;
        pre.xfer_sectors = 256;
        pre.qd = 1;
        wl_result_t pr;
        wl_run(fw, &pre, &pr);
        wl_result_free(&pr);

        wl_params_t p;
        wl_params_default(&p);
        p.kind = WL_RAND_READ;
        p.ops = scaled(200000);
        p.xfer_sectors = 1;
        p.qd = (uint32_t)qd;
        p.region_lbas = 2048;
        wl_result_t r;
        wl_run(fw, &p, &r);
        fw_destroy(fw);
        lat_summary_t s;
        lat_summarize(&r.lat_all, &s);
        char variant[64];
        snprintf(variant, sizeof(variant), "%s/qd%d", fw_queue_impl(), qd);
        double kops = (double)r.ops / ((double)r.wall_ns / 1e9) / 1e3;
        row("queue_e2e", variant, "cmd_rate", kops, "Kcmds/s(wall)");
        row("queue_e2e", variant, "lat_avg", s.avg_ns / 1e3, "us(wall)");
        row("queue_e2e", variant, "lat_p99", s.p99_ns / 1e3, "us(wall)");
        printf("  %-14s %8.1f K cmds/s  avg %.2f us  p99 %.2f us\n", variant, kops,
               s.avg_ns / 1e3, s.p99_ns / 1e3);
        wl_result_free(&r);
    }
}

/* ---- powerloss ------------------------------------------------------------------ */

static void exp_powerloss(void)
{
    printf("powerloss (%d trials per mode)\n", g_trials);
    for (int m = 0; m < PL_MODE_COUNT; m++) {
        const char *mode = pl_mode_str((pl_mode_t)m);
        uint64_t acked = 0, lost = 0, viol = 0, unread = 0, max_lost = 0, with_loss = 0;
        uint64_t torn = 0, newer = 0, flushes = 0;
        double iops_sum = 0, lost_frac_sum = 0;
        for (int t = 0; t < g_trials; t++) {
            pl_params_t p;
            pl_params_default(&p);
            p.mode = (pl_mode_t)m;
            base_config(&p.fw);
            p.fw.capacity_lbas = 0;
            p.blocks = 8192;           /* 32 MiB working set */
            p.qd = 4;
            p.flush_every = 32;
            /* Cut at a uniformly random moment 50..1000 ms (drive time)
             * after power-on: covers both bursts the cache absorbs and
             * steady-state destaging. */
            p.cut_min_ms = 50.0 * g_scale;
            p.cut_max_ms = 1000.0 * g_scale;
            p.seed = (uint64_t)(m * 100000 + t + 1);
            pl_result_t r;
            if (pl_run_trial(&p, &r) != 0) {
                fprintf(stderr, "bench: power-loss trial failed\n");
                exit(1);
            }
            double iops = (double)r.writes_acked / (r.run_wall_ms / 1e3 / g_scale);
            fprintf(trials_csv, "%s,%d,%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f\n",
                    mode, t, r.run_wall_ms / g_scale, r.writes_acked, r.blocks_checked,
                    r.lost, r.violations, r.dirty_at_cut, r.torn_writes, r.newer_than_ack, iops);
            acked += r.writes_acked;
            lost += r.lost;
            viol += r.violations;
            unread += r.unreadable;
            torn += r.torn_writes;
            newer += r.newer_than_ack;
            flushes += r.flushes_acked;
            if (r.lost > max_lost)
                max_lost = r.lost;
            with_loss += r.lost > 0;
            iops_sum += iops;
            lost_frac_sum += r.blocks_checked ? (double)r.lost / (double)r.blocks_checked : 0;
        }
        double n = g_trials;
        row("powerloss", mode, "trials", n, "count");
        row("powerloss", mode, "acked_writes_per_trial", (double)acked / n, "writes");
        row("powerloss", mode, "lost_per_trial_avg", (double)lost / n, "writes");
        row("powerloss", mode, "lost_per_trial_max", (double)max_lost, "writes");
        row("powerloss", mode, "lost_total", (double)lost, "writes");
        row("powerloss", mode, "lost_fraction_avg", 100.0 * lost_frac_sum / n, "%");
        row("powerloss", mode, "trials_with_loss", (double)with_loss, "count");
        row("powerloss", mode, "durability_violations", (double)viol, "count");
        row("powerloss", mode, "unreadable_after_reboot", (double)unread, "count");
        row("powerloss", mode, "torn_writes", (double)torn, "count");
        row("powerloss", mode, "unacked_writes_that_landed", (double)newer, "count");
        row("powerloss", mode, "flushes_per_trial", (double)flushes / n, "count");
        row("powerloss", mode, "write_iops_avg", iops_sum / n, "IOPS");
        printf("  %-14s acked/trial %7.1f  lost/trial %6.1f (max %" PRIu64 ", %" PRIu64
               "/%d trials)  violations %" PRIu64 "  %.1f IOPS\n",
               mode, (double)acked / n, (double)lost / n, max_lost, with_loss, g_trials, viol,
               iops_sum / n);
    }
}

/* ---- integrity ------------------------------------------------------------------ */

static void exp_integrity(void)
{
    printf("integrity\n");
    row("integrity", "crc32", "check_value_123456789", (double)crc32("123456789", 9), "hex_as_int");
    const uint32_t nsec = (uint32_t)scaled(20000);
    const uint32_t persistent = 25;
    fw_config_t cfg;
    base_config(&cfg);
    cfg.time_scale = 0.0;
    cfg.capacity_lbas = nsec;
    fw_t *fw = fw_create(&cfg, NULL);
    uint8_t buf[256 * 512];
    for (uint32_t l = 0; l < nsec; l += 256) {
        uint32_t n = nsec - l < 256 ? nsec - l : 256;
        for (uint32_t k = 0; k < n; k++)
            pattern_fill(buf + k * 512, 512, l + k, 1);
        fw_cmd_t c = { .opcode = FW_OP_WRITE, .lba = l, .count = n, .buf = buf };
        fw_exec(fw, &c);
    }
    fw_shutdown(fw);
    fw_destroy(fw);

    cfg.media_create = 0;
    cfg.cache_entries = 64;
    cfg.read_ahead_sectors = 0;
    cfg.fault_read_flip_prob = 1e-3;   /* transient read-channel errors */
    cfg.read_retries = 3;
    fw = fw_create(&cfg, NULL);
    /* Persistent corruption at 25 distinct random LBAs. */
    uint8_t *bad = calloc(nsec, 1);
    uint64_t rng = 31337;
    for (uint32_t i = 0; i < persistent;) {
        uint32_t l = (uint32_t)rng_below64(&rng, nsec);
        if (bad[l])
            continue;
        bad[l] = 1;
        fw_inject_media_bitflip(fw, l, -1);
        i++;
    }
    uint64_t medium = 0, medium_correct_lba = 0, silent = 0, ok = 0;
    for (uint32_t l = 0; l < nsec; l++) {
        fw_cmd_t c = { .opcode = FW_OP_READ, .lba = l, .count = 1, .buf = buf };
        fw_status_t st = fw_exec(fw, &c);
        if (st == FW_ERR_MEDIUM) {
            medium++;
            medium_correct_lba += bad[l] && c.err_lba == l;
            continue;
        }
        uint32_t v = 0;
        if (st == FW_OK && pattern_check(buf, 512, l, &v) == PAT_OK && v == 1)
            ok++;
        else
            silent++;
    }
    fw_stats_t st;
    fw_get_stats(fw, &st);
    fw_destroy(fw);
    free(bad);
    row("integrity", "faults", "sectors_read", nsec, "count");
    row("integrity", "faults", "persistent_flips_injected", persistent, "count");
    row("integrity", "faults", "transient_flips_injected", (double)st.transient_flips, "count");
    row("integrity", "faults", "crc_errors_detected", (double)st.crc_errors, "count");
    row("integrity", "faults", "retries", (double)st.crc_retries, "count");
    row("integrity", "faults", "recovered_by_retry", (double)st.crc_recovered, "count");
    row("integrity", "faults", "medium_errors_returned", (double)medium, "count");
    row("integrity", "faults", "medium_errors_at_injected_lba", (double)medium_correct_lba, "count");
    row("integrity", "faults", "good_reads", (double)ok, "count");
    row("integrity", "faults", "silent_corruptions", (double)silent, "count");
    printf("  %u sectors: %" PRIu64 " transient + %u persistent flips -> %" PRIu64
           " CRC errors, %" PRIu64 " recovered, %" PRIu64 " MEDIUM_ERROR, %" PRIu64 " silent\n",
           nsec, st.transient_flips, persistent, st.crc_errors, st.crc_recovered, medium, silent);
}

/* ---- main ----------------------------------------------------------------------- */

typedef struct {
    const char *name;
    void (*fn)(void);
} exp_t;

static const exp_t experiments[] = {
    { "cache_mode", exp_cache_mode },     { "destage_order", exp_destage_order },
    { "readahead", exp_readahead },       { "queue_micro", exp_queue_micro },
    { "queue_e2e", exp_queue_e2e },       { "powerloss", exp_powerloss },
    { "integrity", exp_integrity },
};
#define NEXP (sizeof(experiments) / sizeof(experiments[0]))

int main(int argc, char **argv)
{
    const char *out = "results/results.csv";
    int append = 0, selected[NEXP] = { 0 }, any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--append")) {
            append = 1;
        } else if (!strcmp(argv[i], "--quick")) {
            g_quick = 1;
        } else if (!strcmp(argv[i], "--trials") && i + 1 < argc) {
            g_trials = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--time-scale") && i + 1 < argc) {
            g_scale = strtod(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--all")) {
            for (size_t k = 0; k < NEXP; k++)
                selected[k] = 1;
            any = 1;
        } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            const char *name = argv[++i];
            size_t k;
            for (k = 0; k < NEXP && strcmp(experiments[k].name, name); k++)
                ;
            if (k == NEXP) {
                fprintf(stderr, "unknown experiment %s\n", name);
                return 2;
            }
            selected[k] = 1;
            any = 1;
        } else {
            fprintf(stderr, "usage: fwbench --csv FILE [--append] [--all | --only NAME]... "
                            "[--quick] [--trials N] [--time-scale X]\n");
            return 2;
        }
    }
    if (!any)
        for (size_t k = 0; k < NEXP; k++)
            selected[k] = 1;
    if (g_quick && g_trials > 20)
        g_trials = 20;

    csv = fopen(out, append ? "a" : "w");
    if (!csv) {
        perror(out);
        return 1;
    }
    if (!append)
        fprintf(csv, "experiment,variant,metric,value,unit\n");
    char tpath[512];
    snprintf(tpath, sizeof(tpath), "%s", out);
    char *dot = strrchr(tpath, '/');
    snprintf(dot ? dot + 1 : tpath, sizeof(tpath) - (size_t)(dot ? dot + 1 - tpath : 0),
             "powerloss_trials.csv");
    if (selected[5]) {
        trials_csv = fopen(tpath, "w");
        if (!trials_csv) {
            perror(tpath);
            return 1;
        }
        fprintf(trials_csv, "mode,trial,cut_at_ms,acked_writes,blocks_checked,lost,"
                            "violations,dirty_sectors_at_cut,torn_writes,unacked_landed,"
                            "write_iops\n");
    }
    snprintf(g_media, sizeof(g_media), "/tmp/fwbench-%d.media", (int)getpid());

    if (!append) {
        row("meta", "config", "time_scale", g_scale, "wall_s_per_drive_s");
        row("meta", "config", "cache_entries", 4096, "sectors");
        row("meta", "config", "quick", g_quick, "bool");
    }
    for (size_t k = 0; k < NEXP; k++) {
        if (!selected[k])
            continue;
        uint64_t t0 = now_ns();
        experiments[k].fn();
        printf("  (%.1f s)\n", (double)(now_ns() - t0) / 1e9);
    }
    fclose(csv);
    if (trials_csv)
        fclose(trials_csv);
    unlink(g_media);
    return 0;
}
