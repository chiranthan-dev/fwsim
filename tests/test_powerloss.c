/*
 * test_powerloss.c - integration test of durability under power loss.
 *
 * First deterministic scenarios that pin down the exact contract of each
 * mode, then randomized trials (random data, random cut moment) through
 * the same pl_run_trial() the benchmark uses.
 */
#include "check.h"
#include "clock.h"
#include "pattern.h"
#include "powerloss.h"

static char path[128];

#define BLK 8   /* sectors per 4 KiB block */

static void write_block(fw_t *fw, uint32_t b, uint32_t ver, uint8_t flags)
{
    uint8_t buf[BLK * 512];
    for (uint32_t k = 0; k < BLK; k++)
        pattern_fill(buf + k * 512, 512, (uint64_t)b * BLK + k, ver);
    fw_cmd_t c = mkcmd(FW_OP_WRITE, 0, (uint64_t)b * BLK, BLK, buf);
    c.flags = flags;
    CHECK_EQ(fw_exec(fw, &c), FW_OK);       /* acknowledged */
}

/* Version of block b on media (0 = never written / lost), -1 = garbage. */
static int block_version(fw_t *fw, uint32_t b)
{
    uint8_t buf[BLK * 512];
    fw_cmd_t c = mkcmd(FW_OP_READ, 0, (uint64_t)b * BLK, BLK, buf);
    if (fw_exec(fw, &c) != FW_OK)
        return -1;
    uint32_t v0 = 0;
    for (uint32_t k = 0; k < BLK; k++) {
        uint32_t v = 0;
        int pc = pattern_check(buf + k * 512, 512, (uint64_t)b * BLK + k, &v);
        if (pc == PAT_BAD)
            return -1;
        if (k == 0)
            v0 = v;
        else if (v != v0)
            return -1;
    }
    return (int)v0;
}

static fw_t *reboot(fw_t *fw, fw_config_t *cfg)
{
    fw_power_cut(fw);
    fw_destroy(fw);                 /* DRAM cache is gone */
    fw_config_t again = *cfg;
    again.media_create = 0;
    fw = fw_create(&again, NULL);
    CHECK(fw != NULL);
    return fw;
}

static void test_write_back_without_flush_loses_acked_writes(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.cache_entries = 4096;       /* everything fits: nothing destaged */
    fw_t *fw = fw_create(&cfg, NULL);
    for (uint32_t b = 0; b < 50; b++)
        write_block(fw, b, 1, 0);
    fw = reboot(fw, &cfg);
    int lost = 0;
    for (uint32_t b = 0; b < 50; b++)
        lost += block_version(fw, b) == 0;
    CHECK_EQ(lost, 50);             /* all 50 acknowledged writes gone */
    fw_destroy(fw);
}

static void test_flush_makes_writes_durable(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.cache_entries = 4096;
    fw_t *fw = fw_create(&cfg, NULL);
    for (uint32_t b = 0; b < 50; b++)
        write_block(fw, b, 1, 0);
    fw_cmd_t f = mkcmd(FW_OP_FLUSH, 0, 0, 0, NULL);
    CHECK_EQ(fw_exec(fw, &f), FW_OK);
    for (uint32_t b = 0; b < 10; b++)
        write_block(fw, b, 2, 0);   /* acked after the flush: volatile */
    fw = reboot(fw, &cfg);
    int v1 = 0, v2 = 0;
    for (uint32_t b = 0; b < 50; b++) {
        int v = block_version(fw, b);
        v1 += v == 1;
        v2 += v == 2;
    }
    CHECK_EQ(v1, 50);               /* flushed data survived (old version) */
    CHECK_EQ(v2, 0);                /* post-flush writes were lost */
    fw_destroy(fw);
}

static void test_fua_and_write_through_never_lose(void)
{
    for (int mode = 0; mode < 2; mode++) {
        fw_config_t cfg;
        test_config(&cfg, path);
        cfg.cache_entries = 4096;
        cfg.write_cache = mode == 0;   /* 0: WB + FUA, 1: write-through */
        fw_t *fw = fw_create(&cfg, NULL);
        for (uint32_t b = 0; b < 50; b++)
            write_block(fw, b, 3, mode == 0 ? FW_CMD_FUA : 0);
        fw = reboot(fw, &cfg);
        int ok = 0;
        for (uint32_t b = 0; b < 50; b++)
            ok += block_version(fw, b) == 3;
        CHECK_EQ(ok, 50);
        fw_destroy(fw);
    }
}

