/*
 * test_policy.c - the write-protection policies (env_monitor.c) on the
 * frozen physical model:
 *   - with the feature off, the firmware's output is bit-identical to the
 *     pre-feature firmware (fingerprint computed from commit acac711);
 *   - a write issued at the onset of a real shock, under the threshold
 *     policy (at the 250 mg default), is held until the shock has passed and reads back correctly
 *     with zero CRC errors (while the same writes unprotected land
 *     off-track);
 *   - the AI policy switches on verify-after-write under vibration, the
 *     hybrid releases tap-triggered inhibits early, trace events carry
 *     class and confidence, and the threaded-interrupt mode works.
 */
#include <math.h>

#include "check.h"
#include "env_monitor.h"
#include "fingerprint.h"
#include "media.h"
#include "periph/env_sim.h"

static char path[128];

/* fingerprint_run() of tests/fingerprint.h compiled against the sources of
 * commit acac711 (before any Edge AI change), both queue builds. */
#define BASELINE_FINGERPRINT 0xCA075D0Bu

static void tweak_policy_without_env(fw_config_t *c)
{
    c->vib_policy = FW_VIB_HYBRID;      /* a policy, but no environment */
    c->vib_threshold_mg = 123;
}

static void test_feature_off_matches_baseline(void)
{
    uint32_t a = fingerprint_run(path, NULL);
    uint32_t b = fingerprint_run(path, tweak_policy_without_env);
    printf("    fingerprint 0x%08X (pre-feature firmware: 0x%08X)\n", a, BASELINE_FINGERPRINT);
    CHECK_EQ(a, BASELINE_FINGERPRINT);
    CHECK_EQ(b, BASELINE_FINGERPRINT);
}

/* ---- a real shock, at the media layer -------------------------------------------- */

typedef struct {
    media_t        m;
    fw_counters_t  ctr;
    atomic_int     power;
    env_monitor_t *v;
} rig_t;

static int rig_open(rig_t *r, int mix, uint64_t seed, int policy, uint32_t thr)
{
    memset(r, 0, sizeof(*r));
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.vib_env = mix;
    cfg.vib_seed = seed;
    cfg.vib_policy = policy;
    cfg.vib_threshold_mg = thr;
    atomic_init(&r->power, 1);
    if (media_open(&r->m, &cfg, &r->power, &r->ctr) != 0)
        return -1;
    if (policy != FW_VIB_NONE) {
        r->v = envmon_create(&r->m, &cfg, &r->ctr);
        if (!r->v)
            return -1;
        r->m.wgate = envmon_gate;
        r->m.wgate_ctx = r->v;
    }
    return 0;
}

static void rig_close(rig_t *r)
{
    envmon_destroy(r->v);
    media_close(&r->m);
}

/* Onset (ms) of the first shock that displaces the head by > 40% of a
 * track: the first SHOCK-labelled sample of that burst. */
static double find_big_shock(uint64_t seed)
{
    env_sim_t e;
    if (env_init(&e, ENV_MIX_SHOCK, seed) != 0)
        return -1;
    double onset = -1;
    uint64_t start = 0;
    int in = 0;
    for (uint64_t t = 0; t < 60000000u && onset < 0; t += 250) {
        int s = env_label(&e, t) == ENV_SHOCK;
        if (s && !in)
            start = t;
        in = s;
        if (s && fabs(env_disp_pct(&e, t)) > 40.0)
            onset = (double)start / 1000.0;
    }
    env_free(&e);
    return onset;
}

/* 20 writes issued back to back from the instant the shock begins. */
static void burst(rig_t *r, double t0_ms, unsigned *bad_readback, uint64_t *crc_errors)
{
    r->m.sim_ms = t0_ms;              /* the head was idle until the shock hits */
    uint8_t buf[8 * 512], out[512];
    uint64_t rng = 77;
    uint64_t lba[20];
    for (int i = 0; i < 20; i++) {
        lba[i] = rng_below64(&rng, (1u << 20) - 8);
        for (int k = 0; k < 8; k++)
            pattern_fill(buf + k * 512, 512, lba[i] + k, 5);
        CHECK_EQ(media_write(&r->m, lba[i], 8, buf), FW_OK);
    }
    r->m.read_retries = 0;
    *bad_readback = 0;
    for (int i = 0; i < 20; i++)
        for (int k = 0; k < 8; k++) {
            uint64_t bl = 0;
            uint32_t ver = 0;
            fw_status_t st = media_read(&r->m, lba[i] + k, 1, out, &bl);
            if (st != FW_OK || pattern_check(out, 512, lba[i] + k, &ver) != PAT_OK || ver != 5)
                (*bad_readback)++;
        }
    *crc_errors = ctr_get(&r->ctr.crc_errors);
}

