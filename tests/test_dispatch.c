/*
 * test_dispatch.c - the firmware command layer end to end: opcode
 * dispatch, argument validation and sense data, NCQ tags, command state,
 * and the write-back cache policies (FLUSH, FUA, write-through,
 * SET_FEATURES, destage triggers, read-ahead, clean shutdown).
 */
#include <stdatomic.h>

#include "check.h"
#include "clock.h"
#include "pattern.h"

static char path[128];

static fw_t *open_drive(fw_config_t *cfg)
{
    fw_t *fw = fw_create(cfg, NULL);
    CHECK(fw != NULL);
    return fw;
}

static void write_pattern(fw_t *fw, uint64_t lba, uint32_t n, uint32_t ver, uint8_t flags)
{
    uint32_t ss = fw_sector_size(fw);
    uint8_t *buf = malloc((size_t)n * ss);
    for (uint32_t k = 0; k < n; k++)
        pattern_fill(buf + (size_t)k * ss, ss, lba + k, ver);
    fw_cmd_t c = mkcmd(FW_OP_WRITE, 3, lba, n, buf);
    c.flags = flags;
    CHECK_EQ(fw_exec(fw, &c), FW_OK);
    free(buf);
}

/* Returns the number of sectors that do NOT hold version `ver`. */
static int verify_pattern(fw_t *fw, uint64_t lba, uint32_t n, uint32_t ver)
{
    uint32_t ss = fw_sector_size(fw);
    uint8_t *buf = malloc((size_t)n * ss);
    fw_cmd_t c = mkcmd(FW_OP_READ, 4, lba, n, buf);
    int bad = 0;
    if (fw_exec(fw, &c) != FW_OK) {
        bad = (int)n;
    } else {
        for (uint32_t k = 0; k < n; k++) {
            uint32_t v = 0;
            if (pattern_check(buf + (size_t)k * ss, ss, lba + k, &v) != PAT_OK || v != ver)
                bad++;
        }
    }
    free(buf);
    return bad;
}

static fw_status_t simple(fw_t *fw, fw_cmd_t c)
{
    return fw_exec(fw, &c);
}

/*
 * Background work (read-ahead, threshold destaging) runs on the firmware
 * thread AFTER a command's completion is posted, so right after fw_exec()
 * returns it may still be in progress. The firmware is strictly serial,
 * so completing one more command (a harmless IDENTIFY) acts as a barrier:
 * everything queued before it is finished. Only then read the counters.
 */
static void sync_stats(fw_t *fw, fw_stats_t *st)
{
    uint8_t buf[512];
    fw_cmd_t c = mkcmd(FW_OP_IDENTIFY, 31, 0, 1, buf);
    CHECK_EQ(fw_exec(fw, &c), FW_OK);
    fw_get_stats(fw, st);
}

/* ---- dispatch and validation --------------------------------------------- */

static void test_identify(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = open_drive(&cfg);
    uint8_t buf[512];
    memset(buf, 0xEE, sizeof(buf));
    fw_cmd_t c = mkcmd(FW_OP_IDENTIFY, 0, 0, 1, buf);
    CHECK_EQ(fw_exec(fw, &c), FW_OK);
    fw_identify_t id;
    memcpy(&id, buf, sizeof(id));
    CHECK_EQ(id.capacity_lbas, fw_capacity(fw));
    CHECK_EQ(id.capacity_lbas, 1u << 20);
    CHECK_EQ(id.sector_size, 512);
    CHECK_EQ(id.queue_depth, FW_MAX_TAGS);
    CHECK_EQ(id.write_cache_enabled, 1);
    CHECK_EQ(id.fua_supported, 1);
    CHECK(strcmp(id.queue_impl, fw_queue_impl()) == 0);
    CHECK(strncmp(id.model, "FWSIM", 5) == 0);
    CHECK_EQ(buf[511], 0);                       /* zero padded to a sector */
    CHECK_EQ(c.state, FW_CMD_COMPLETED);
    CHECK(c.t_submit_ns <= c.t_start_ns && c.t_start_ns <= c.t_done_ns);

    /* IDENTIFY with no buffer is an invalid field, not a crash. */
    CHECK_EQ(simple(fw, mkcmd(FW_OP_IDENTIFY, 0, 0, 1, NULL)), FW_ERR_INVALID_FIELD);
    fw_destroy(fw);
}

