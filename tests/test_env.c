/*
 * test_env.c - the physical model (Part B), tested on its own before any
 * classifier exists: the synthetic environment generator, the off-track
 * probability, the write-path mechanisms a policy can use (inhibit/stall,
 * write gate, extra settle, verify-after-write), and the end-to-end
 * property that every off-track sector is DETECTED on read-back.
 */
#include <math.h>

#include "check.h"
#include "media.h"
#include "pattern.h"
#include "periph/env_sim.h"

static char path[128];

/* ---- generator ------------------------------------------------------------------- */

static void test_env_deterministic(void)
{
    env_sim_t a, b, c;
    CHECK_EQ(env_init(&a, ENV_MIX_MIXED, 42), 0);
    CHECK_EQ(env_init(&b, ENV_MIX_MIXED, 42), 0);
    CHECK_EQ(env_init(&c, ENV_MIX_MIXED, 43), 0);
    int same = 1, differ = 0;
    for (uint64_t t = 0; t < 20000000; t += 250) {
        double x = env_accel_mg(&a, t), y = env_accel_mg(&b, t), z = env_accel_mg(&c, t);
        same &= x == y && env_label(&a, t) == env_label(&b, t);
        differ |= x != z;
    }
    CHECK(same);
    CHECK(differ);
    env_free(&a);
    env_free(&b);
    env_free(&c);
    CHECK(env_init(&c, ENV_MIX_OFF, 1) != 0);     /* "off" has no model */
}

/* Fraction of 0.25 ms samples with each label over `secs` seconds. */
static void label_fractions(env_mix_t mix, uint64_t seed, int secs, double frac[ENV_NCLASSES])
{
    env_sim_t e;
    CHECK_EQ(env_init(&e, mix, seed), 0);
    uint64_t cnt[ENV_NCLASSES] = { 0 }, n = 0;
    for (uint64_t t = 0; t < (uint64_t)secs * 1000000u; t += 250, n++)
        cnt[env_label(&e, t)]++;
    for (int k = 0; k < ENV_NCLASSES; k++)
        frac[k] = (double)cnt[k] / (double)n;
    env_free(&e);
}

static void test_env_mixes(void)
{
    double f[ENV_NCLASSES];
    label_fractions(ENV_MIX_QUIET, 1, 120, f);
    CHECK(f[ENV_QUIET] > 0.8);
    CHECK(f[ENV_SHOCK] == 0.0);
    label_fractions(ENV_MIX_VIBRATION, 1, 120, f);
    CHECK(f[ENV_VIBRATION] > 0.5);
    label_fractions(ENV_MIX_SHOCK, 1, 120, f);
    CHECK(f[ENV_SHOCK] > 0.02);                   /* shocks are short events */
    label_fractions(ENV_MIX_MIXED, 1, 120, f);
    printf("    mixed: quiet %.3f shock %.3f vibration %.3f tap %.3f\n", f[0], f[1], f[2], f[3]);
    for (int k = 0; k < ENV_NCLASSES; k++)
        CHECK(f[k] > 0.002);     /* every class occurs (taps ring for ms only) */
}

/* The ordering of harm must come out of the physics: shocks displace the
 * head most, sustained vibration next, taps and quiet hardly at all. */
static void test_env_harm_ordering(void)
{
    env_sim_t e;
    CHECK_EQ(env_init(&e, ENV_MIX_MIXED, 7), 0);
    double psum[ENV_NCLASSES] = { 0 }, dmax[ENV_NCLASSES] = { 0 };
    uint64_t n[ENV_NCLASSES] = { 0 };
    for (uint64_t t = 0; t < 300000000u; t += 250) {
        int c = env_label(&e, t);
        double d = fabs(env_disp_pct(&e, t));
        psum[c] += env_offtrack_prob(d);
        if (d > dmax[c])
            dmax[c] = d;
        n[c]++;
    }
    double p[ENV_NCLASSES];
    for (int k = 0; k < ENV_NCLASSES; k++)
        p[k] = n[k] ? psum[k] / (double)n[k] : 0;
    printf("    mean p(off-track): quiet %.2e shock %.2e vibration %.2e tap %.2e\n",
           p[ENV_QUIET], p[ENV_SHOCK], p[ENV_VIBRATION], p[ENV_TAP]);
    CHECK(p[ENV_SHOCK] > p[ENV_VIBRATION]);
    CHECK(p[ENV_VIBRATION] > p[ENV_TAP]);
    CHECK(p[ENV_VIBRATION] > 10 * p[ENV_QUIET]);
    CHECK(p[ENV_QUIET] < 1e-4);
    CHECK(dmax[ENV_SHOCK] > 30.0);
    env_free(&e);
}

