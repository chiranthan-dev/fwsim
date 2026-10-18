/*
 * disksim.h - public API of the disk-sim media layer.
 *
 * This header is the ONLY thing another project needs to link against
 * libdisksim.a and use the simulated hard disk as its "media": open a disk
 * from a geometry/timing config, hand it requests, get back how long the
 * mechanics took. Everything else in the library (schedulers, workloads,
 * metrics) is built on top of this API, not inside it.
 *
 * Design rules for this API:
 *   - The disk is an opaque handle (disk_t). Callers cannot poke at the
 *     zone table or defect lists directly, so the internals can change
 *     without breaking a project that links us.
 *   - Time is passed IN by the caller (now_ms). The disk never owns a clock.
 *     That keeps it usable from any event loop: our simulator, a cache
 *     simulator, a test that wants to start at an exact platter angle.
 *   - Functions return int status codes (0 = OK, negative = DISK_E*),
 *     results come back through out-pointers. No errno, no globals.
 *   - All addresses are zero-based. LBA 0 is the first user sector.
 *
 * Units: time in milliseconds (double), sizes in sectors unless stated.
 */
#ifndef DISKSIM_H
#define DISKSIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- status codes ------------------------------------------------------ */
#define DISK_OK          0
#define DISK_EINVAL    (-1)  /* bad argument or bad config                  */
#define DISK_ERANGE    (-2)  /* LBA / CHS outside the user-addressable area */
#define DISK_ENOSPC    (-3)  /* spare area exhausted, cannot reallocate     */
#define DISK_EDEFECT   (-4)  /* CHS points at a defective / unmapped sector */
#define DISK_ENOMEM    (-5)  /* allocation failed                           */

/* Upper bound on zones; real drives use ~16-30 recording zones. */
#define DISK_MAX_ZONES 64

typedef struct disk disk_t;  /* opaque */

typedef enum { DISK_OP_READ = 0, DISK_OP_WRITE = 1 } disk_op_t;

/*
 * Drive configuration. Fill with disk_config_default() and override fields.
 */
typedef struct {
    /* --- geometry --- */
    uint32_t cylinders;          /* tracks per surface                         */
    uint32_t heads;              /* recording surfaces (2 per platter)         */
    uint32_t sectors_per_track;  /* used when zoned == 0                       */
    uint32_t sector_size;        /* bytes: 512 or 4096                         */
    uint32_t rpm;                /* spindle speed                              */

    /* --- zoned bit recording (ZBR) --- */
    int      zoned;              /* 0 = fixed SPT, 1 = zones                   */
    uint32_t num_zones;          /* zones, outer (cyl 0) to inner              */
    uint32_t spt_outer;          /* SPT in the outermost zone                  */
    uint32_t spt_inner;          /* SPT in the innermost zone                  */

    /* --- seek model: 0 if distance==0 else settle + k*sqrt(distance) --- */
    double   seek_settle_ms;
    double   seek_k_ms;

    /*
     * Cylinder skew, as a fraction of one revolution. Sector 0 of cylinder c
     * sits at angle frac(c * cyl_skew). It lets a sequential transfer that
     * crosses into the next cylinder catch sector 0 right after the
     * one-track seek instead of waiting almost a full revolution.
     */
    double   cyl_skew;

    /* --- defect management --- */
    uint32_t spare_cylinders;    /* innermost cylinders reserved as G-list spares */
    const uint64_t *plist;       /* factory defects as physical sector numbers  */
    size_t   plist_len;          /* (copied by disk_open; may be NULL/0)        */
} disk_config_t;

typedef struct {
    uint32_t cylinder;
    uint32_t head;
    uint32_t sector;
} disk_chs_t;

typedef struct {
    uint64_t  lba;     /* first sector */
    uint32_t  count;   /* number of sectors, >= 1 */
    disk_op_t op;
} disk_request_t;

/* Breakdown of where the time went for one request (or one seek). */
typedef struct {
    double   seek_ms;
    double   rotate_ms;
    double   transfer_ms;
    double   total_ms;          /* seek + rotate + transfer              */
    uint64_t cylinders_moved;   /* head travel, in cylinders             */
    uint32_t extents;           /* physical pieces the request split into */
    uint32_t remapped_sectors;  /* sectors served from the spare area     */
} disk_timing_t;

typedef struct {
    uint64_t capacity_lbas;     /* user-addressable sectors              */
    uint32_t sector_size;
    uint32_t cylinders;
    uint32_t heads;
    uint32_t num_zones;
    double   rev_ms;            /* one revolution                        */
    uint64_t total_pbas;        /* every physical sector, incl. spares   */
    uint64_t spare_total;       /* sectors in the spare area             */
    uint64_t spare_used;        /* spares consumed by the G-list         */
    size_t   plist_len;         /* factory defects (slipped)             */
    size_t   glist_len;         /* grown defects (reallocated)           */
} disk_info_t;

typedef struct {
    uint32_t first_cylinder;
    uint32_t num_cylinders;
    uint32_t sectors_per_track;
} disk_zone_t;

/* ---- lifecycle ---------------------------------------------------------- */
void    disk_config_default(disk_config_t *cfg);
disk_t *disk_open(const disk_config_t *cfg, int *err);  /* err may be NULL */
void    disk_close(disk_t *d);
void    disk_get_info(const disk_t *d, disk_info_t *info);
int     disk_get_zone(const disk_t *d, uint32_t idx, disk_zone_t *out);
const char *disk_strerror(int err);

/* ---- addressing ----------------------------------------------------------
 * disk_lba_to_chs returns where the LBA's data physically lives right now:
 * after P-list slipping and, if the LBA was reallocated, in the spare area.
 * disk_chs_to_lba is the exact inverse; it fails with DISK_EDEFECT for a
 * slipped/retired sector or an unused spare. */
int disk_lba_to_chs(const disk_t *d, uint64_t lba, disk_chs_t *out);
int disk_chs_to_lba(const disk_t *d, const disk_chs_t *chs, uint64_t *lba);

/* ---- mechanics ---------------------------------------------------------- */

/* Service one request starting at now_ms. Moves the head. */
int disk_service_request(disk_t *d, const disk_request_t *req, double now_ms,
                         disk_timing_t *out);

/* Move the head to a cylinder without transferring (used by SCAN-style
 * schedulers that sweep to the edge of the disk). */
int disk_seek(disk_t *d, uint32_t cylinder, disk_timing_t *out);

/* Positioning time (seek + rotational latency) to reach the first sector of
 * `lba` if we started now. Pure: does not move the head. This is what an
 * SPTF / rotational-position-ordering scheduler needs. */
int disk_estimate_access(const disk_t *d, uint64_t lba, double now_ms,
                         double *positioning_ms);

uint32_t disk_head_cylinder(const disk_t *d);
/* Physical cylinder holding the first sector of `lba` (remap-aware). */
int disk_lba_cylinder(const disk_t *d, uint64_t lba, uint32_t *cyl);

/* ---- defect management -------------------------------------------------- */

/* Mark an LBA as a grown defect: its data is reallocated to the next free
 * spare sector. Re-adding an already remapped LBA means the spare itself
 * went bad, so it gets a fresh spare. */
int disk_add_grown_defect(disk_t *d, uint64_t lba);
int disk_is_remapped(const disk_t *d, uint64_t lba);  /* 1 / 0 */

#ifdef __cplusplus
}
#endif

#endif /* DISKSIM_H */
