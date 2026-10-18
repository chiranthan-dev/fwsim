/*
 * check.h - a deliberately tiny test harness (no external framework).
 *
 * CHECK() records a failure and keeps going, so one run reports every
 * broken assertion instead of stopping at the first. Each test binary
 * returns non-zero if anything failed, which is all `make test` needs.
 */
#ifndef FWSIM_CHECK_H
#define FWSIM_CHECK_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fwsim.h"

static int g_checks, g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        uint64_t va_ = (uint64_t)(a), vb_ = (uint64_t)(b);                 \
        g_checks++;                                                        \
        if (va_ != vb_) {                                                  \
            g_failures++;                                                  \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%" PRIu64 " vs %" PRIu64 ")\n", \
                    __FILE__, __LINE__, #a, #b, va_, vb_);                 \
        }                                                                  \
    } while (0)

#define RUN_TEST(fn)                                                       \
    do {                                                                   \
        int before_ = g_failures;                                          \
        fn();                                                              \
        printf("  %-44s %s\n", #fn, g_failures == before_ ? "ok" : "FAILED"); \
    } while (0)

static inline int test_summary(const char *suite)
{
    printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
    return g_failures ? 1 : 0;
}

/* A unique backing-file path in /tmp for this process. */
static inline const char *tmp_media(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "/tmp/fwsim-test-%d-%s.media", (int)getpid(), tag);
    return buf;
}

/* A drive config for tests: media takes no time, no idle destaging (so
 * tests are deterministic), small cache. */
static inline void test_config(fw_config_t *cfg, const char *path)
{
    fw_config_default(cfg);
    cfg->media_path = path;
    cfg->media_create = 1;
    cfg->time_scale = 0.0;
    cfg->cache_entries = 256;
    cfg->idle_destage_ms = 0.0;
    cfg->capacity_lbas = 1u << 20;   /* 512 MiB is plenty for tests */
}

/* Build a command in one line. */
static inline fw_cmd_t mkcmd(uint8_t op, uint8_t tag, uint64_t lba, uint32_t count,
                             void *buf)
{
    fw_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = op;
    c.tag = tag;
    c.lba = lba;
    c.count = count;
    c.buf = buf;
    return c;
}

#endif