static void test_protected_shock_write_reads_back(void)
{
    const uint64_t seed = 3;
    double t0 = find_big_shock(seed);
    CHECK(t0 > 0);
    if (t0 <= 0)
        return;
    rig_t a, b;
    unsigned bad_p0 = 0, bad_p1 = 0;
    uint64_t crc_p0 = 0, crc_p1 = 0;
    CHECK_EQ(rig_open(&a, ENV_MIX_SHOCK, seed, FW_VIB_NONE, 0), 0);
    burst(&a, t0, &bad_p0, &crc_p0);
    uint64_t ot_p0 = ctr_get(&a.ctr.vib_offtrack_sectors);
    rig_close(&a);
    CHECK_EQ(rig_open(&b, ENV_MIX_SHOCK, seed, FW_VIB_THRESHOLD, 250), 0);
    burst(&b, t0, &bad_p1, &crc_p1);
    uint64_t ot_p1 = ctr_get(&b.ctr.vib_offtrack_sectors);
    uint64_t stall = ctr_get(&b.ctr.vib_stall_us), irqs = ctr_get(&b.ctr.vib_shock_irqs);
    rig_close(&b);
    printf("    shock at %.1f ms, 20 writes: unprotected %llu sectors off-track (%u bad on "
           "read-back); threshold policy stalled %.1f ms on %llu IRQs, %llu off-track, %u bad, "
           "%llu CRC errors\n", t0, (unsigned long long)ot_p0, bad_p0, stall / 1000.0,
           (unsigned long long)irqs, (unsigned long long)ot_p1, bad_p1,
           (unsigned long long)crc_p1);
    CHECK(ot_p0 > 0);                 /* the scenario really is harmful   */
    CHECK_EQ(bad_p0, ot_p0);          /* ... and every hit is detected     */
    CHECK(stall > 0);
    CHECK(irqs > 0);
    CHECK_EQ(ot_p1, 0);               /* protected: nothing off-track     */
    CHECK_EQ(bad_p1, 0);              /* every sector reads back          */
    CHECK_EQ(crc_p1, 0);              /* with zero CRC errors              */
}

/* ---- policies through the firmware API ------------------------------------------------ */

static fw_stats_t run_policy(int mix, int policy, uint64_t seed, int nwrites, uint32_t thr,
                             int threaded, fw_trace_fn tr, void *tctx)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.write_cache = 0;
    cfg.read_ahead_sectors = 0;
    cfg.vib_env = mix;
    cfg.vib_seed = seed;
    cfg.vib_policy = policy;
    cfg.vib_threshold_mg = thr;
    cfg.vib_irq_threaded = threaded;
    cfg.trace = tr;
    cfg.trace_ctx = tctx;
    fw_stats_t st;
    memset(&st, 0, sizeof(st));
    fw_t *fw = fw_create(&cfg, NULL);
    CHECK(fw != NULL);
    if (!fw)
        return st;
    uint8_t buf[8 * 512];
    memset(buf, 0xA5, sizeof(buf));
    uint64_t rng = seed;
    for (int i = 0; i < nwrites; i++) {
        fw_cmd_t c = mkcmd(FW_OP_WRITE, 0, rng_below64(&rng, (1u << 20) - 8), 8, buf);
        CHECK_EQ(fw_exec(fw, &c), FW_OK);
    }
    fw_shutdown(fw);
    fw_get_stats(fw, &st);
    fw_destroy(fw);
    return st;
}

