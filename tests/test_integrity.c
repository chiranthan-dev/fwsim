/*
 * test_integrity.c - per-sector CRC on the media: persistent corruption ->
 * retries -> MEDIUM_ERROR with the right LBA; transient read errors ->
 * recovered by retry; misdirected writes caught by the LBA-seeded CRC;
 * reallocation of a bad sector on rewrite; never any silent corruption.
 */
#include <fcntl.h>

#include "check.h"
#include "pattern.h"

static char path[128];

/* Write `n` sectors with pattern version `ver`, then power-cycle cleanly so
 * the next reads come from the media, not the cache. */
static fw_t *write_and_reopen(fw_config_t *cfg, uint64_t lba, uint32_t n, uint32_t ver)
{
    fw_t *fw = fw_create(cfg, NULL);
    uint8_t *buf = malloc((size_t)n * 512);
    for (uint32_t k = 0; k < n; k++)
        pattern_fill(buf + (size_t)k * 512, 512, lba + k, ver);
    fw_cmd_t c = mkcmd(FW_OP_WRITE, 0, lba, n, buf);
    CHECK_EQ(fw_exec(fw, &c), FW_OK);
    free(buf);
    fw_shutdown(fw);
    fw_destroy(fw);
    fw_config_t again = *cfg;
    again.media_create = 0;
    fw = fw_create(&again, NULL);
    CHECK(fw != NULL);
    return fw;
}

static fw_status_t read1(fw_t *fw, uint64_t lba, uint32_t n, uint8_t *buf, uint64_t *err_lba)
{
    fw_cmd_t c = mkcmd(FW_OP_READ, 0, lba, n, buf);
    fw_status_t st = fw_exec(fw, &c);
    if (err_lba)
        *err_lba = c.err_lba;
    return st;
}

static void test_persistent_flip_gives_medium_error(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.read_retries = 3;
    fw_t *fw = write_and_reopen(&cfg, 1000, 16, 1);
    CHECK_EQ(fw_inject_media_bitflip(fw, 1005, 1234), 0);   /* a data bit */

    uint8_t buf[16 * 512];
    uint64_t err_lba = 0;
    fw_cmd_t c = mkcmd(FW_OP_READ, 0, 1000, 16, buf);
    CHECK_EQ(fw_exec(fw, &c), FW_ERR_MEDIUM);
    err_lba = c.err_lba;
    CHECK_EQ(err_lba, 1005);                   /* exact failing LBA reported */
    CHECK_EQ(c.sense.key, 0x3);                /* MEDIUM ERROR */
    CHECK_EQ(c.sense.asc, 0x11);               /* UNRECOVERED READ ERROR */

    fw_stats_t st;
    fw_get_stats(fw, &st);
    CHECK_EQ(st.crc_errors, 1);
    CHECK_EQ(st.crc_retries, 3);               /* tried N times first */
    CHECK_EQ(st.crc_unrecovered, 1);
    CHECK_EQ(st.crc_recovered, 0);

    /* The neighbours are fine; only the bad sector fails. */
    CHECK_EQ(read1(fw, 1000, 5, buf, NULL), FW_OK);
    CHECK_EQ(read1(fw, 1006, 10, buf, NULL), FW_OK);
    CHECK_EQ(read1(fw, 1005, 1, buf, NULL), FW_ERR_MEDIUM);
    fw_destroy(fw);
}

static void test_flip_in_crc_field_is_detected(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = write_and_reopen(&cfg, 0, 1, 1);
    CHECK_EQ(fw_inject_media_bitflip(fw, 0, 512 * 8 + 5), 0);  /* stored CRC */
    uint8_t buf[512];
    CHECK_EQ(read1(fw, 0, 1, buf, NULL), FW_ERR_MEDIUM);
    fw_destroy(fw);
}

static void test_rewrite_reallocates_bad_sector(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.write_cache = 0;     /* the rewrite goes straight to the media */
    fw_t *fw = write_and_reopen(&cfg, 2000, 4, 1);
    fw_inject_media_bitflip(fw, 2002, -1);
    uint8_t buf[4 * 512];
    CHECK_EQ(read1(fw, 2000, 4, buf, NULL), FW_ERR_MEDIUM);

    /* Host rewrites the sector with fresh data: the firmware moves it to a
     * spare (disk-sim G-list) and the read now succeeds. */
    for (uint32_t k = 0; k < 4; k++)
        pattern_fill(buf + k * 512, 512, 2000 + k, 2);
    fw_cmd_t w = mkcmd(FW_OP_WRITE, 0, 2000, 4, buf);
    CHECK_EQ(fw_exec(fw, &w), FW_OK);
    fw_stats_t st;
    fw_get_stats(fw, &st);
    CHECK_EQ(st.reallocations, 1);
    fw_shutdown(fw);
    fw_destroy(fw);

    cfg.media_create = 0;
    fw = fw_create(&cfg, NULL);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(read1(fw, 2000, 4, buf, NULL), FW_OK);
    int good = 0;
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t v = 0;
        good += pattern_check(buf + k * 512, 512, 2000 + k, &v) == PAT_OK && v == 2;
    }
    CHECK_EQ(good, 4);
    fw_destroy(fw);
}

