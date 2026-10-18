/*
 * geometry.c - zone table and PBA <-> CHS translation.
 *
 * Why zones: a platter spins at constant angular velocity, but an outer
 * track is physically longer than an inner one. Packing the same number of
 * sectors on every track (the old fixed-CHS model) wastes the outer tracks.
 * Zoned bit recording splits the surface into bands; each band gets as
 * many sectors as its innermost track can hold at the target linear
 * density. Consequence we model: the outer zones transfer more bytes per
 * revolution, so sequential throughput is higher at low LBAs.
 *
 * Why a zone table instead of a formula: once SPT varies, "LBA = (c*H + h)*S
 * + s" no longer holds. Real firmware keeps a small zone table and searches
 * it; we do the same. The table is tiny (<= DISK_MAX_ZONES entries) so a
 * linear or binary search costs nothing compared to a millisecond seek.
 */
#include "geometry.h"

int geometry_init(geometry_t *g, const disk_config_t *cfg)
{
    if (!g || !cfg || cfg->cylinders == 0 || cfg->heads == 0)
        return DISK_EINVAL;

    g->cylinders = cfg->cylinders;
    g->heads = cfg->heads;

    if (!cfg->zoned) {
        if (cfg->sectors_per_track == 0)
            return DISK_EINVAL;
        g->num_zones = 1;
        g->zones[0].first_cyl = 0;
        g->zones[0].num_cyls = cfg->cylinders;
        g->zones[0].spt = cfg->sectors_per_track;
    } else {
        uint32_t nz = cfg->num_zones;
        if (nz == 0 || nz > DISK_MAX_ZONES || nz > cfg->cylinders)
            return DISK_EINVAL;
        if (cfg->spt_outer == 0 || cfg->spt_inner == 0 ||
            cfg->spt_inner > cfg->spt_outer)
            return DISK_EINVAL;   /* outer tracks are longer, never sparser */

        /* Equal-width bands; the last zone absorbs the remainder. SPT steps
         * down linearly from outer to inner, which approximates how track
         * length shrinks linearly with radius. */
        uint32_t per = cfg->cylinders / nz;
        g->num_zones = nz;
        for (uint32_t i = 0; i < nz; i++) {
            zone_t *z = &g->zones[i];
            z->first_cyl = i * per;
            z->num_cyls = (i == nz - 1) ? cfg->cylinders - i * per : per;
            if (nz == 1) {
                z->spt = cfg->spt_outer;
            } else {
                uint32_t drop = cfg->spt_outer - cfg->spt_inner;
                /* integer interpolation, exact at both ends */
                z->spt = cfg->spt_outer - (uint32_t)(((uint64_t)drop * i) / (nz - 1));
            }
        }
    }

    /* Prefix sums give each zone its starting PBA, so PBA -> zone is a
     * search over first_pba and CHS -> PBA is one multiply-add. */
    uint64_t pba = 0;
    for (uint32_t i = 0; i < g->num_zones; i++) {
        zone_t *z = &g->zones[i];
        z->first_pba = pba;
        pba += (uint64_t)z->num_cyls * g->heads * z->spt;
    }
    g->total_pbas = pba;
    return DISK_OK;
}

uint32_t geometry_zone_of_cyl(const geometry_t *g, uint32_t cyl)
{
    /* Binary search for the last zone whose first_cyl <= cyl. */
    uint32_t lo = 0, hi = g->num_zones - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (g->zones[mid].first_cyl <= cyl)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

uint32_t geometry_spt(const geometry_t *g, uint32_t cyl)
{
    return g->zones[geometry_zone_of_cyl(g, cyl)].spt;
}

uint64_t geometry_cyl_start_pba(const geometry_t *g, uint32_t cyl)
{
    if (cyl >= g->cylinders)
        return g->total_pbas;
    const zone_t *z = &g->zones[geometry_zone_of_cyl(g, cyl)];
    return z->first_pba + (uint64_t)(cyl - z->first_cyl) * g->heads * z->spt;
}

int geometry_pba_to_chs(const geometry_t *g, uint64_t pba, disk_chs_t *out)
{
    if (pba >= g->total_pbas)
        return DISK_ERANGE;

    uint32_t lo = 0, hi = g->num_zones - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (g->zones[mid].first_pba <= pba)
            lo = mid;
        else
            hi = mid - 1;
    }
    const zone_t *z = &g->zones[lo];
    uint64_t off = pba - z->first_pba;
    uint64_t track = off / z->spt;          /* track index within the zone */
    out->cylinder = z->first_cyl + (uint32_t)(track / g->heads);
    out->head = (uint32_t)(track % g->heads);
    out->sector = (uint32_t)(off % z->spt);
    return DISK_OK;
}

int geometry_chs_to_pba(const geometry_t *g, const disk_chs_t *chs, uint64_t *pba)
{
    if (chs->cylinder >= g->cylinders || chs->head >= g->heads)
        return DISK_ERANGE;
    const zone_t *z = &g->zones[geometry_zone_of_cyl(g, chs->cylinder)];
    if (chs->sector >= z->spt)
        return DISK_ERANGE;
    *pba = z->first_pba
         + ((uint64_t)(chs->cylinder - z->first_cyl) * g->heads + chs->head) * z->spt
         + chs->sector;
    return DISK_OK;
}
