/*
 * fingerprint.h - a deterministic workload whose result is condensed into
 * one 32-bit number: the CRC of the whole media file plus every counter
 * that a behaviour change would move. Uses only the public fwsim API that
 * existed BEFORE the Edge AI feature, so the identical function compiled
 * against the pre-feature sources (commit acac711) gives the expected
 * value; tests/test_policy.c checks that, with the feature off, the
 * current firmware still produces it bit for bit.
 *
 * Deterministic by construction: time_scale 0 (media time is simulated
 * only), write-through + FUA (no background destage, whose interleaving
 * with host commands depends on host timing), QD 1, fixed seed.
 */
#ifndef FWSIM_TESTS_FINGERPRINT_H
#define FWSIM_TESTS_FINGERPRINT_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "crc32.h"
#include "fwsim.h"
#include "pattern.h"
#include "util.h"

static uint32_t fp_mix(uint32_t c, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    return crc32_update(c, b, 8);
}

/* `tweak` lets a caller change the config before the drive is created
 * (e.g. set Edge AI fields); NULL for none. Returns the fingerprint. */
static uint32_t fingerprint_run(const char *path, void (*tweak)(fw_config_t *))
{
    fw_config_t cfg;
    fw_config_default(&cfg);
    cfg.media_path = path;
    cfg.media_create = 1;
    cfg.time_scale = 0.0;
    cfg.cache_entries = 256;
    cfg.idle_destage_ms = 0.0;
    cfg.capacity_lbas = 1u << 18;
    cfg.write_cache = 0;
    cfg.seed = 99;
    if (tweak)
        tweak(&cfg);
    fw_t *fw = fw_create(&cfg, NULL);
    if (!fw)
        return 0;
    uint64_t rng = 12345;
    static uint8_t buf[64 * 512];
    for (int i = 0; i < 600; i++) {
        uint64_t lba = rng_below64(&rng, (1u << 18) - 64);
        uint32_t n = 1 + (uint32_t)rng_below64(&rng, 64);
        int write = rng_below64(&rng, 3) != 0;
        fw_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = write ? FW_OP_WRITE : FW_OP_READ;
        c.lba = lba;
        c.count = n;
        c.buf = buf;
        c.flags = write && (i & 1) ? FW_CMD_FUA : 0;
        if (write)
            for (uint32_t k = 0; k < n; k++)
                pattern_fill(buf + k * 512, 512, lba + k, (uint32_t)i);
        (void)fw_exec(fw, &c);
    }
    /* A sequential stream to exercise read-ahead, then FLUSH. */
    for (int i = 0; i < 40; i++) {
        fw_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = FW_OP_READ;
        c.lba = 200000 + (uint64_t)i * 32;
        c.count = 32;
        c.buf = buf;
        (void)fw_exec(fw, &c);
    }
    fw_cmd_t fl;
    memset(&fl, 0, sizeof(fl));
    fl.opcode = FW_OP_FLUSH;
    (void)fw_exec(fw, &fl);
    fw_shutdown(fw);
    fw_stats_t st;
    fw_get_stats(fw, &st);
    fw_destroy(fw);

    uint32_t c = CRC32_INIT;
    uint64_t v[] = { st.media_reads, st.media_writes, st.media_sectors_read,
                     st.media_sectors_written, (uint64_t)(st.media_busy_ms * 1000.0 + 0.5),
                     st.read_hit_sectors, st.read_miss_sectors, st.write_hit_sectors,
                     st.readahead_ops, st.readahead_sectors, st.crc_errors, st.cmd_errors };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        c = fp_mix(c, v[i]);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    static uint8_t blk[1 << 16];
    ssize_t r;
    while ((r = read(fd, blk, sizeof(blk))) > 0)
        c = crc32_update(c, blk, (size_t)r);
    close(fd);
    return crc32_final(c);
}

#endif