static void test_invalid_opcode_and_sense(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = open_drive(&cfg);
    uint8_t buf[512];
    fw_cmd_t c = mkcmd(FW_OP_COUNT, 1, 0, 1, buf);
    CHECK_EQ(fw_exec(fw, &c), FW_ERR_INVALID_OPCODE);
    CHECK_EQ(c.sense.key, 0x5);          /* ILLEGAL REQUEST */
    CHECK_EQ(c.sense.asc, 0x20);         /* INVALID COMMAND OPERATION CODE */
    c = mkcmd(0xFF, 1, 0, 1, buf);
    CHECK_EQ(fw_exec(fw, &c), FW_ERR_INVALID_OPCODE);

    /* Every status has a name and the documented sense key. */
    CHECK_EQ(fw_status_sense(FW_OK).key, 0x0);
    CHECK_EQ(fw_status_sense(FW_ERR_INVALID_LBA).asc, 0x21);
    CHECK_EQ(fw_status_sense(FW_ERR_MEDIUM).key, 0x3);
    CHECK_EQ(fw_status_sense(FW_ERR_MEDIUM).asc, 0x11);
    CHECK_EQ(fw_status_sense(FW_ERR_TAG_IN_USE).key, 0xB);
    CHECK_EQ(fw_status_sense(FW_ERR_HARDWARE).key, 0x4);
    for (int s = 0; s < FW_STATUS_COUNT; s++)
        CHECK(strcmp(fw_status_str((fw_status_t)s), "UNKNOWN") != 0);
    CHECK(strcmp(fw_opcode_str(FW_OP_FLUSH), "FLUSH") == 0);
    CHECK(strcmp(fw_opcode_str(200), "INVALID") == 0);

    fw_stats_t st;
    sync_stats(fw, &st);
    CHECK_EQ(st.cmd_errors, 2);
    fw_destroy(fw);
}

static void test_range_validation(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = open_drive(&cfg);
    uint64_t cap = fw_capacity(fw);
    uint8_t *buf = calloc(4, 512);

    CHECK_EQ(simple(fw, mkcmd(FW_OP_READ, 0, 0, 0, buf)), FW_ERR_INVALID_FIELD);
    CHECK_EQ(simple(fw, mkcmd(FW_OP_READ, 0, 0, 1, NULL)), FW_ERR_INVALID_FIELD);
    CHECK_EQ(simple(fw, mkcmd(FW_OP_WRITE, 0, 0, 1, NULL)), FW_ERR_INVALID_FIELD);
    CHECK_EQ(simple(fw, mkcmd(FW_OP_READ, 0, 0, 100000, buf)), FW_ERR_INVALID_FIELD);

    fw_cmd_t c = mkcmd(FW_OP_READ, 0, cap, 1, buf);
    CHECK_EQ(fw_exec(fw, &c), FW_ERR_INVALID_LBA);
    CHECK_EQ(c.err_lba, cap);
    CHECK_EQ(c.sense.asc, 0x21);
    CHECK_EQ(simple(fw, mkcmd(FW_OP_WRITE, 0, cap - 1, 2, buf)), FW_ERR_INVALID_LBA);
    /* lba + count would overflow uint64: must not wrap around to "valid". */
    CHECK_EQ(simple(fw, mkcmd(FW_OP_READ, 0, UINT64_MAX - 1, 4, buf)), FW_ERR_INVALID_LBA);
    /* The last sector itself is fine. */
    CHECK_EQ(simple(fw, mkcmd(FW_OP_WRITE, 0, cap - 1, 1, buf)), FW_OK);
    CHECK_EQ(simple(fw, mkcmd(FW_OP_READ, 0, cap - 1, 1, buf)), FW_OK);
    free(buf);
    fw_destroy(fw);
}

