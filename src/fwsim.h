/*
 * fwsim.h - public API of the simulated HDD firmware command processor.
 *
 * The picture (see README for the diagram):
 *
 *   host thread --fw_submit()--> [submission queue] --> firmware thread
 *        ^                                                   |
 *        |                                    dispatch table by opcode
 *        |                                                   v
 *        |                                       write-back sector cache
 *        |                                                   |
 *        |                                       media: disk-sim timing +
 *        |                                       backing file + CRC-32
 *   fw_poll() <------------- [completion queue] <------------+
 *
 * Command ownership: a fw_cmd_t belongs to the host until fw_submit(),
 * to the firmware until it is posted to the completion queue, and to the
 * host again once fw_poll() returns it. The queues' acquire/release
 * ordering publishes the struct's contents at each hand-off, so the
 * command struct itself needs no lock or atomics. Touching a command while
 * the firmware owns it is a data race (TSan will flag it).
 *
 * Threading contract: fw_submit() and fw_poll() must be called from ONE
 * host thread (the lock-free queues are single-producer/single-consumer).
 */
#ifndef FWSIM_H
#define FWSIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "disksim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- commands ------------------------------------------------------------ */

typedef enum {
    FW_OP_IDENTIFY = 0,     /* return drive identity / capabilities      */
    FW_OP_READ,
    FW_OP_WRITE,            /* honours FW_CMD_FUA                        */
    FW_OP_FLUSH,            /* destage all dirty data to media           */
    FW_OP_SET_FEATURES,     /* e.g. enable/disable the write cache       */
    FW_OP_SMART_READ,       /* SMART attributes + failure prediction, see
                               fw_smart_data_t (needs SMART enabled)     */
    FW_OP_COUNT
} fw_opcode_t;

#define FW_CMD_FUA 0x01     /* Force Unit Access: data on media before ack */

/* NCQ allows 32 outstanding commands, identified by a 5-bit tag. */
#define FW_MAX_TAGS 32

/* SET_FEATURES feature codes (modelled on ATA SET FEATURES 02h/82h). */
#define FW_FEAT_WRITE_CACHE 0x02   /* value: 1 = write-back, 0 = write-through */
#define FW_FEAT_READ_AHEAD  0xAA   /* value: 1 = on, 0 = off                   */
#define FW_FEAT_SMART       0xD8   /* value: 1 = enable SMART, 0 = disable
                                      (ATA SMART ENABLE/DISABLE OPERATIONS) */

/* SMART_READ "page" (cmd->feature), like ATA SMART READ DATA (D0h). */
#define FW_SMART_PAGE_DATA  0xD0

/*
 * Status codes, modelled loosely on SCSI sense data. Each maps to a sense
 * key + additional sense code (ASC/ASCQ), see fw_status_sense().
 */
typedef enum {
    FW_OK = 0,
    FW_ERR_INVALID_OPCODE,  /* ILLEGAL REQUEST   20/00 invalid command op code */
    FW_ERR_INVALID_FIELD,   /* ILLEGAL REQUEST   24/00 invalid field in CDB    */
    FW_ERR_INVALID_LBA,     /* ILLEGAL REQUEST   21/00 LBA out of range        */
    FW_ERR_MEDIUM,          /* MEDIUM ERROR      11/00 unrecovered read error  */
    FW_ERR_WRITE_FAULT,     /* MEDIUM ERROR      0C/00 write error             */
    FW_ERR_TAG_IN_USE,      /* ABORTED COMMAND   4E/00 overlapped commands     */
    FW_ERR_ABORTED,         /* ABORTED COMMAND   00/00 (power loss, shutdown)  */
    FW_ERR_HARDWARE,        /* HARDWARE ERROR    44/00 internal target failure */
    FW_ERR_SMART_DISABLED,  /* ILLEGAL REQUEST   2C/00 command sequence error:
                               SMART_READ while SMART is disabled           */
    /* Not an error: the command succeeded and its data is valid, but the
     * drive predicts its own failure (SCSI informational exception). */
    FW_INFO_FAILURE_PREDICTED, /* RECOVERED ERROR 5D/00 failure prediction
                               threshold exceeded                          */
    FW_STATUS_COUNT
} fw_status_t;

