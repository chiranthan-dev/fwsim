/*
 * defects.h - P-list slipping and G-list reallocation (internal).
 *
 * Two defect lists, two very different strategies, and the difference is
 * a classic interview topic:
 *
 *   P-list (primary / factory list): defects found when the drive is
 *   formatted at the factory. They are SLIPPED: LBAs are simply assigned to
 *   the next good physical sector, so the defect costs one sector-time when
 *   the head passes over it and LBA order still equals physical order.
 *
 *   G-list (grown list): sectors that go bad in the field. Slipping would
 *   shift every following LBA (i.e. move user data), so instead the single
 *   bad LBA is REALLOCATED to a spare sector somewhere else. Cheap to do,
 *   but a sequential read that hits it must seek to the spare and back.
 */
#ifndef DISKSIM_DEFECTS_H
#define DISKSIM_DEFECTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t lba;        /* user address that was reallocated */
    uint64_t spare_pba;  /* where its data lives now          */
} glist_entry_t;

typedef struct {
    uint64_t      *plist;      /* sorted, unique PBAs in the data area      */
    size_t         plist_len;
    glist_entry_t *glist;      /* sorted by lba -> binary search on access  */
    size_t         glist_len;
    size_t         glist_cap;
    uint64_t       data_pbas;  /* PBAs [0, data_pbas) hold user data        */
    uint64_t       spare_end;  /* PBAs [data_pbas, spare_end) are spares    */
    uint64_t       spare_next; /* next spare to hand out                    */
} defects_t;

int  defects_init(defects_t *df, const uint64_t *plist, size_t plist_len,
                  uint64_t data_pbas, uint64_t total_pbas);
void defects_free(defects_t *df);

/* User-visible sectors: data area minus slipped sectors. */
uint64_t defects_capacity(const defects_t *df);

/* Number of P-list entries <= pba. */
size_t defects_plist_count_le(const defects_t *df, uint64_t pba);

/* LBA -> PBA through P-list slipping only (where the LBA "should" live). */
uint64_t defects_slip(const defects_t *df, uint64_t lba);

/* Full translation: G-list first, then slipping. *remapped set to 0/1. */
uint64_t defects_lba_to_pba(const defects_t *df, uint64_t lba, int *remapped);

/* Inverse of defects_lba_to_pba. DISK_EDEFECT if no LBA lives there. */
int defects_pba_to_lba(const defects_t *df, uint64_t pba, uint64_t *lba);

const glist_entry_t *defects_glist_find(const defects_t *df, uint64_t lba);

/* Reallocate `lba` to the next spare. DISK_ENOSPC when spares run out. */
int defects_add_grown(defects_t *df, uint64_t lba);

#endif
