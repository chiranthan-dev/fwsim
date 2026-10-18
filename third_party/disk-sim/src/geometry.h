/*
 * geometry.h - physical layout of the platters (internal).
 *
 * Two address spaces exist in a drive and it matters to keep them apart:
 *   PBA (physical block address): every sector that physically exists,
 *       numbered outer-to-inner, cylinder-major, then head, then sector.
 *       Includes defective sectors and the spare area.
 *   LBA (logical block address): what the host sees. It is a PBA with the
 *       factory defects squeezed out and the spare area hidden.
 * This module only knows about PBAs and CHS. LBA <-> PBA lives in defects.c.
 */
#ifndef DISKSIM_GEOMETRY_H
#define DISKSIM_GEOMETRY_H

#include <stdint.h>
#include "disksim.h"

/* One recording zone: a band of cylinders that share a sectors-per-track. */
typedef struct {
    uint32_t first_cyl;
    uint32_t num_cyls;
    uint32_t spt;
    uint64_t first_pba;   /* PBA of (first_cyl, head 0, sector 0) */
} zone_t;

typedef struct {
    uint32_t cylinders;
    uint32_t heads;
    uint32_t num_zones;
    zone_t   zones[DISK_MAX_ZONES];
    uint64_t total_pbas;
} geometry_t;

/* Build the zone table from a config. Returns DISK_OK or DISK_EINVAL. */
int geometry_init(geometry_t *g, const disk_config_t *cfg);

/* Zone index that contains cylinder `cyl` (cyl must be < cylinders). */
uint32_t geometry_zone_of_cyl(const geometry_t *g, uint32_t cyl);

/* Sectors per track on a given cylinder. */
uint32_t geometry_spt(const geometry_t *g, uint32_t cyl);

/* PBA of the first sector of cylinder `cyl` (cyl may equal cylinders,
 * which returns total_pbas: handy for "everything before cylinder X"). */
uint64_t geometry_cyl_start_pba(const geometry_t *g, uint32_t cyl);

int geometry_pba_to_chs(const geometry_t *g, uint64_t pba, disk_chs_t *out);
int geometry_chs_to_pba(const geometry_t *g, const disk_chs_t *chs, uint64_t *pba);

#endif