/* ---- NCQ tags --------------------------------------------------------------- */

static void test_tags(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    /* Media time on, so the first command is still in flight when we try
     * to reuse its tag (a cold read costs several ms of drive time). */
    cfg.time_scale = 0.2;
    fw_t *fw = open_drive(&cfg);
    uint8_t a[512], b[512];

    fw_cmd_t bad = mkcmd(FW_OP_READ, FW_MAX_TAGS, 0, 1, a);
    CHECK_EQ(fw_submit(fw, &bad), FW_ERR_INVALID_FIELD);
    CHECK_EQ(fw_submit(fw, NULL), FW_ERR_INVALID_FIELD);

    fw_cmd_t c1 = mkcmd(FW_OP_READ, 5, 500000, 1, a);
    fw_cmd_t c2 = mkcmd(FW_OP_READ, 5, 1000, 1, b);
    CHECK_EQ(fw_submit(fw, &c1), FW_OK);
    CHECK_EQ(fw_submit(fw, &c2), FW_ERR_TAG_IN_USE);   /* overlapped command */
    fw_cmd_t *done = NULL;
    while (!done)
        done = fw_poll(fw, 1000);
    CHECK(done == &c1);
    CHECK_EQ(c1.status, FW_OK);
    /* Once completed, the tag is free again. */
    CHECK_EQ(fw_exec(fw, &c2), FW_OK);
    fw_destroy(fw);
}

/* All 32 tags outstanding at once; completions may be collected in any
 * order, every command must complete exactly once with correct data. */
static void test_full_queue_depth(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = open_drive(&cfg);
    uint32_t ss = fw_sector_size(fw);
    fw_cmd_t cmds[FW_MAX_TAGS];
    uint8_t *bufs = malloc((size_t)FW_MAX_TAGS * 8 * ss);
    for (int round = 0; round < 3; round++) {
        for (int t = 0; t < FW_MAX_TAGS; t++) {
            uint64_t lba = (uint64_t)t * 1000;
            uint8_t *b = bufs + (size_t)t * 8 * ss;
            for (int k = 0; k < 8; k++)
                pattern_fill(b + (size_t)k * ss, ss, lba + k, (uint32_t)round + 1);
            cmds[t] = mkcmd(FW_OP_WRITE, (uint8_t)t, lba, 8, b);
            CHECK_EQ(fw_submit(fw, &cmds[t]), FW_OK);
        }
        int seen[FW_MAX_TAGS] = { 0 };
        for (int n = 0; n < FW_MAX_TAGS;) {
            fw_cmd_t *c = fw_poll(fw, 100000);
            if (!c)
                continue;
            CHECK(c == &cmds[c->tag]);
            CHECK_EQ(c->status, FW_OK);
            CHECK_EQ(c->state, FW_CMD_COMPLETED);
            seen[c->tag]++;
            n++;
        }
        for (int t = 0; t < FW_MAX_TAGS; t++)
            CHECK_EQ(seen[t], 1);
    }
    for (int t = 0; t < FW_MAX_TAGS; t++)
        CHECK_EQ(verify_pattern(fw, (uint64_t)t * 1000, 8, 3), 0);
    free(bufs);
    fw_destroy(fw);
}

/* ---- cache policy ------------------------------------------------------------ */

