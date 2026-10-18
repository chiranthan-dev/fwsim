/*
 * test_crc.c - CRC-32 against the standard check value, an independent
 * bitwise implementation, and its error-detection guarantees.
 */
#include <pthread.h>

#include "check.h"
#include "crc32.h"
#include "util.h"

/* Runs FIRST, before anything has touched the lazily built table: several
 * threads race to compute a CRC. pthread_once must make that safe (and
 * TSan must stay quiet). */
#define RACERS 8
static uint32_t racer_out[RACERS];

static void *racer(void *arg)
{
    racer_out[(size_t)arg] = crc32("123456789", 9);
    return NULL;
}

static void test_concurrent_first_use(void)
{
    pthread_t t[RACERS];
    for (size_t i = 0; i < RACERS; i++)
        pthread_create(&t[i], NULL, racer, (void *)i);
    for (size_t i = 0; i < RACERS; i++) {
        pthread_join(t[i], NULL);
        CHECK_EQ(racer_out[i], 0xCBF43926u);
    }
}

static void test_check_value(void)
{
    /* The catalogue check value for CRC-32/ISO-HDLC. */
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(crc32_bitwise("123456789", 9), 0xCBF43926u);
}

static void test_known_vectors(void)
{
    CHECK_EQ(crc32("", 0), 0x00000000u);
    CHECK_EQ(crc32("a", 1), 0xE8B7BE43u);
    const char *fox = "The quick brown fox jumps over the lazy dog";
    CHECK_EQ(crc32(fox, strlen(fox)), 0x414FA339u);
}

static void test_table_matches_bitwise(void)
{
    uint64_t rng = 99;
    uint8_t buf[1031];
    for (size_t len = 0; len < sizeof(buf); len += 37) {
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)rng_next64(&rng);
        CHECK_EQ(crc32(buf, len), crc32_bitwise(buf, len));
    }
}

static void test_incremental(void)
{
    const char *s = "123456789";
    for (size_t split = 0; split <= 9; split++) {
        uint32_t c = crc32_update(CRC32_INIT, s, split);
        c = crc32_update(c, s + split, 9 - split);
        CHECK_EQ(crc32_final(c), 0xCBF43926u);
    }
}

/* A CRC-32 detects EVERY single-bit error: flip each bit of a 516-byte
 * sector record in turn and the CRC must change every time. */
static void test_detects_every_single_bit_flip(void)
{
    uint8_t rec[516];
    uint64_t rng = 7;
    for (size_t i = 0; i < sizeof(rec); i++)
        rec[i] = (uint8_t)rng_next64(&rng);
    uint32_t good = crc32(rec, sizeof(rec));
    int missed = 0;
    for (size_t bit = 0; bit < sizeof(rec) * 8; bit++) {
        rec[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        if (crc32(rec, sizeof(rec)) == good)
            missed++;
        rec[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    CHECK_EQ(missed, 0);
}

/* ...and every burst error of length <= 32 bits. */
static void test_detects_bursts(void)
{
    uint8_t rec[128];
    memset(rec, 0x5A, sizeof(rec));
    uint32_t good = crc32(rec, sizeof(rec));
    uint64_t rng = 3;
    int missed = 0;
    for (int trial = 0; trial < 5000; trial++) {
        uint8_t copy[sizeof(rec)];
        memcpy(copy, rec, sizeof(rec));
        unsigned len = 1 + (unsigned)rng_below64(&rng, 32);
        size_t start = rng_below64(&rng, sizeof(rec) * 8 - len);
        /* A burst: first and last bit flipped, anything in between. */
        uint64_t pattern = rng_next64(&rng) | 1u | (1ull << (len - 1));
        for (unsigned k = 0; k < len; k++)
            if (pattern >> k & 1)
                copy[(start + k) / 8] ^= (uint8_t)(1u << ((start + k) % 8));
        if (crc32(copy, sizeof(copy)) == good)
            missed++;
    }
    CHECK_EQ(missed, 0);
}

int main(void)
{
    printf("test_crc\n");
    RUN_TEST(test_concurrent_first_use);
    RUN_TEST(test_check_value);
    RUN_TEST(test_known_vectors);
    RUN_TEST(test_table_matches_bitwise);
    RUN_TEST(test_incremental);
    RUN_TEST(test_detects_every_single_bit_flip);
    RUN_TEST(test_detects_bursts);
    return test_summary("test_crc");
}