/* Cut power in the middle of one long write-through write. Sectors are
 * written in order as they pass under the head, so after reboot we must
 * find a prefix of new sectors followed only by never-written ones - and
 * the command must never have been acknowledged. */
static void test_torn_write_is_a_clean_prefix(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.write_cache = 0;
    cfg.time_scale = 0.3;            /* ~18 ms of drive time -> ~5.4 ms wall */
    const uint32_t n = 2048;
    fw_t *fw = fw_create(&cfg, NULL);
    uint8_t *buf = malloc((size_t)n * 512);
    for (uint32_t k = 0; k < n; k++)
        pattern_fill(buf + (size_t)k * 512, 512, 200000 + k, 1);
    fw_cmd_t c = mkcmd(FW_OP_WRITE, 0, 200000, n, buf);
    CHECK_EQ(fw_submit(fw, &c), FW_OK);
    struct timespec ts = { 0, 3000000 };     /* 3 ms: mid-transfer */
    nanosleep(&ts, NULL);
    fw_power_cut(fw);
    CHECK(fw_poll(fw, 0) == NULL);           /* never acknowledged */
    fw_stats_t st;
    fw_get_stats(fw, &st);
    CHECK_EQ(st.torn_writes, 1);
    fw_destroy(fw);

    cfg.media_create = 0;
    cfg.time_scale = 0.0;
    fw = fw_create(&cfg, NULL);
    fw_cmd_t r = mkcmd(FW_OP_READ, 0, 200000, n, buf);
    CHECK_EQ(fw_exec(fw, &r), FW_OK);
    uint32_t prefix = 0;
    int in_prefix = 1, violations = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t v = 0;
        int pc = pattern_check(buf + (size_t)k * 512, 512, 200000 + k, &v);
        if (in_prefix && pc == PAT_OK) {
            prefix++;
        } else {
            in_prefix = 0;
            if (pc != PAT_ZERO)
                violations++;
        }
    }
    CHECK_EQ(violations, 0);
    CHECK(prefix < n);
    printf("    (torn write: %u of %u sectors reached the media)\n", prefix, n);
    free(buf);
    fw_destroy(fw);
}

/* Randomized trials. Under the sanitizers each trial is much slower, so we
 * run fewer of them; the invariants are the same. */
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define TRIALS 3
#else
#define TRIALS 12
#endif

static void test_randomized_trials(void)
{
    uint64_t lost[PL_MODE_COUNT] = { 0 }, acked[PL_MODE_COUNT] = { 0 };
    for (int m = 0; m < PL_MODE_COUNT; m++) {
        for (int t = 0; t < TRIALS; t++) {
            pl_params_t p;
            pl_params_default(&p);
            p.mode = (pl_mode_t)m;
            p.fw.media_path = path;
            p.fw.time_scale = 0.01;
            p.fw.cache_entries = 2048;
            p.fw.capacity_lbas = 1u << 20;
            p.blocks = 2048;
            p.flush_every = 16;
            p.cut_min_ms = 3;
            p.cut_max_ms = 15;
            p.seed = (uint64_t)(m * 1000 + t + 1);
            pl_result_t r;
            CHECK_EQ(pl_run_trial(&p, &r), 0);
            CHECK_EQ(r.violations, 0);   /* a durability promise was broken */
            CHECK_EQ(r.unreadable, 0);
            CHECK(r.writes_acked > 0);
            if (m == PL_WT || m == PL_WB_FUA)
                CHECK_EQ(r.lost, 0);
            lost[m] += r.lost;
            acked[m] += r.writes_acked;
        }
        printf("    %-14s %d trials: %" PRIu64 " acked writes, %" PRIu64 " lost\n",
               pl_mode_str((pl_mode_t)m), TRIALS, acked[m], lost[m]);
    }
    CHECK(lost[PL_WB_NOFLUSH] > 0);      /* write-back really is volatile */
}

int main(void)
{
    tmp_media(path, sizeof(path), "powerloss");
    printf("test_powerloss (queue=%s)\n", fw_queue_impl());
    RUN_TEST(test_write_back_without_flush_loses_acked_writes);
    RUN_TEST(test_flush_makes_writes_durable);
    RUN_TEST(test_fua_and_write_through_never_lose);
    RUN_TEST(test_torn_write_is_a_clean_prefix);
    RUN_TEST(test_randomized_trials);
    unlink(path);
    return test_summary("test_powerloss");
}