static void test_write_back_then_flush(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = open_drive(&cfg);
    write_pattern(fw, 100, 16, 1, 0);
    fw_stats_t st;
    sync_stats(fw, &st);
    CHECK_EQ(st.cache_dirty, 16);              /* acknowledged, NOT on media */
    CHECK_EQ(st.media_writes, 0);
    CHECK_EQ(verify_pattern(fw, 100, 16, 1), 0);   /* served from cache */
    sync_stats(fw, &st);
    CHECK_EQ(st.read_hit_sectors, 16);

    CHECK_EQ(simple(fw, mkcmd(FW_OP_FLUSH, 0, 0, 0, NULL)), FW_OK);
    sync_stats(fw, &st);
    CHECK_EQ(st.cache_dirty, 0);
    CHECK_EQ(st.media_sectors_written, 16);
    CHECK_EQ(st.destage_runs, 1);              /* 16 adjacent sectors -> ONE write */
    CHECK_EQ(st.flushes, 1);
    /* A FLUSH with nothing dirty is a no-op that still succeeds. */
    CHECK_EQ(simple(fw, mkcmd(FW_OP_FLUSH, 0, 0, 0, NULL)), FW_OK);
    fw_destroy(fw);
}

static void test_fua_and_write_through(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = open_drive(&cfg);
    fw_stats_t st;

    write_pattern(fw, 0, 8, 1, 0);            /* dirty */
    write_pattern(fw, 0, 8, 2, FW_CMD_FUA);   /* FUA supersedes the dirty copy */
    sync_stats(fw, &st);
    CHECK_EQ(st.cache_dirty, 0);
    CHECK_EQ(st.media_sectors_written, 8);
    CHECK_EQ(st.write_hit_sectors, 8);
    CHECK_EQ(verify_pattern(fw, 0, 8, 2), 0);
    fw_destroy(fw);

    test_config(&cfg, path);
    cfg.write_cache = 0;                      /* write-through */
    fw = open_drive(&cfg);
    write_pattern(fw, 64, 8, 1, 0);
    sync_stats(fw, &st);
    CHECK_EQ(st.cache_dirty, 0);
    CHECK_EQ(st.media_sectors_written, 8);
    CHECK_EQ(verify_pattern(fw, 64, 8, 1), 0);
    sync_stats(fw, &st);
    CHECK_EQ(st.read_hit_sectors, 8);         /* still caches for reads */
    fw_destroy(fw);
}

static void test_set_features(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = open_drive(&cfg);
    fw_stats_t st;
    write_pattern(fw, 0, 8, 1, 0);

    fw_cmd_t c = mkcmd(FW_OP_SET_FEATURES, 0, 0, 0, NULL);
    c.feature = FW_FEAT_WRITE_CACHE;
    c.feature_value = 0;
    CHECK_EQ(fw_exec(fw, &c), FW_OK);
    sync_stats(fw, &st);
    CHECK_EQ(st.cache_dirty, 0);              /* disabling WCE flushed first */
    write_pattern(fw, 8, 8, 1, 0);
    sync_stats(fw, &st);
    CHECK_EQ(st.cache_dirty, 0);              /* now write-through */
    CHECK_EQ(st.media_sectors_written, 16);

    uint8_t idb[512];
    fw_cmd_t id = mkcmd(FW_OP_IDENTIFY, 0, 0, 1, idb);
    CHECK_EQ(fw_exec(fw, &id), FW_OK);
    fw_identify_t ident;
    memcpy(&ident, idb, sizeof(ident));        /* idb may be misaligned */
    CHECK_EQ(ident.write_cache_enabled, 0);

    c.feature_value = 1;
    CHECK_EQ(fw_exec(fw, &c), FW_OK);
    write_pattern(fw, 16, 8, 1, 0);
    sync_stats(fw, &st);
    CHECK_EQ(st.cache_dirty, 8);              /* write-back again */

    c.feature = 0x77;
    CHECK_EQ(fw_exec(fw, &c), FW_ERR_INVALID_FIELD);
    c.feature = FW_FEAT_WRITE_CACHE;
    c.feature_value = 2;
    CHECK_EQ(fw_exec(fw, &c), FW_ERR_INVALID_FIELD);
    fw_destroy(fw);
}