static void test_ai_verifies_under_vibration(void)
{
    fw_stats_t ai = run_policy(ENV_MIX_VIBRATION, FW_VIB_AI, 4, 1000, 0, 0, NULL, NULL);
    fw_stats_t th = run_policy(ENV_MIX_VIBRATION, FW_VIB_THRESHOLD, 4, 1000, 0, 0, NULL, NULL);
    printf("    vibration-heavy, 1000 writes: AI %llu windows (%llu vibration), %llu verified "
           "writes, %llu rewrites; threshold %llu IRQs, %llu verified\n",
           (unsigned long long)ai.vib_windows, (unsigned long long)ai.vib_class_windows[2],
           (unsigned long long)ai.vib_verified_writes, (unsigned long long)ai.vib_verify_rewrites,
           (unsigned long long)th.vib_shock_irqs, (unsigned long long)th.vib_verified_writes);
    CHECK(ai.vib_windows > 0);
    CHECK(ai.vib_class_windows[2] > 0);
    CHECK(ai.vib_verified_writes > 0);
    CHECK_EQ(ai.vib_shock_irqs, 0);       /* P2 does not use the threshold */
    CHECK_EQ(th.vib_verified_writes, 0);  /* P1 never verifies             */
    CHECK_EQ(th.vib_windows, 0);          /* ... and runs no classifier    */
}

static void test_hybrid_releases_tap_inhibits(void)
{
    /* Taps trip a 250 mg threshold; the classifier can release those. */
    fw_stats_t p1 = run_policy(ENV_MIX_MIXED, FW_VIB_THRESHOLD, 8, 1500, 250, 0, NULL, NULL);
    fw_stats_t p3 = run_policy(ENV_MIX_MIXED, FW_VIB_HYBRID, 8, 1500, 250, 0, NULL, NULL);
    printf("    mixed environment, 250 mg threshold: P1 %llu IRQs, %.1f ms needless stall; "
           "P3 %llu early releases, %.1f ms needless stall\n",
           (unsigned long long)p1.vib_shock_irqs, p1.vib_false_stall_ms,
           (unsigned long long)p3.vib_early_releases, p3.vib_false_stall_ms);
    CHECK(p1.vib_shock_irqs > 0);
    CHECK(p3.vib_early_releases > 0);
    /* Whether that saves stall time overall is a benchmark question (P3
     * also verifies under vibration, so its writes land at other moments):
     * see `make bench-edgeai`, 30 seeds per mix. */
}

typedef struct {
    atomic_int n, bad, irq;
} vtrace_t;

static void on_trace(void *ctx, const fw_trace_t *ev)
{
    vtrace_t *t = ctx;
    if (ev->kind != FW_TRACE_VIBRATION)
        return;
    if (ev->vib_class == 255) {
        atomic_fetch_add(&t->irq, 1);
        return;
    }
    atomic_fetch_add(&t->n, 1);
    if (ev->vib_class > 3 || ev->vib_conf < 25 || ev->vib_conf > 100)
        atomic_fetch_add(&t->bad, 1);
}

static void test_trace_events(void)
{
    vtrace_t t;
    atomic_init(&t.n, 0);
    atomic_init(&t.bad, 0);
    atomic_init(&t.irq, 0);
    fw_stats_t st = run_policy(ENV_MIX_MIXED, FW_VIB_HYBRID, 2, 400, 0, 0, on_trace, &t);
    CHECK(atomic_load(&t.n) > 0);
    CHECK_EQ(atomic_load(&t.n), st.vib_windows);       /* one per window */
    CHECK_EQ(atomic_load(&t.bad), 0);
    CHECK_EQ(atomic_load(&t.irq), st.vib_shock_irqs);
}

static void test_threaded_interrupts(void)
{
    /* ISRs on their own thread, concurrent with the firmware's bottom half.
     * Results are timing-dependent; under TSan this is the race check. */
    fw_stats_t st = run_policy(ENV_MIX_MIXED, FW_VIB_HYBRID, 6, 400, 0, 1, NULL, NULL);
    CHECK(st.vib_isr_runs > 0);
    CHECK(st.vib_windows > 0);
}

int main(void)
{
    tmp_media(path, sizeof(path), "policy");
    printf("test_policy (queue=%s)\n", fw_queue_impl());
    RUN_TEST(test_feature_off_matches_baseline);
    RUN_TEST(test_protected_shock_write_reads_back);
    RUN_TEST(test_ai_verifies_under_vibration);
    RUN_TEST(test_hybrid_releases_tap_inhibits);
    RUN_TEST(test_trace_events);
    RUN_TEST(test_threaded_interrupts);
    unlink(path);
    return test_summary("test_policy");
}
