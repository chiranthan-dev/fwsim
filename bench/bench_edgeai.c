/*
 * bench_edgeai.c - system benchmark of the write-protection policies on the
 * frozen physical model (README "Edge AI vibration classifier").
 *
 *   fwbench_edgeai --out FILE [--mix shock|vibration|mixed|quiet]... [--seeds N]
 *                  [--secs S] [--append]
 *
 * One run = one (mix, policy variant, seed): a fresh 1 GiB write-through
 * drive in a synthetic environment (env seed 5000 + seed: disjoint from the
 * classifier's training/validation/test seeds 1..200), a closed-loop host
 * doing random 4 KiB writes at QD 1 for S seconds of SIMULATED drive time,
 * then a reboot WITHOUT the environment and a read-back of every block
 * written. Everything runs at time_scale 0, so every number is drive time
 * from disk-sim and the run is exactly reproducible from its seed.
 *
 * Per run (one CSV row): writes, write IOPS, p99 write latency, corrupted
 * sectors found on read-back (and per 1,000 writes), silent corruptions
 * (must be 0), stall and needless-stall time per second, verified writes,
 * threshold IRQs, classifier windows. tools/edgeai/report.py turns the rows
 * into means with 95% confidence intervals.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "env_monitor.h"
#include "fwsim.h"
#include "pattern.h"
#include "periph/env_sim.h"
#include "util.h"

#define REGION_BLOCKS (1u << 18)       /* 4 KiB blocks: 1 GiB                 */
#define BLK_SECTORS   8u

typedef struct {
    const char *name;
    int         policy;
    uint32_t    thr_mg;
} variant_t;

/* P1 at 250 mg is the baseline's default; the other thresholds show the
 * whole trade-off curve so P2/P3 are not compared against one arbitrary
 * setting. */
static const variant_t variants[] = {
    { "P0_none", FW_VIB_NONE, 0 },
    { "P1_thr150", FW_VIB_THRESHOLD, 150 },
    { "P1_thr250", FW_VIB_THRESHOLD, 250 },
    { "P1_thr500", FW_VIB_THRESHOLD, 500 },
    { "P1_thr1000", FW_VIB_THRESHOLD, 1000 },
    { "P2_ai", FW_VIB_AI, 0 },
    { "P3_hybrid", FW_VIB_HYBRID, 250 },
};
#define NVAR (sizeof(variants) / sizeof(variants[0]))

/* Command timestamps in drive time, from the trace (written on the firmware
 * thread before the completion is posted; read by the host after fw_exec
 * returns, which the completion queue's release/acquire orders). */
static double t_start, t_done;