static void test_transient_errors_recovered_by_retry(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.cache_entries = 16;          /* force nearly every read to the media */
    cfg.read_ahead_sectors = 0;
    cfg.read_retries = 8;            /* P(9 transient failures in a row) ~ 1e-12 */
    cfg.fault_read_flip_prob = 0.05;
    const uint32_t n = 512;
    fw_t *fw = write_and_reopen(&cfg, 4096, n, 7);

    uint8_t buf[8 * 512];
    int silent = 0, errors = 0;
    for (int pass = 0; pass < 4; pass++) {
        for (uint32_t i = 0; i < n; i += 8) {
            if (read1(fw, 4096 + i, 8, buf, NULL) != FW_OK) {
                errors++;
                continue;
            }
            for (uint32_t k = 0; k < 8; k++) {
                uint32_t v = 0;
                if (pattern_check(buf + k * 512, 512, 4096 + i + k, &v) != PAT_OK || v != 7)
                    silent++;            /* wrong data returned as OK! */
            }
        }
    }
    fw_stats_t st;
    fw_get_stats(fw, &st);
    CHECK(st.transient_flips > 0);
    CHECK(st.crc_errors > 0);
    CHECK_EQ(st.crc_recovered, st.crc_errors);   /* every one recovered */
    CHECK_EQ(st.crc_unrecovered, 0);
    CHECK_EQ(errors, 0);
    CHECK_EQ(silent, 0);
    printf("    (%" PRIu64 " sector reads from media, %" PRIu64 " transient flips, "
           "%" PRIu64 " CRC errors, %" PRIu64 " recovered)\n",
           st.media_sectors_read, st.transient_flips, st.crc_errors, st.crc_recovered);
    fw_destroy(fw);
}

static void test_zero_retries(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    cfg.read_retries = 0;
    fw_t *fw = write_and_reopen(&cfg, 50, 1, 1);
    fw_inject_media_bitflip(fw, 50, 77);
    uint8_t buf[512];
    CHECK_EQ(read1(fw, 50, 1, buf, NULL), FW_ERR_MEDIUM);
    fw_stats_t st;
    fw_get_stats(fw, &st);
    CHECK_EQ(st.crc_retries, 0);
    fw_destroy(fw);
}

static void test_unwritten_sector_corruption_detected(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = fw_create(&cfg, NULL);
    uint8_t buf[512];
    CHECK_EQ(read1(fw, 9999, 1, buf, NULL), FW_OK);      /* zeros, no CRC error */
    fw_inject_media_bitflip(fw, 9998, 3);
    CHECK_EQ(read1(fw, 9998, 1, buf, NULL), FW_ERR_MEDIUM);
    fw_destroy(fw);
}

/* A misdirected write: the record for LBA 300 (valid data, valid CRC for
 * LBA 300) ends up in LBA 301's slot. A CRC over the data alone would
 * accept it; ours includes the LBA, so it is rejected. */
static void test_misdirected_write_detected(void)
{
    fw_config_t cfg;
    test_config(&cfg, path);
    fw_t *fw = write_and_reopen(&cfg, 300, 2, 1);
    fw_destroy(fw);

    const size_t rec = 512 + 4;
    int fd = open(path, O_RDWR);
    uint8_t r[516];
    CHECK(pread(fd, r, rec, (off_t)(300 * rec)) == (ssize_t)rec);
    CHECK(pwrite(fd, r, rec, (off_t)(301 * rec)) == (ssize_t)rec);
    close(fd);

    cfg.media_create = 0;
    fw = fw_create(&cfg, NULL);
    uint8_t buf[512];
    CHECK_EQ(read1(fw, 300, 1, buf, NULL), FW_OK);
    uint64_t bad = 0;
    CHECK_EQ(read1(fw, 301, 1, buf, &bad), FW_ERR_MEDIUM);
    CHECK_EQ(bad, 301);
    fw_destroy(fw);
}

int main(void)
{
    tmp_media(path, sizeof(path), "integrity");
    printf("test_integrity (queue=%s)\n", fw_queue_impl());
    RUN_TEST(test_persistent_flip_gives_medium_error);
    RUN_TEST(test_flip_in_crc_field_is_detected);
    RUN_TEST(test_rewrite_reallocates_bad_sector);
    RUN_TEST(test_transient_errors_recovered_by_retry);
    RUN_TEST(test_zero_retries);
    RUN_TEST(test_unwritten_sector_corruption_detected);
    RUN_TEST(test_misdirected_write_detected);
    unlink(path);
    return test_summary("test_integrity");
}
