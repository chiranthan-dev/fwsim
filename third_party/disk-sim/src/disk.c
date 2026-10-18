/*
 * disk.c - the public media-layer API: ties geometry, defects and timing
 * together into a disk with a moving head.
 *
 * The one piece of mutable mechanical state is the head's cylinder. The
 * platter's angle is NOT state: it is computed from the caller's clock
 * (see timing.c). That is a deliberate choice: the disk cannot drift out of
 * sync with the simulator, and "what would it cost to start this request
 * now?" (disk_estimate_access) is a pure function.
 */
#include "disksim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "defects.h"
#include "geometry.h"
#include "timing.h"

struct disk {
    disk_config_t cfg;      /* copy; cfg.plist is NULLed (we own a copy) */
    geometry_t    geo;
    defects_t     def;
    double        rev_ms;
    uint32_t      head_cyl; /* where the actuator currently sits         */
};

/* A run of sectors on ONE track that can be read in one rotation pass. */
typedef struct {
    uint32_t cyl, head;
    uint32_t s_first, s_last;   /* inclusive */
    uint32_t spt;
    uint32_t remapped;          /* sectors in this run that came from spares */
} extent_t;

void disk_config_default(disk_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    /*
     * A small "desktop 7200 rpm" drive. Real 3.5" drives have ~10x more
     * tracks per surface; we scale cylinders down to keep runs fast, but
     * keep the seek curve realistic: ~0.9 ms track-to-track, ~16 ms full
     * stroke, ~8.3 ms per revolution.
     */
    cfg->cylinders = 50000;
    cfg->heads = 4;
    cfg->sectors_per_track = 1000;
    cfg->sector_size = 512;
    cfg->rpm = 7200;

    cfg->zoned = 1;
    cfg->num_zones = 16;
    cfg->spt_outer = 1300;
    cfg->spt_inner = 700;

    cfg->seek_settle_ms = 0.8;
    cfg->seek_k_ms = 0.068;   /* 0.8 + 0.068*sqrt(49999) ~= 16.0 ms full stroke */

    /* A one-cylinder seek takes 0.868 ms = 0.104 rev at 7200 rpm. Skewing
     * each cylinder by 0.12 rev means sector 0 of the next cylinder arrives
     * just after the head settles. */
    cfg->cyl_skew = 0.12;

    cfg->spare_cylinders = 200;   /* 0.4% of the surface */
}

const char *disk_strerror(int err)
{
    switch (err) {
    case DISK_OK:      return "ok";
    case DISK_EINVAL:  return "invalid argument or configuration";
    case DISK_ERANGE:  return "address out of range";
    case DISK_ENOSPC:  return "spare area exhausted";
    case DISK_EDEFECT: return "defective or unmapped sector";
    case DISK_ENOMEM:  return "out of memory";
    default:           return "unknown error";
    }
}

disk_t *disk_open(const disk_config_t *cfg, int *err)
{
    int rc = DISK_EINVAL;
    disk_t *d = NULL;

    if (!cfg || cfg->rpm == 0 ||
        (cfg->sector_size != 512 && cfg->sector_size != 4096) ||
        cfg->seek_settle_ms < 0.0 || cfg->seek_k_ms < 0.0 ||
        cfg->cyl_skew < 0.0 || cfg->cyl_skew >= 1.0 ||
        cfg->spare_cylinders >= cfg->cylinders)
        goto fail;

    d = calloc(1, sizeof(*d));
    if (!d) {
        rc = DISK_ENOMEM;
        goto fail;
    }
    d->cfg = *cfg;
    d->cfg.plist = NULL;
    d->cfg.plist_len = 0;

    rc = geometry_init(&d->geo, cfg);
    if (rc != DISK_OK)
        goto fail;

    /* Spares are the innermost `spare_cylinders` cylinders. */
    uint64_t data_pbas = geometry_cyl_start_pba(&d->geo,
                                                cfg->cylinders - cfg->spare_cylinders);
    rc = defects_init(&d->def, cfg->plist, cfg->plist_len,
                      data_pbas, d->geo.total_pbas);
    if (rc != DISK_OK)
        goto fail;

    d->rev_ms = timing_rev_ms(cfg->rpm);
    d->head_cyl = 0;
    if (err)
        *err = DISK_OK;
    return d;

fail:
    if (d) {
        defects_free(&d->def);
        free(d);
    }
    if (err)
        *err = rc;
    return NULL;
}