typedef struct {
    uint8_t key;    /* sense key */
    uint8_t asc;    /* additional sense code */
    uint8_t ascq;   /* additional sense code qualifier */
} fw_sense_t;

/* Per-command lifecycle. QUEUED is set by fw_submit (host side); ACTIVE
 * and COMPLETED by the firmware thread. */
typedef enum {
    FW_CMD_FREE = 0,
    FW_CMD_QUEUED,
    FW_CMD_ACTIVE,
    FW_CMD_COMPLETED
} fw_cmd_state_t;

typedef struct fw_cmd {
    /* --- filled by the host --- */
    uint8_t     opcode;       /* fw_opcode_t (uint8_t: invalid values are testable) */
    uint8_t     flags;        /* FW_CMD_FUA                                  */
    uint8_t     tag;          /* 0 .. FW_MAX_TAGS-1, unique while outstanding */
    uint64_t    lba;
    uint32_t    count;        /* sectors                                     */
    void       *buf;          /* count * sector_size bytes (IDENTIFY: 1 sector) */
    uint32_t    feature;      /* SET_FEATURES only                            */
    uint32_t    feature_value;
    void       *user;         /* host's private pointer, untouched by firmware */

    /* --- filled by the firmware --- */
    fw_status_t     status;
    fw_sense_t      sense;
    uint64_t        err_lba;  /* first failing LBA (sense "information" field) */
    fw_cmd_state_t  state;
    uint64_t        t_submit_ns, t_start_ns, t_done_ns;   /* CLOCK_MONOTONIC */
} fw_cmd_t;

/* IDENTIFY payload (fits in one 512-byte sector, like ATA IDENTIFY). */
typedef struct {
    char     model[40];
    char     serial[20];
    char     firmware_rev[8];
    uint64_t capacity_lbas;
    uint32_t sector_size;
    uint32_t queue_depth;
    uint32_t cache_entries;
    uint8_t  write_cache_enabled;
    uint8_t  read_ahead_enabled;
    uint8_t  fua_supported;
    char     queue_impl[12];      /* "lockfree" or "mutex" */
    uint8_t  smart_supported;     /* FW_OP_SMART_READ implemented          */
    uint8_t  smart_enabled;       /* ... and currently enabled             */
    uint8_t  reserved[2];
    uint32_t smart_model_id;      /* identifies the exported model         */
} fw_identify_t;

/* ---- SMART ------------------------------------------------------------------
 *
 * SMART_READ (cmd->feature = FW_SMART_PAGE_DATA, lba = 0, count = 1, buf =
 * one sector) returns this, zero-padded to a sector. The attribute table
 * mirrors ATA SMART; the prediction comes from a logistic regression
 * trained offline on Backblaze data (tools/smart/) and exported to integer
 * tables (src/smart_model.h). See README "SMART failure prediction".
 * When the prediction trips, the command completes with status
 * FW_INFO_FAILURE_PREDICTED (data still valid) instead of FW_OK.
 */
#define FW_SMART_VERSION 1
#define FW_SMART_NATTR   7

typedef struct {
    uint8_t  id;            /* SMART attribute number (5, 9, 187, ...)   */
    uint8_t  normalized;    /* 100 = new, lower = worse                   */
    uint8_t  worst;         /* lowest normalised value ever reported      */
    uint8_t  threshold;     /* normalised <= threshold: attribute failing */
    uint32_t reserved;
    uint64_t raw;
} fw_smart_attr_t;

typedef struct {
    uint16_t version;            /* FW_SMART_VERSION                       */
    uint8_t  num_attrs;          /* FW_SMART_NATTR                          */
    uint8_t  predicted_failure;  /* 1: the model's flag has tripped         */
    uint16_t risk_bp;            /* P(fail within 30 days), basis points    */
    uint16_t history_days;       /* daily samples in the history ring       */
    int32_t  logit_q8;           /* model log-odds x 256                    */
    int32_t  threshold_q8;       /* flag when logit_q8 >= this              */
    uint32_t model_id;
    uint32_t power_on_hours;
    fw_smart_attr_t attr[FW_SMART_NATTR];
} fw_smart_data_t;