static void on_trace(void *ctx, const fw_trace_t *ev)
{
    (void)ctx;
    if (ev->kind == FW_TRACE_CMD_START)
        t_start = ev->sim_ms;
    else if (ev->kind == FW_TRACE_CMD_DONE)
        t_done = ev->sim_ms;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static char g_media[256];

static void base_cfg(fw_config_t *c)
{
    fw_config_default(c);
    c->media_path = g_media;
    c->media_create = 1;
    c->time_scale = 0.0;
    c->cache_entries = 256;
    c->write_cache = 0;
    c->read_ahead_sectors = 0;
    c->idle_destage_ms = 0.0;
    c->capacity_lbas = (uint64_t)REGION_BLOCKS * BLK_SECTORS;
}

static void run(FILE *out, int mix, const variant_t *v, unsigned seed, double secs)
{
    fw_config_t cfg;
    base_cfg(&cfg);
    cfg.vib_env = mix;
    cfg.vib_seed = 5000u + seed;
    cfg.vib_policy = v->policy;
    cfg.vib_threshold_mg = v->thr_mg;
    cfg.trace = on_trace;
    fw_t *fw = fw_create(&cfg, NULL);
    if (!fw) {
        fprintf(stderr, "fwbench_edgeai: fw_create failed\n");
        exit(1);
    }
    uint16_t *ver = calloc(REGION_BLOCKS, sizeof(*ver));
    size_t cap = 8192, n = 0;
    double *lat = malloc(cap * sizeof(*lat));
    uint8_t buf[BLK_SECTORS * 512];
    uint64_t rng = 0xB0B0000ull + seed;
    double end_ms = 0;
    uint64_t errors = 0;
    while (end_ms < secs * 1000.0) {
        uint32_t b = (uint32_t)rng_below64(&rng, REGION_BLOCKS);
        uint64_t lba = (uint64_t)b * BLK_SECTORS;
        ver[b]++;
        for (unsigned k = 0; k < BLK_SECTORS; k++)
            pattern_fill(buf + k * 512, 512, lba + k, ver[b]);
        fw_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = FW_OP_WRITE;
        c.lba = lba;
        c.count = BLK_SECTORS;
        c.buf = buf;
        if (fw_exec(fw, &c) != FW_OK)
            errors++;
        if (n == cap) {
            cap *= 2;
            lat = realloc(lat, cap * sizeof(*lat));
        }
        lat[n++] = t_done - t_start;
        end_ms = t_done;
    }
    fw_shutdown(fw);
    fw_stats_t st;
    fw_get_stats(fw, &st);
    fw_destroy(fw);

    /* Reboot in a quiet room and read every block back. */
    base_cfg(&cfg);
    cfg.media_create = 0;
    fw = fw_create(&cfg, NULL);
    uint64_t corrupt = 0, silent = 0;
    for (uint32_t b = 0; b < REGION_BLOCKS; b++) {
        if (!ver[b])
            continue;
        uint64_t lba = (uint64_t)b * BLK_SECTORS;
        fw_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = FW_OP_READ;
        c.lba = lba;
        c.count = BLK_SECTORS;
        c.buf = buf;
        fw_status_t s = fw_exec(fw, &c);
        for (unsigned k = 0; k < BLK_SECTORS; k++) {
            if (s != FW_OK) {         /* find exactly which sectors fail */
                fw_cmd_t one;
                memset(&one, 0, sizeof(one));
                one.opcode = FW_OP_READ;
                one.lba = lba + k;
                one.count = 1;
                one.buf = buf + k * 512;
                if (fw_exec(fw, &one) == FW_ERR_MEDIUM) {
                    corrupt++;
                    continue;
                }
            }
            uint32_t got = 0;
            if (pattern_check(buf + k * 512, 512, lba + k, &got) != PAT_OK || got != ver[b])
                silent++;
        }
    }
    fw_destroy(fw);

    qsort(lat, n, sizeof(*lat), cmp_d);
    double p99 = lat[(size_t)(0.99 * (double)(n - 1) + 0.5)];
    double sim_s = end_ms / 1000.0;
    fprintf(out, "%s,%s,%u,%zu,%.3f,%.3f,%.4f,%" PRIu64 ",%.4f,%" PRIu64 ",%.3f,%.3f,%" PRIu64
            ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
            env_mix_str(mix), v->name, seed, n, sim_s, (double)n / sim_s, p99, corrupt,
            1000.0 * (double)corrupt / (double)n, silent, st.vib_stall_ms / sim_s,
            st.vib_false_stall_ms / sim_s, st.vib_verified_writes, st.vib_verify_rewrites,
            st.vib_shock_irqs, st.vib_windows, st.vib_write_aborts, st.vib_stall_timeouts,
            st.vib_offtrack_sectors, errors);
    fflush(out);
    free(lat);
    free(ver);
}

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    int mixes[8], nmix = 0, append = 0;
    unsigned seeds = 30;
    double secs = 30.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
        else if (!strcmp(argv[i], "--mix") && i + 1 < argc && nmix < 8) {
            int m = env_mix_parse(argv[++i]);
            if (m <= 0) {
                fprintf(stderr, "unknown mix %s\n", argv[i]);
                return 2;
            }
            mixes[nmix++] = m;
        } else if (!strcmp(argv[i], "--seeds") && i + 1 < argc)
            seeds = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc)
            secs = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--append"))
            append = 1;
        else {
            fprintf(stderr, "usage: fwbench_edgeai --out FILE [--mix M]... [--seeds N] "
                            "[--secs S] [--append]\n");
            return 2;
        }
    }
    if (!out_path)
        return 2;
    if (!nmix) {
        mixes[nmix++] = ENV_MIX_SHOCK;
        mixes[nmix++] = ENV_MIX_VIBRATION;
        mixes[nmix++] = ENV_MIX_MIXED;
        mixes[nmix++] = ENV_MIX_QUIET;
    }
    FILE *out = fopen(out_path, append ? "a" : "w");
    if (!out) {
        perror(out_path);
        return 1;
    }
    if (!append)
        fprintf(out, "mix,variant,seed,writes,sim_s,write_iops,p99_ms,corrupt_sectors,"
                     "corrupt_per_1k_writes,silent,stall_ms_per_s,false_stall_ms_per_s,"
                     "verified_writes,verify_rewrites,threshold_irqs,classifier_windows,"
                     "gate_aborts,stall_timeouts,offtrack_raw,cmd_errors\n");
    /* The media file is sparse (1 GiB) and each run touches ~20 MB of it.
     * In RAM (/dev/shm) four parallel benchmark processes do not throttle
     * each other on disk writeback; results are simulated drive time and
     * do not depend on where the file lives. */
    const char *dir = access("/dev/shm", W_OK) == 0 ? "/dev/shm" : "/tmp";
    snprintf(g_media, sizeof(g_media), "%s/fwbench-edgeai-%d.media", dir, (int)getpid());
    for (int m = 0; m < nmix; m++)
        for (size_t v = 0; v < NVAR; v++) {
            for (unsigned s = 1; s <= seeds; s++)
                run(out, mixes[m], &variants[v], s, secs);
            printf("  %-10s %-11s %u seeds done\n", env_mix_str(mixes[m]), variants[v].name,
                   seeds);
            fflush(stdout);
        }
    fclose(out);
    unlink(g_media);
    return 0;
}
