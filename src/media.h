/*
 * media.h - the "platters": disk-sim decides how long an access takes, a
 * backing file holds what is actually stored, and every sector carries a
 * CRC-32 that is verified on read.
 *
 * On-media sector record (rec_size = sector_size + 4 bytes):
 *
 *     +---------------------------+--------------------------------+
 *     | data[sector_size]         | crc32(data || lba_le64)  (LE)  |
 *     +---------------------------+--------------------------------+
 *
 * WHY the LBA is folded into the CRC (but not stored): it catches a
 * misdirected write or read - correct data, wrong place - which a CRC over
 * the data alone would happily accept. Real drives do the same with
 * LBA-seeded ECC/CRC and T10 PI "reference tags".
 *
 * WHY an all-zero record means "never written": the backing file is sparse
 * and a fresh drive must read back zeros without a CRC error, without a
 * slow full-surface format at startup. A bit flip on an unwritten sector
 * makes it non-zero, so it is still detected.
 *
 * Only the firmware thread calls media_read/media_write (disk_t is not
 * thread-safe, and the firmware owns the head).
 */
#ifndef FWSIM_MEDIA_H
#define FWSIM_MEDIA_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "disksim.h"
#include "fwsim.h"
#include "util.h"

/* Counters shared by media.c and firmware.c; see util.h for why each is a
 * single-writer relaxed atomic. */
typedef struct {
    counter_t cmds[FW_OP_COUNT];
    counter_t cmd_errors;
    counter_t read_hit_sectors, read_miss_sectors, write_hit_sectors;
    counter_t destage_runs, destaged_sectors;
    counter_t threshold_destages, forced_destages, idle_destages, flushes;
    counter_t readahead_ops, readahead_sectors;
    counter_t media_reads, media_writes;
    counter_t media_sectors_read, media_sectors_written;
    counter_t media_busy_us;      /* simulated microseconds */
    counter_t crc_errors, crc_retries, crc_recovered, crc_unrecovered;
    counter_t transient_flips, reallocations, torn_writes;
    counter_t cache_dirty, cache_used;
} fw_counters_t;

typedef struct {
    int         fd;
    disk_t     *disk;
    uint64_t    capacity;      /* LBAs exposed to the host               */
    uint32_t    sector_size;
    size_t      rec_size;      /* sector_size + 4                        */
    double      time_scale;
    double      rev_ms;
    double      sim_ms;        /* simulated clock passed to disk-sim     */
    uint64_t    t0_ns;         /* wall time when the drive powered on    */

    const atomic_int *power_on;   /* media ops abort when this drops to 0 */
    fw_counters_t    *ctr;

    uint32_t    read_retries;
    double      flip_prob;
    uint64_t    rng;           /* firmware thread only                   */
    _Atomic uint64_t inject_rng;  /* host-side fault injection           */

    uint64_t   *pending;       /* LBAs that failed a read: reallocate on next write */
    size_t      npending, pending_cap;

    uint8_t    *recbuf;        /* bounce buffer for records              */
    size_t      recbuf_sectors;
} media_t;

int  media_open(media_t *m, const fw_config_t *cfg, const atomic_int *power_on,
                fw_counters_t *ctr);
void media_close(media_t *m);

/* Read `n` sectors into buf. CRC-checked, with retries. On an unrecoverable
 * sector returns FW_ERR_MEDIUM and stores its LBA in *bad_lba. Returns
 * FW_ERR_ABORTED if power is lost during the access. */
fw_status_t media_read(media_t *m, uint64_t lba, uint32_t n, void *buf,
                       uint64_t *bad_lba);

/* Write `n` sectors. If power is lost mid-transfer, only the sectors that
 * had already passed under the head are written (a torn write) and
 * FW_ERR_ABORTED is returned. */
fw_status_t media_write(media_t *m, uint64_t lba, uint32_t n, const void *buf);

int media_inject_bitflip(media_t *m, uint64_t lba, int bit);

#endif