static void test_offtrack_curve(void)
{
    CHECK(env_offtrack_prob(0.0) < 1e-5);
    CHECK(fabs(env_offtrack_prob(ENV_WFT_PCT) - 0.5) < 0.01);
    CHECK(env_offtrack_prob(30.0) > 0.999);
    CHECK(env_offtrack_prob(-10.0) == env_offtrack_prob(10.0));
    int mono = 1;
    for (double d = 0; d < 40; d += 0.5)
        mono &= env_offtrack_prob(d + 0.5) >= env_offtrack_prob(d);
    CHECK(mono);
    CHECK(fabs(env_settle_residual_pct(0) - ENV_SETTLE_R0_PCT) < 1e-12);
    CHECK(env_settle_residual_pct(5 * ENV_SETTLE_TAU_MS) < 0.01 * ENV_SETTLE_R0_PCT);
}

static void test_env_history(void)
{
    env_sim_t a, b;
    CHECK_EQ(env_init(&a, ENV_MIX_SHOCK, 5), 0);
    CHECK_EQ(env_init(&b, ENV_MIX_SHOCK, 5), 0);
    (void)env_accel_mg(&a, 3000000);              /* jump ahead 3 s ...       */
    int same = 1;
    for (uint64_t t = 1000000; t < 3000000; t += 250)   /* ... and look back */
        same &= env_accel_mg(&a, t) == env_accel_mg(&b, t);
    CHECK(same);
    CHECK_EQ(a.clamped, 0);
    (void)env_accel_mg(&a, 20000000);
    (void)env_accel_mg(&a, 0);                    /* far older than history */
    CHECK_EQ(a.clamped, 1);
    env_free(&a);
    env_free(&b);
}

/* ---- the write path, at the media layer ---------------------------------------------- */

typedef struct {
    media_t       m;
    fw_counters_t ctr;
    atomic_int    power;
} rig_t;

static int rig_open(rig_t *r, int mix, uint64_t seed)
{
    memset(r, 0, sizeof(*r));
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.vib_env = mix;
    cfg.vib_seed = seed;
    atomic_init(&r->power, 1);
    return media_open(&r->m, &cfg, &r->power, &r->ctr);
}

typedef struct {
    int    mode, calls;
    double value;
} gate_t;

enum { G_INHIBIT_UNTIL, G_ABORT_ONCE, G_SETTLE, G_VERIFY, G_FOREVER, G_CREEP };

static void fake_gate(void *ctx, double t_ms, media_wpolicy_t *pol)
{
    gate_t *g = ctx;
    g->calls++;
    switch (g->mode) {
    case G_INHIBIT_UNTIL: pol->inhibit_until_ms = g->value; break;
    case G_ABORT_ONCE:    if (g->calls == 2) pol->inhibit_until_ms = t_ms + 1.0; break;
    case G_SETTLE:        pol->settle_ms = g->value; break;
    case G_VERIFY:        pol->verify = (uint32_t)g->value; break;
    case G_FOREVER:       pol->inhibit_until_ms = t_ms + 1000.0; break;
    case G_CREEP:         pol->inhibit_until_ms = t_ms + 0.3; break;   /* never quite ends */
    }
}

static void test_gate_inhibit_stalls_then_writes(void)
{
    rig_t r;
    CHECK_EQ(rig_open(&r, ENV_MIX_QUIET, 3), 0);
    gate_t g = { G_INHIBIT_UNTIL, 0, 100.0 };
    r.m.wgate = fake_gate;
    r.m.wgate_ctx = &g;
    uint8_t buf[8 * 512];
    memset(buf, 0x5A, sizeof(buf));
    CHECK_EQ(media_write(&r.m, 1000, 8, buf), FW_OK);
    CHECK(r.m.sim_ms > 100.0);                           /* waited, then wrote */
    CHECK_EQ(ctr_get(&r.ctr.vib_stall_us), 100000);
    /* Quiet environment: (almost) all of that stall was unnecessary. */
    CHECK(ctr_get(&r.ctr.vib_false_stall_us) > 90000);
    CHECK_EQ(ctr_get(&r.ctr.media_writes), 1);
    media_close(&r.m);
}

static void test_gate_abort_at_transfer_retries(void)
{
    rig_t r;
    CHECK_EQ(rig_open(&r, ENV_MIX_QUIET, 3), 0);
    gate_t g = { G_ABORT_ONCE, 0, 0 };
    r.m.wgate = fake_gate;
    r.m.wgate_ctx = &g;
    uint8_t buf[8 * 512];
    memset(buf, 0x11, sizeof(buf));
    CHECK_EQ(media_write(&r.m, 5000, 8, buf), FW_OK);
    CHECK_EQ(ctr_get(&r.ctr.vib_write_aborts), 1);
    CHECK_EQ(ctr_get(&r.ctr.media_writes), 1);
    media_close(&r.m);
}

