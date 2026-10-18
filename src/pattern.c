#include "pattern.h"

#include <string.h>

#include "util.h"

#define PAT_MAGIC 0x50535746u   /* "FWSP" little-endian */
#define PAT_HDR   16u

static uint64_t seed_of(uint64_t lba, uint32_t version)
{
    return lba * 0xD6E8FEB86659FD93ull ^ ((uint64_t)version << 32 | version);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

void pattern_fill(void *sector, uint32_t size, uint64_t lba, uint32_t version)
{
    uint8_t *p = sector;
    put32(p, PAT_MAGIC);
    put32(p + 4, version);
    put32(p + 8, (uint32_t)lba);
    put32(p + 12, (uint32_t)(lba >> 32));
    uint64_t s = seed_of(lba, version);
    for (uint32_t i = PAT_HDR; i < size; i += 8) {
        uint64_t r = rng_next64(&s);
        for (uint32_t k = 0; k < 8 && i + k < size; k++)
            p[i + k] = (uint8_t)(r >> (8 * k));
    }
}

int pattern_check(const void *sector, uint32_t size, uint64_t lba, uint32_t *version)
{
    const uint8_t *p = sector;
    int zero = 1;
    for (uint32_t i = 0; i < size; i++)
        if (p[i]) {
            zero = 0;
            break;
        }
    if (zero)
        return PAT_ZERO;
    if (get32(p) != PAT_MAGIC)
        return PAT_BAD;
    uint64_t got_lba = (uint64_t)get32(p + 8) | (uint64_t)get32(p + 12) << 32;
    if (got_lba != lba)
        return PAT_BAD;
    uint32_t v = get32(p + 4);
    uint64_t s = seed_of(lba, v);
    for (uint32_t i = PAT_HDR; i < size; i += 8) {
        uint64_t r = rng_next64(&s);
        for (uint32_t k = 0; k < 8 && i + k < size; k++)
            if (p[i + k] != (uint8_t)(r >> (8 * k)))
                return PAT_BAD;
    }
    *version = v;
    return PAT_OK;
}
