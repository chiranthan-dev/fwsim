/*
 * powerloss.h - the power-loss experiment.
 *
 * One trial:
 *   1. Power on a blank drive. The host writes 4 KiB blocks at random
 *      (queue depth qd) and records, per block, the latest version the
 *      drive ACKNOWLEDGED, and the latest version the drive PROMISED was
 *      durable (FUA ack, write-through ack, or covered by a completed
 *      FLUSH).
 *   2. At a random moment, cut power: the firmware stops wherever it is
 *      (mid-seek, mid-destage), DRAM cache contents are discarded.
 *   3. "Reboot": a fresh firmware instance on the same backing file, cold
 *      cache. Read every block back and compare with the host's records.
 *
 * Two different failures are counted:
 *   lost       - the latest ACKNOWLEDGED data is not on the media. Allowed
 *                by the contract of a volatile write cache! This is the
 *                cost of write-back caching without FLUSH.
 *   violations - data the drive PROMISED was durable is gone. Must be 0 in
 *                every mode; anything else is a firmware bug.
 */
#ifndef FWSIM_POWERLOSS_H
#define FWSIM_POWERLOSS_H

#include <stdint.h>

#include "fwsim.h"

typedef enum {
    PL_WB_NOFLUSH = 0,  /* write-back, host never flushes            */
    PL_WB_FLUSH,        /* write-back, FLUSH after every N acked writes */
    PL_WB_FUA,          /* write-back cache, but every write has FUA  */
    PL_WT,              /* write cache disabled (write-through)       */
    PL_MODE_COUNT
} pl_mode_t;

typedef struct {
    pl_mode_t   mode;
    fw_config_t fw;             /* base drive config (media path, timing) */
    uint32_t    blocks;         /* working set, in blocks                 */
    uint32_t    block_sectors;  /* 8 = 4 KiB                              */
    uint32_t    qd;
    uint32_t    flush_every;    /* PL_WB_FLUSH                            */
    double      cut_min_ms;     /* the cut happens uniformly at random    */
    double      cut_max_ms;     /* in [min, max] wall-clock ms after start */
    uint64_t    seed;
} pl_params_t;

typedef struct {
    uint64_t writes_submitted;
    uint64_t writes_acked;
    uint64_t flushes_acked;
    uint64_t blocks_checked;    /* blocks with at least one acked write   */
    uint64_t lost;              /* latest acked version missing           */
    uint64_t violations;        /* promised-durable version missing       */
    uint64_t unreadable;        /* read error or garbage after reboot     */
    uint64_t newer_than_ack;    /* unacknowledged writes that did land    */
    uint64_t dirty_at_cut;      /* dirty sectors discarded by the cut     */
    uint64_t torn_writes;       /* media writes interrupted by the cut    */
    double   run_wall_ms;       /* power-on to cut                        */
} pl_result_t;

const char *pl_mode_str(pl_mode_t m);
void        pl_params_default(pl_params_t *p);
int         pl_run_trial(const pl_params_t *p, pl_result_t *r);   /* 0 / -1 */

#endif