void disk_close(disk_t *d)
{
    if (!d)
        return;
    defects_free(&d->def);
    free(d);
}

void disk_get_info(const disk_t *d, disk_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->capacity_lbas = defects_capacity(&d->def);
    info->sector_size = d->cfg.sector_size;
    info->cylinders = d->geo.cylinders;
    info->heads = d->geo.heads;
    info->num_zones = d->geo.num_zones;
    info->rev_ms = d->rev_ms;
    info->total_pbas = d->geo.total_pbas;
    info->spare_total = d->def.spare_end - d->def.data_pbas;
    info->spare_used = d->def.spare_next - d->def.data_pbas;
    info->plist_len = d->def.plist_len;
    info->glist_len = d->def.glist_len;
}

int disk_get_zone(const disk_t *d, uint32_t idx, disk_zone_t *out)
{
    if (!d || !out || idx >= d->geo.num_zones)
        return DISK_EINVAL;
    out->first_cylinder = d->geo.zones[idx].first_cyl;
    out->num_cylinders = d->geo.zones[idx].num_cyls;
    out->sectors_per_track = d->geo.zones[idx].spt;
    return DISK_OK;
}

/* ---- addressing --------------------------------------------------------- */

int disk_lba_to_chs(const disk_t *d, uint64_t lba, disk_chs_t *out)
{
    if (!d || !out)
        return DISK_EINVAL;
    if (lba >= defects_capacity(&d->def))
        return DISK_ERANGE;
    return geometry_pba_to_chs(&d->geo, defects_lba_to_pba(&d->def, lba, NULL), out);
}

int disk_chs_to_lba(const disk_t *d, const disk_chs_t *chs, uint64_t *lba)
{
    if (!d || !chs || !lba)
        return DISK_EINVAL;
    uint64_t pba;
    int rc = geometry_chs_to_pba(&d->geo, chs, &pba);
    if (rc != DISK_OK)
        return rc;
    return defects_pba_to_lba(&d->def, pba, lba);
}

uint32_t disk_head_cylinder(const disk_t *d)
{
    return d->head_cyl;
}

int disk_lba_cylinder(const disk_t *d, uint64_t lba, uint32_t *cyl)
{
    disk_chs_t chs;
    int rc = disk_lba_to_chs(d, lba, &chs);
    if (rc == DISK_OK)
        *cyl = chs.cylinder;
    return rc;
}

int disk_is_remapped(const disk_t *d, uint64_t lba)
{
    return defects_glist_find(&d->def, lba) != NULL;
}

int disk_add_grown_defect(disk_t *d, uint64_t lba)
{
    if (!d)
        return DISK_EINVAL;
    return defects_add_grown(&d->def, lba);
}

/* ---- mechanics ---------------------------------------------------------- */

static uint64_t cyl_distance(uint32_t a, uint32_t b)
{
    return a > b ? (uint64_t)(a - b) : (uint64_t)(b - a);
}

/* Angle where sector s on cylinder `cyl` starts, including cylinder skew. */
static double sector_angle(const disk_t *d, uint32_t cyl, uint32_t s, uint32_t spt)
{
    double skew = fmod((double)cyl * d->cfg.cyl_skew, 1.0);
    return timing_sector_angle(s, spt, skew);
}

/*
 * Position for and transfer one extent, starting at *t. Advances *t and the
 * head, and accumulates the breakdown into acc.
 */