/* ---- tracing ----------------------------------------------------------------
 *
 * An optional observer for visualisation (the live web dashboard) and
 * debugging. When fw_config_t.trace is set, the firmware calls it ON THE
 * FIRMWARE THREAD for every media access, command start/finish and CRC
 * event. The callback must be quick and must not call back into the
 * fw_* API (it would deadlock or race); copy what it needs and return.
 * With trace == NULL nothing is computed and nothing is called.
 */
typedef enum {
    FW_TRACE_MEDIA = 0,     /* a media access is starting (timing known)   */
    FW_TRACE_CMD_START,     /* firmware dequeued a command (ACTIVE)        */
    FW_TRACE_CMD_DONE,      /* completion about to be posted (COMPLETED)   */
    FW_TRACE_CRC,           /* a CRC mismatch was resolved (or not)        */
    FW_TRACE_VIBRATION      /* vibration monitor: a classifier window, or a
                               threshold interrupt (vib_class = 255)       */
} fw_trace_kind_t;

typedef enum {
    FW_IO_HOST = 0,         /* serving a host READ/WRITE                   */
    FW_IO_DESTAGE,          /* writing dirty cache data back               */
    FW_IO_READAHEAD         /* prefetch after a sequential read            */
} fw_io_reason_t;

typedef struct {
    fw_trace_kind_t kind;
    double      sim_ms;     /* simulated drive time of the event           */
    uint8_t     opcode;     /* CMD_*: the command's opcode                  */
    uint8_t     tag;        /* CMD_*                                        */
    uint8_t     flags;      /* CMD_*: FW_CMD_FUA                            */
    uint8_t     is_write;   /* MEDIA                                        */
    uint8_t     reason;     /* MEDIA: fw_io_reason_t                        */
    uint8_t     wce;        /* CMD_DONE: write cache was enabled            */
    uint8_t     has_chs;    /* cyl/head/sector/spt below are valid          */
    fw_status_t status;     /* CMD_DONE                                     */
    int         recovered;  /* CRC: 1 = fixed by a retry, 0 = MEDIUM ERROR  */
    uint64_t    lba;        /* first sector (all kinds)                     */
    uint32_t    count;
    uint32_t    cyl, head, sector, spt;  /* where `lba` physically lives   */
    double      seek_ms, rot_ms, xfer_ms; /* MEDIA: disk-sim's breakdown   */
    /* VIBRATION */
    uint8_t     vib_class;  /* 0 quiet 1 shock 2 vibration 3 tap; 255 = threshold IRQ */
    uint8_t     vib_conf;   /* classifier confidence, percent              */
    uint8_t     vib_inhibit;  /* writes are inhibited after this event      */
    uint8_t     vib_protect;  /* vibration mode: extra settle + verify      */
    uint32_t    vib_rms_mg, vib_peak_mg;  /* of the classified window       */
    double      vib_inhibit_until_ms;
} fw_trace_t;

typedef void (*fw_trace_fn)(void *ctx, const fw_trace_t *ev);

/* ---- configuration -------------------------------------------------------- */

