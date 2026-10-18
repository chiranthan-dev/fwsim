/*
 * crc32.c - table-driven CRC-32.
 *
 * The bitwise algorithm shifts one bit at a time: 8 iterations per byte.
 * The table version precomputes the effect of all 256 possible bytes, so
 * each byte costs one table lookup + shift + XOR. Real drives do this in
 * hardware in the data path; in software, a 1 KiB table is the classic
 * size/speed trade-off (slice-by-8 would be ~4x faster with an 8 KiB table).
 *
 * The table is built once, lazily, under pthread_once(). WHY not a plain
 * "if (!initialised) build()": two threads could build it at the same time
 * and one could read a half-written table - a data race that TSan would
 * (rightly) report. pthread_once gives exactly-once initialisation with
 * the correct memory ordering for free.
 */
#include "crc32.h"

#include <pthread.h>

#define CRC32_POLY_REFLECTED 0xEDB88320u

static uint32_t table[256];
static pthread_once_t table_once = PTHREAD_ONCE_INIT;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (c >> 1) ^ CRC32_POLY_REFLECTED : c >> 1;
        table[i] = c;
    }
}

uint32_t crc32_update(uint32_t crc, const void *buf, size_t len)
{
    pthread_once(&table_once, build_table);
    const uint8_t *p = buf;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t crc32(const void *buf, size_t len)
{
    return crc32_final(crc32_update(CRC32_INIT, buf, len));
}

uint32_t crc32_bitwise(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    uint32_t crc = CRC32_INIT;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc & 1u) ? (crc >> 1) ^ CRC32_POLY_REFLECTED : crc >> 1;
    }
    return crc32_final(crc);
}
