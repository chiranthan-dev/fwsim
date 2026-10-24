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
#include "periph/env_sim.h"
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
    /* mechanical environment (media_write's environment path) */
    counter_t vib_offtrack_sectors, vib_stall_us, vib_false_stall_us, vib_stall_timeouts;
    counter_t vib_write_aborts, vib_extra_settles, vib_verified_writes, vib_verify_rewrites;
    /* vibration monitor (env_monitor.c) */
    counter_t vib_windows, vib_class_windows[4], vib_shock_irqs, vib_inhibits, vib_early_releases;
} fw_counters_t;

/*
 * Write gate: how a write-protection policy (env_monitor.c) steers the
 * media write path while an environment model is active. Before seeking,
 * and again at the instant the transfer would start, media_write asks the
 * gate for its current directives at simulated time t_ms; the gate first
 * brings its own view of the sensor up to t_ms (so it can only use what
 * the sensor has seen by then).
 *
 *   inhibit_until_ms  no sector may be written before this time: before
 *                     the seek the write waits (a stall); at transfer time
 *                     the write gate is closed - nothing is written, and
 *                     the write is retried after the inhibit
 *   settle_ms         minimum time from the end of a seek to the start of
 *                     the transfer; if the rotational wait is shorter, the
 *                     write waits one extra revolution
 *   verify            >0: read the sectors back one revolution later and
 *                     rewrite any that fail CRC, up to `verify` times
 *
 * No gate (NULL) = no protection at all (policy P0).
 */
typedef struct {
    double   inhibit_until_ms;
    double   settle_ms;
    uint32_t verify;
} media_wpolicy_t;

typedef void (*media_wgate_fn)(void *ctx, double t_ms, media_wpolicy_t *pol);

/* A write is held by inhibits for at most this long after its first stall
 * (per media write): firmware cannot stall a host command forever, so after
 * this it writes anyway and counts a stall timeout. */
#define MEDIA_MAX_STALL_MS   500.0
/* ... and is aborted at the write gate at most this many times. */
#define MEDIA_MAX_GATE_ABORTS 8u

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

    /* Where and when the previous media access ended (for read-ahead
     * streaming, see media_read_ahead). */
    uint64_t    last_end_lba;
    double      last_end_ms;
    int         last_was_read;

    /* Tracing (see fwsim.h). io_reason is set by the firmware before each
     * media call so the trace can say WHY the head is moving. */
    fw_trace_fn trace;
    void       *trace_ctx;
    uint8_t     io_reason;

    /* Mechanical environment (NULL when cfg.vib_env is off). The model
     * belongs to the media: it is the physical world the head lives in. */
    env_sim_t      *env;
    uint64_t        ot_rng;      /* off-track draws, firmware thread only */
    media_wgate_fn  wgate;       /* write protection policy (may be NULL) */
    void           *wgate_ctx;

    uint32_t    nzones;
    disk_zone_t zones[DISK_MAX_ZONES];
} media_t;

int  media_open(media_t *m, const fw_config_t *cfg, const atomic_int *power_on,
                fw_counters_t *ctr);
void media_close(media_t *m);

/* Read `n` sectors into buf. CRC-checked, with retries. On an unrecoverable
 * sector returns FW_ERR_MEDIUM and stores its LBA in *bad_lba. Returns
 * FW_ERR_ABORTED if power is lost during the access. */
fw_status_t media_read(media_t *m, uint64_t lba, uint32_t n, void *buf,
                       uint64_t *bad_lba);

/* Like media_read, for prefetch. If the range starts exactly where the
 * previous READ ended, the head never stopped: a real drive keeps its read
 * channel streaming into the buffer after a sequential read, so there is
 * no rotational miss - only transfer time is charged. */
fw_status_t media_read_ahead(media_t *m, uint64_t lba, uint32_t n, void *buf,
                             uint64_t *bad_lba);

/* Write `n` sectors. If power is lost mid-transfer, only the sectors that
 * had already passed under the head are written (a torn write) and
 * FW_ERR_ABORTED is returned. */
fw_status_t media_write(media_t *m, uint64_t lba, uint32_t n, const void *buf);

int media_inject_bitflip(media_t *m, uint64_t lba, int bit);

/* The simulated clock now (ms), as the next access would see it.
 * Firmware thread only. */
double media_now_ms(media_t *m);

/* Fill ev->cyl/head/sector/spt for `lba` (sets has_chs). Firmware thread. */
void media_locate(const media_t *m, uint64_t lba, fw_trace_t *ev);

#endif