static void test_threshold_and_forced_destage(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.cache_entries = 64;                   /* high = 48, low = 32 */
    fw_t *fw = open_drive(&cfg);
    fw_stats_t st;
    for (uint32_t i = 0; i < 7; i++)          /* 56 dirty sectors > 48 */
        write_pattern(fw, (uint64_t)i * 8 * 37, 8, 1, 0);
    sync_stats(fw, &st);
    CHECK(st.threshold_destages >= 1);
    CHECK(st.cache_dirty < 48);
    CHECK_EQ(st.forced_destages, 0);

    /* A single 128-sector write cannot fit in 64 entries at all: the
     * firmware must destage synchronously in the middle of the command. */
    write_pattern(fw, 10000, 128, 2, 0);
    sync_stats(fw, &st);
    CHECK(st.forced_destages >= 1);
    CHECK_EQ(verify_pattern(fw, 10000, 128, 2), 0);
    for (uint32_t i = 0; i < 7; i++)
        CHECK_EQ(verify_pattern(fw, (uint64_t)i * 8 * 37, 8, 1), 0);
    CHECK_EQ(st.cmd_errors, 0);
    fw_destroy(fw);
}

static void test_unsorted_destage_is_also_correct(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.cache_entries = 64;
    cfg.destage_sorted = 0;
    fw_t *fw = open_drive(&cfg);
    for (uint32_t i = 0; i < 40; i++)
        write_pattern(fw, (uint64_t)((i * 7919) % 500) * 8, 8, i + 1, 0);
    CHECK_EQ(simple(fw, mkcmd(FW_OP_FLUSH, 0, 0, 0, NULL)), FW_OK);
    fw_shutdown(fw);
    fw_destroy(fw);
    cfg.media_create = 0;
    fw = open_drive(&cfg);
    for (uint32_t i = 0; i < 40; i++)
        CHECK_EQ(verify_pattern(fw, (uint64_t)((i * 7919) % 500) * 8, 8, i + 1), 0);
    fw_destroy(fw);
}

static void test_read_ahead(void)
{
    fw_config_t cfg;
    fw_stats_t st;
    for (int ra = 0; ra <= 1; ra++) {
        test_config(&cfg, path);
        cfg.read_ahead_sectors = ra ? 64 : 0;
        fw_t *fw = open_drive(&cfg);
        uint8_t buf[8 * 512];
        for (uint64_t i = 0; i < 40; i++) {
            fw_cmd_t c = mkcmd(FW_OP_READ, 1, 5000 + i * 8, 8, buf);
            CHECK_EQ(fw_exec(fw, &c), FW_OK);
        }
        sync_stats(fw, &st);
        if (ra) {
            CHECK(st.readahead_ops > 0);
            /* Only the first two reads miss; the stream then hits. */
            CHECK(st.read_hit_sectors >= 38 * 8);
        } else {
            CHECK_EQ(st.readahead_ops, 0);
            CHECK_EQ(st.read_hit_sectors, 0);
        }
        /* A random read does not trigger read-ahead. */
        uint64_t before = st.readahead_ops;
        fw_cmd_t c = mkcmd(FW_OP_READ, 1, 900000, 8, buf);
        CHECK_EQ(fw_exec(fw, &c), FW_OK);
        sync_stats(fw, &st);
        CHECK_EQ(st.readahead_ops, before);
        fw_destroy(fw);
    }
}

