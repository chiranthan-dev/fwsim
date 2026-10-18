/*
 * crc32.h - CRC-32 (IEEE 802.3 / zlib / PNG polynomial), used as the
 * per-sector integrity check.
 *
 * Parameters ("CRC-32/ISO-HDLC" in the CRC catalogue):
 *   polynomial 0x04C11DB7, processed bit-reversed as 0xEDB88320,
 *   init 0xFFFFFFFF, input and output reflected, final XOR 0xFFFFFFFF.
 *   Check value: crc32("123456789") == 0xCBF43926.
 *
 * WHY CRC and not a simple checksum: a CRC detects every single-bit error,
 * every 2-bit error, every burst of up to 32 bits, and all odd numbers of
 * flipped bits. A byte-sum misses e.g. two bytes swapped. That burst
 * property matters on a disk, where media defects corrupt adjacent bits.
 *
 * The API is incremental (init/update/final) so the firmware can chain the
 * sector data and the sector's LBA into one CRC (see media.c) without
 * copying them into a single buffer first.
 */
#ifndef FWSIM_CRC32_H
#define FWSIM_CRC32_H

#include <stddef.h>
#include <stdint.h>

#define CRC32_INIT 0xFFFFFFFFu

uint32_t crc32_update(uint32_t crc, const void *buf, size_t len);
static inline uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

/* One-shot convenience: crc32_final(crc32_update(CRC32_INIT, buf, len)). */
uint32_t crc32(const void *buf, size_t len);

/* Bit-at-a-time reference implementation. Slow; exists so the tests can
 * check the table-driven version against an independent implementation. */
uint32_t crc32_bitwise(const void *buf, size_t len);

#endif