static void test_gate_stall_is_bounded(void)
{
    rig_t r;
    CHECK_EQ(rig_open(&r, ENV_MIX_QUIET, 3), 0);
    gate_t g = { G_FOREVER, 0, 0 };
    r.m.wgate = fake_gate;
    r.m.wgate_ctx = &g;
    uint8_t buf[512] = { 1 };
    CHECK_EQ(media_write(&r.m, 7, 1, buf), FW_OK);
    CHECK_EQ(ctr_get(&r.ctr.vib_stall_timeouts), 1);
    CHECK_EQ(ctr_get(&r.ctr.vib_stall_us), (uint64_t)(MEDIA_MAX_STALL_MS * 1000));

    /* Regression: thousands of short stalls (an inhibit re-extended by
     * every sample, as a low threshold in strong vibration does) late in a
     * long run. The limit was once a floating-point running sum that could
     * stop just short of 500 ms by less than the clock's resolution, and
     * the write then stalled for zero time forever. */
    gate_t creep = { G_CREEP, 0, 0 };
    r.m.wgate_ctx = &creep;
    r.m.sim_ms = 123456.789;
    CHECK_EQ(media_write(&r.m, 9, 1, buf), FW_OK);
    CHECK_EQ(ctr_get(&r.ctr.vib_stall_timeouts), 2);
    CHECK(creep.calls > 1000);
    media_close(&r.m);
}

static void test_gate_extra_settle_costs_a_revolution(void)
{
    rig_t r;
    CHECK_EQ(rig_open(&r, ENV_MIX_QUIET, 3), 0);
    gate_t g = { G_SETTLE, 0, 1e9 };                     /* never enough */
    r.m.wgate = fake_gate;
    r.m.wgate_ctx = &g;
    uint8_t buf[8 * 512] = { 0 };
    buf[0] = 1;
    uint64_t rng = 9;
    for (int i = 0; i < 20; i++)
        CHECK_EQ(media_write(&r.m, rng_below64(&rng, (1u << 20) - 8), 8, buf), FW_OK);
    CHECK(ctr_get(&r.ctr.vib_extra_settles) >= 18);    /* (almost) every write seeks */
    media_close(&r.m);
}

/* Count corrupted sectors in [lba, lba+n) straight from the records. */
static unsigned corrupt_sectors(rig_t *r, uint64_t lba, uint32_t n)
{
    unsigned bad = 0;
    uint64_t bl = 0;
    uint8_t out[512];
    for (uint32_t k = 0; k < n; k++)
        if (media_read(&r->m, lba + k, 1, out, &bl) == FW_ERR_MEDIUM)
            bad++;
    return bad;
}

static void test_verify_after_write_repairs(void)
{
    /* Shock-heavy environment, no protection vs verify-after-write, same
     * write positions. Verify must find and rewrite bad sectors, and leave
     * fewer of them behind. */
    unsigned left[2] = { 0, 0 };
    uint64_t rewrites = 0, offtrack_plain = 0;
    for (int v = 0; v <= 1; v++) {
        rig_t r;
        CHECK_EQ(rig_open(&r, ENV_MIX_SHOCK, 11), 0);
        r.m.read_retries = 0;
        gate_t g = { G_VERIFY, 0, 3 };
        if (v) {
            r.m.wgate = fake_gate;
            r.m.wgate_ctx = &g;
        }
        uint8_t buf[8 * 512];
        for (int i = 0; i < 2000; i++) {
            for (int k = 0; k < 8; k++)
                pattern_fill(buf + k * 512, 512, (uint64_t)i * 64 + k, 1);
            CHECK_EQ(media_write(&r.m, (uint64_t)i * 64, 8, buf), FW_OK);
        }
        for (int i = 0; i < 2000; i++)
            left[v] += corrupt_sectors(&r, (uint64_t)i * 64, 8);
        if (v)
            rewrites = ctr_get(&r.ctr.vib_verify_rewrites);
        else
            offtrack_plain = ctr_get(&r.ctr.vib_offtrack_sectors);
        if (v)
            CHECK_EQ(ctr_get(&r.ctr.vib_verified_writes), 2000);
        media_close(&r.m);
    }
    printf("    shock mix, 2000 writes: %u bad sectors unprotected (%llu off-track), "
           "%u with verify (%llu rewrites)\n", left[0], (unsigned long long)offtrack_plain,
           left[1], (unsigned long long)rewrites);
    CHECK_EQ(left[0], offtrack_plain);       /* every off-track sector detected */
    CHECK(left[0] > 0);
    CHECK(rewrites > 0);
    CHECK(left[1] < left[0]);
}