typedef struct {
    const char   *media_path;     /* backing file holding sector data + CRC   */
    int           media_create;   /* 1: start from a blank (all-zero) medium  */
    disk_config_t disk;           /* disk-sim geometry and seek model         */
    uint64_t      capacity_lbas;  /* 0 = whole disk; else expose only this many */

    /* Wall-clock seconds per simulated second of media time. 1.0 = real
     * time, 0.02 = a 10 ms seek takes 200 us, 0 = media takes no time
     * (unit tests). All reported latencies are converted back to drive
     * time by dividing by this factor. */
    double        time_scale;

    size_t        cache_entries;      /* sectors of DRAM cache             */
    int           write_cache;        /* 1 = write-back (WCE), 0 = write-through */
    double        destage_high;       /* start destaging at this dirty fraction */
    double        destage_low;        /* ... and stop at this one              */
    int           destage_sorted;     /* 1 = sort by LBA (elevator), 0 = age order */
    uint32_t      destage_runs_per_cmd; /* background destage writes allowed
                                         between two host commands         */
    double        idle_destage_ms;    /* simulated idle time before background
                                         destage; 0 = never destage on idle  */
    uint32_t      idle_destage_batch; /* entries destaged per idle tick      */
    uint32_t      read_ahead_sectors; /* prefetch after a sequential read; 0=off */
    uint32_t      read_retries;       /* re-reads after a CRC mismatch       */
    double        fault_read_flip_prob; /* per sector read: transient bit flip */
    uint64_t      seed;
    fw_trace_fn   trace;              /* optional observer, see above      */
    void         *trace_ctx;
    int           smart_enable;       /* 1 = SMART on at power-up (default 0;
                                         SET_FEATURES FW_FEAT_SMART toggles) */
    uint32_t      smart_temp_c;       /* temperature input (no thermal model) */

    /* ---- mechanical environment (README "Edge AI vibration classifier").
     * Everything is OFF by default: with vib_env == 0 there is no
     * environment model, no off-track physics, and the media path is
     * exactly the one without this feature. */
    int           vib_env;            /* env_mix_t (periph/env_sim.h); 0 = off */
    uint64_t      vib_seed;           /* environment seed; 0 = use `seed`      */
    /* Write protection (env_monitor.h); needs vib_env. 0 = default for
     * every numeric field below. */
    int           vib_policy;         /* FW_VIB_*; 0 = none (P0)               */
    uint32_t      vib_threshold_mg;   /* P1/P3 shock threshold (250)           */
    double        vib_settle_ms;      /* inhibit after a shock (20)            */
    uint32_t      vib_window;         /* classifier window, samples (exported default) */
    double        vib_extra_settle_ms;/* vibration: min post-seek settle (1.5) */
    uint32_t      vib_verify_passes;  /* vibration: verify-after-write (2)     */
    int           vib_irq_threaded;   /* 1: ISRs on an interrupt thread (TSan) */
} fw_config_t;

/* Write-protection policies (fw_config_t.vib_policy). */
#define FW_VIB_NONE      0   /* P0 */
#define FW_VIB_THRESHOLD 1   /* P1: threshold interrupt, no AI */
#define FW_VIB_AI        2   /* P2: classifier */
#define FW_VIB_HYBRID    3   /* P3: threshold for speed + classifier */

/* ---- statistics (a snapshot; safe to call at any time) ------------------- */

typedef struct {
    uint64_t cmds[FW_OP_COUNT];
    uint64_t cmd_errors;
    uint64_t read_hit_sectors, read_miss_sectors;
    uint64_t write_hit_sectors;          /* writes that overwrote a cached sector */
    uint64_t destage_runs, destaged_sectors;
    uint64_t threshold_destages, forced_destages, idle_destages, flushes;
    uint64_t readahead_ops, readahead_sectors;
    uint64_t media_reads, media_writes;
    uint64_t media_sectors_read, media_sectors_written;
    double   media_busy_ms;              /* simulated mechanical time          */
    uint64_t crc_errors;                 /* CRC mismatches seen on read        */
    uint64_t crc_retries;
    uint64_t crc_recovered;              /* fixed by a retry                   */
    uint64_t crc_unrecovered;            /* reported as MEDIUM_ERROR           */
    uint64_t transient_flips;            /* injected read-channel bit flips    */
    uint64_t reallocations;              /* LBAs moved to spares (disk-sim G-list) */
    uint64_t torn_writes;                /* media writes cut short by power loss */
    size_t   cache_dirty;                /* at the time of the snapshot        */
    size_t   cache_used;

    /* Mechanical environment (all zero while vib_env is off). */
    uint64_t vib_offtrack_sectors;       /* stored off-track (ground truth)    */
    double   vib_stall_ms;               /* writes held by a write inhibit     */
    double   vib_false_stall_ms;         /* ... while the environment was
                                            harmless (quiet or tapping)        */
    uint64_t vib_stall_timeouts;         /* inhibit outlasted the stall limit  */
    uint64_t vib_write_aborts;           /* write gate closed before transfer  */
    uint64_t vib_extra_settles;          /* extra settle cost a revolution     */
    uint64_t vib_verified_writes;        /* writes read back after writing     */
    uint64_t vib_verify_rewrites;        /* sectors rewritten after a verify   */
    uint64_t vib_windows;                /* classifier windows run             */
    uint64_t vib_class_windows[4];       /* ... by class (quiet shock vib tap)  */
    uint64_t vib_shock_irqs;             /* threshold interrupts handled       */
    uint64_t vib_inhibits;               /* write-inhibit episodes             */
    uint64_t vib_early_releases;         /* hybrid: inhibits ended by the model */
    uint64_t vib_isr_runs;               /* accelerometer ISR invocations      */
} fw_stats_t;