static void run_extent(disk_t *d, const extent_t *e, double *t, disk_timing_t *acc)
{
    uint64_t dist = cyl_distance(d->head_cyl, e->cyl);
    double seek = timing_seek_ms(d->cfg.seek_settle_ms, d->cfg.seek_k_ms, dist);
    *t += seek;

    /* Head switches within a cylinder are treated as free (see README:
     * limitations); only cylinder changes cost a seek. */
    double rot = timing_rot_wait_ms(*t, sector_angle(d, e->cyl, e->s_first, e->spt),
                                    d->rev_ms);
    *t += rot;

    /* Sectors between s_first and s_last that we do not need (slipped
     * P-list defects, or other spares) still pass under the head, so the
     * transfer covers the whole span. That is exactly the cost of slipping:
     * one sector time per slipped sector, no extra seek. */
    double xfer = timing_transfer_ms((uint64_t)(e->s_last - e->s_first) + 1,
                                     e->spt, d->rev_ms);
    *t += xfer;

    d->head_cyl = e->cyl;
    acc->seek_ms += seek;
    acc->rotate_ms += rot;
    acc->transfer_ms += xfer;
    acc->cylinders_moved += dist;
    acc->extents++;
    acc->remapped_sectors += e->remapped;
}

int disk_service_request(disk_t *d, const disk_request_t *req, double now_ms,
                         disk_timing_t *out)
{
    if (!d || !req || !out || req->count == 0)
        return DISK_EINVAL;
    uint64_t cap = defects_capacity(&d->def);
    if (req->lba >= cap || req->count > cap - req->lba)
        return DISK_ERANGE;

    memset(out, 0, sizeof(*out));
    double t = now_ms;

    /*
     * Walk the request sector by sector and group sectors into extents:
     * maximal runs on the same track with increasing sector numbers. A new
     * extent starts when we cross to another track or cylinder, or when a
     * reallocated LBA sends us to the spare area (and again when we come
     * back). Each extent pays its own seek + rotational latency.
     */
    extent_t cur = {0};
    int have = 0;
    for (uint32_t i = 0; i < req->count; i++) {
        int remapped;
        uint64_t pba = defects_lba_to_pba(&d->def, req->lba + i, &remapped);
        disk_chs_t chs;
        geometry_pba_to_chs(&d->geo, pba, &chs);   /* pba is always valid here */

        if (have && chs.cylinder == cur.cyl && chs.head == cur.head &&
            chs.sector > cur.s_last) {
            cur.s_last = chs.sector;
            cur.remapped += (uint32_t)remapped;
            continue;
        }
        if (have)
            run_extent(d, &cur, &t, out);
        cur.cyl = chs.cylinder;
        cur.head = chs.head;
        cur.s_first = cur.s_last = chs.sector;
        cur.spt = geometry_spt(&d->geo, chs.cylinder);
        cur.remapped = (uint32_t)remapped;
        have = 1;
    }
    run_extent(d, &cur, &t, out);

    out->total_ms = t - now_ms;
    return DISK_OK;
}

int disk_seek(disk_t *d, uint32_t cylinder, disk_timing_t *out)
{
    if (!d || !out || cylinder >= d->geo.cylinders)
        return DISK_EINVAL;
    memset(out, 0, sizeof(*out));
    uint64_t dist = cyl_distance(d->head_cyl, cylinder);
    out->seek_ms = timing_seek_ms(d->cfg.seek_settle_ms, d->cfg.seek_k_ms, dist);
    out->total_ms = out->seek_ms;
    out->cylinders_moved = dist;
    d->head_cyl = cylinder;
    return DISK_OK;
}

int disk_estimate_access(const disk_t *d, uint64_t lba, double now_ms,
                         double *positioning_ms)
{
    if (!d || !positioning_ms)
        return DISK_EINVAL;
    if (lba >= defects_capacity(&d->def))
        return DISK_ERANGE;

    disk_chs_t chs;
    geometry_pba_to_chs(&d->geo, defects_lba_to_pba(&d->def, lba, NULL), &chs);
    double seek = timing_seek_ms(d->cfg.seek_settle_ms, d->cfg.seek_k_ms,
                                 cyl_distance(d->head_cyl, chs.cylinder));
    uint32_t spt = geometry_spt(&d->geo, chs.cylinder);
    double rot = timing_rot_wait_ms(now_ms + seek,
                                    sector_angle(d, chs.cylinder, chs.sector, spt),
                                    d->rev_ms);
    *positioning_ms = seek + rot;
    return DISK_OK;
}