/* ---- end to end through the firmware API --------------------------------------------- */

static void test_offtrack_detected_never_silent(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.vib_env = ENV_MIX_SHOCK;
    cfg.vib_seed = 21;
    cfg.write_cache = 0;                     /* every write goes to media */
    fw_t *fw = fw_create(&cfg, NULL);
    CHECK(fw != NULL);
    const int nblk = 3000;
    uint8_t buf[8 * 512];
    /* Scattered, distinct block addresses (the multiplier is coprime with
     * 8000), so every write seeks and each LBA is written exactly once. */
#define BLK_LBA(i) ((uint64_t)(((uint64_t)(i) * 2654435761u) % 8000u) * 128u)
    for (int i = 0; i < nblk; i++) {
        for (int k = 0; k < 8; k++)
            pattern_fill(buf + k * 512, 512, BLK_LBA(i) + k, 1);
        fw_cmd_t c = mkcmd(FW_OP_WRITE, 0, BLK_LBA(i), 8, buf);
        CHECK_EQ(fw_exec(fw, &c), FW_OK);
    }
    fw_shutdown(fw);
    fw_stats_t st;
    fw_get_stats(fw, &st);
    fw_destroy(fw);

    /* Reboot without the environment and read every sector back. */
    cfg.vib_env = 0;
    cfg.media_create = 0;
    cfg.read_ahead_sectors = 0;
    fw = fw_create(&cfg, NULL);
    uint64_t medium = 0, silent = 0, good = 0;
    for (int i = 0; i < nblk; i++)
        for (int k = 0; k < 8; k++) {
            uint64_t lba = BLK_LBA(i) + k;
            fw_cmd_t c = mkcmd(FW_OP_READ, 0, lba, 1, buf);
            fw_status_t s = fw_exec(fw, &c);
            uint32_t ver = 0;
            if (s == FW_ERR_MEDIUM && c.err_lba == lba)
                medium++;
            else if (s == FW_OK && pattern_check(buf, 512, lba, &ver) == PAT_OK && ver == 1)
                good++;
            else
                silent++;
        }
    fw_destroy(fw);
    printf("    %d writes in a shock-heavy mix: %llu sectors off-track, %llu MEDIUM ERROR "
           "on read-back, %llu silent\n", nblk, (unsigned long long)st.vib_offtrack_sectors,
           (unsigned long long)medium, (unsigned long long)silent);
    CHECK(st.vib_offtrack_sectors > 0);
    CHECK_EQ(medium, st.vib_offtrack_sectors);
    CHECK_EQ(silent, 0);
    CHECK_EQ(good + medium, (uint64_t)nblk * 8);
}

static void test_env_off_changes_nothing(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = fw_create(&cfg, NULL);
    uint8_t buf[8 * 512] = { 3 };
    for (int i = 0; i < 200; i++) {
        fw_cmd_t c = mkcmd(FW_OP_WRITE, 0, (uint64_t)i * 977, 8, buf);
        c.flags = FW_CMD_FUA;
        CHECK_EQ(fw_exec(fw, &c), FW_OK);
    }
    fw_stats_t st;
    fw_get_stats(fw, &st);
    fw_destroy(fw);
    CHECK_EQ(st.vib_offtrack_sectors + st.vib_write_aborts + st.vib_verified_writes +
             st.vib_extra_settles + st.vib_stall_timeouts, 0);
    CHECK(st.vib_stall_ms == 0.0);
}

int main(void)
{
    tmp_media(path, sizeof(path), "env");
    printf("test_env (queue=%s)\n", fw_queue_impl());
    RUN_TEST(test_env_deterministic);
    RUN_TEST(test_env_mixes);
    RUN_TEST(test_env_harm_ordering);
    RUN_TEST(test_offtrack_curve);
    RUN_TEST(test_env_history);
    RUN_TEST(test_gate_inhibit_stalls_then_writes);
    RUN_TEST(test_gate_abort_at_transfer_retries);
    RUN_TEST(test_gate_stall_is_bounded);
    RUN_TEST(test_gate_extra_settle_costs_a_revolution);
    RUN_TEST(test_verify_after_write_repairs);
    RUN_TEST(test_offtrack_detected_never_silent);
    RUN_TEST(test_env_off_changes_nothing);
    unlink(path);
    return test_summary("test_env");
}