static void test_clean_shutdown_persists(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = open_drive(&cfg);
    write_pattern(fw, 777, 24, 9, 0);          /* only in cache */
    fw_shutdown(fw);                           /* STANDBY IMMEDIATE: destage */
    fw_cmd_t c = mkcmd(FW_OP_FLUSH, 0, 0, 0, NULL);
    CHECK_EQ(fw_submit(fw, &c), FW_ERR_ABORTED);   /* drive is off */
    fw_destroy(fw);

    cfg.media_create = 0;                      /* same platters, cold cache */
    fw = open_drive(&cfg);
    CHECK_EQ(verify_pattern(fw, 777, 24, 9), 0);
    fw_stats_t st;
    sync_stats(fw, &st);
    CHECK_EQ(st.read_miss_sectors, 24);        /* really came from media */
    /* Never-written sectors read back as zeros. */
    uint8_t z[512];
    memset(z, 0xFF, sizeof(z));
    CHECK_EQ(simple(fw, mkcmd(FW_OP_READ, 0, 123456, 1, z)), FW_OK);
    int nonzero = 0;
    for (size_t i = 0; i < sizeof(z); i++)
        nonzero |= z[i];
    CHECK_EQ(nonzero, 0);
    fw_destroy(fw);
}

/* ---- tracing ------------------------------------------------------------------ */

/* The trace callback runs on the firmware thread; the test thread reads the
 * counters afterwards, so they are atomics (TSan checks this). */
static atomic_int tr_start, tr_done, tr_host_writes, tr_destages, tr_bad_chs;

static void count_trace(void *ctx, const fw_trace_t *ev)
{
    (void)ctx;
    switch (ev->kind) {
    case FW_TRACE_CMD_START: atomic_fetch_add(&tr_start, 1); break;
    case FW_TRACE_CMD_DONE:  atomic_fetch_add(&tr_done, 1); break;
    case FW_TRACE_MEDIA:
        if (ev->is_write && ev->reason == FW_IO_HOST)
            atomic_fetch_add(&tr_host_writes, 1);
        if (ev->is_write && ev->reason == FW_IO_DESTAGE)
            atomic_fetch_add(&tr_destages, 1);
        if (!ev->has_chs || ev->spt == 0 || ev->sector >= ev->spt)
            atomic_fetch_add(&tr_bad_chs, 1);
        break;
    default: break;
    }
}

static void test_trace_events(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.trace = count_trace;
    fw_t *fw = open_drive(&cfg);
    write_pattern(fw, 64, 8, 1, FW_CMD_FUA);     /* media write, reason HOST */
    write_pattern(fw, 4096, 8, 1, 0);            /* cache only */
    CHECK_EQ(simple(fw, mkcmd(FW_OP_FLUSH, 0, 0, 0, NULL)), FW_OK);  /* destage */
    fw_stats_t st;
    sync_stats(fw, &st);
    fw_shutdown(fw);                              /* thread joined: reads are final */
    CHECK_EQ(atomic_load(&tr_start), 4);          /* 2 writes, FLUSH, IDENTIFY */
    CHECK_EQ(atomic_load(&tr_done), 4);
    CHECK_EQ(atomic_load(&tr_host_writes), 1);
    CHECK_EQ(atomic_load(&tr_destages), 1);
    CHECK_EQ(atomic_load(&tr_bad_chs), 0);
    CHECK(fw_sim_now_ms(fw) == 0.0);              /* time_scale 0 */
    fw_destroy(fw);
}

int main(void)
{
    tmp_media(path, sizeof(path), "dispatch");
    printf("test_dispatch (queue=%s)\n", fw_queue_impl());
    RUN_TEST(test_identify);
    RUN_TEST(test_invalid_opcode_and_sense);
    RUN_TEST(test_range_validation);
    RUN_TEST(test_tags);
    RUN_TEST(test_full_queue_depth);
    RUN_TEST(test_write_back_then_flush);
    RUN_TEST(test_fua_and_write_through);
    RUN_TEST(test_set_features);
    RUN_TEST(test_threshold_and_forced_destage);
    RUN_TEST(test_unsorted_destage_is_also_correct);
    RUN_TEST(test_read_ahead);
    RUN_TEST(test_clean_shutdown_persists);
    RUN_TEST(test_trace_events);
    unlink(path);
    return test_summary("test_dispatch");
}