typedef struct fw fw_t;

void        fw_config_default(fw_config_t *cfg);
const char *fw_status_str(fw_status_t st);
fw_sense_t  fw_status_sense(fw_status_t st);
const char *fw_opcode_str(uint8_t op);
const char *fw_queue_impl(void);     /* "lockfree" or "mutex" (build-time) */

/* Create the drive and start the firmware thread. NULL on error; *err gets
 * a negative errno-style code if err != NULL. */
fw_t *fw_create(const fw_config_t *cfg, int *err);

/* Hand a command to the firmware (host thread only). Returns FW_OK, or:
 *   FW_ERR_INVALID_FIELD  tag out of range / NULL cmd
 *   FW_ERR_TAG_IN_USE     another command with this tag is outstanding
 *   FW_ERR_ABORTED        the drive is powered off or shutting down
 * On error the command was NOT queued and no completion will arrive. */
fw_status_t fw_submit(fw_t *fw, fw_cmd_t *cmd);

/* Next completed command, waiting up to timeout_us (0 = don't wait).
 * NULL if none. Host thread only. */
fw_cmd_t *fw_poll(fw_t *fw, uint64_t timeout_us);

/* Submit one command and wait for its completion (no other commands may
 * be outstanding). Convenience for tests and the reboot verifier. */
fw_status_t fw_exec(fw_t *fw, fw_cmd_t *cmd);

/* Clean shutdown: finish queued commands, destage the whole cache (like
 * ATA STANDBY IMMEDIATE), stop the thread. Idempotent. */
void fw_shutdown(fw_t *fw);

/* Power loss: the firmware stops at the next instruction boundary, even in
 * the middle of a seek or a multi-sector write (torn write). Nothing is
 * flushed. Joins the firmware thread; completions already posted before
 * the cut can still be collected with fw_poll(). */
void fw_power_cut(fw_t *fw);

/* Free everything. The cache is discarded, never flushed: call
 * fw_shutdown() first if you want the data. */
void fw_destroy(fw_t *fw);

void     fw_get_stats(fw_t *fw, fw_stats_t *out);
uint64_t fw_capacity(const fw_t *fw);
uint32_t fw_sector_size(const fw_t *fw);
double   fw_time_scale(const fw_t *fw);
/* Simulated drive time now, estimated from the wall clock since power-on
 * (ms). Safe from any thread. 0 when time_scale is 0. */
double   fw_sim_now_ms(const fw_t *fw);

/* Fault injection: flip one bit of the stored record (data or CRC) of
 * `lba` on the medium. bit = -1 picks a random bit. Persistent: every
 * later read of that sector fails CRC until it is rewritten. Safe to call
 * while the firmware runs (it is a separate pwrite on the backing file),
 * but the effect on an in-flight read of the same sector is undefined -
 * just as with a real media defect. Returns 0 or -1. */
int fw_inject_media_bitflip(fw_t *fw, uint64_t lba, int bit);

/* Simulation hooks for SMART (safe from the host thread at any time):
 * let `hours` of simulated power-on time pass without media activity
 * (the drive sat idle), and set the temperature sensor input. They change
 * nothing unless SMART is enabled. */
void fw_sim_advance_hours(fw_t *fw, uint64_t hours);
void fw_sim_set_temperature(fw_t *fw, uint32_t celsius);

#ifdef __cplusplus
}
#endif

#endif
