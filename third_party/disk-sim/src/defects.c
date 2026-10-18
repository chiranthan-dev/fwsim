/*
 * defects.c - P-list slipping and G-list reallocation.
 *
 * Data structures and why:
 *   - P-list: sorted array of PBAs. It is written once at format time and
 *     only read afterwards, so a sorted array + binary search beats a tree
 *     or hash (smaller, cache-friendly, and gives us "how many defects are
 *     below X" for free, which is exactly what slipping needs).
 *   - G-list: sorted array of (lba, spare_pba). Grown defects are rare and
 *     the list is short; insertion is O(n) memmove, lookup is O(log n) on
 *     every sector we touch. Lookups vastly outnumber inserts, so we
 *     optimise for lookup. Real firmware keeps a similar sorted table.
 *   - Spares: a single pool at the inner edge, handed out in order. Real
 *     drives often spread spares per zone to shorten the detour; that is a
 *     listed limitation and the bench shows why it would matter.
 */
#include "defects.h"

#include <stdlib.h>
#include <string.h>

#include "disksim.h"

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int defects_init(defects_t *df, const uint64_t *plist, size_t plist_len,
                 uint64_t data_pbas, uint64_t total_pbas)
{
    memset(df, 0, sizeof(*df));
    df->data_pbas = data_pbas;
    df->spare_end = total_pbas;
    df->spare_next = data_pbas;

    if (plist_len == 0)
        return DISK_OK;
    if (!plist)
        return DISK_EINVAL;

    df->plist = malloc(plist_len * sizeof(*df->plist));
    if (!df->plist)
        return DISK_ENOMEM;
    memcpy(df->plist, plist, plist_len * sizeof(*df->plist));
    qsort(df->plist, plist_len, sizeof(*df->plist), cmp_u64);

    /* Deduplicate in place, and refuse factory defects inside the spare
     * area: those are simply not offered as spares by real firmware, but
     * modelling that adds nothing here, so we keep the input strict. */
    size_t out = 0;
    for (size_t i = 0; i < plist_len; i++) {
        if (df->plist[i] >= data_pbas) {
            free(df->plist);
            df->plist = NULL;
            return DISK_EINVAL;
        }
        if (out == 0 || df->plist[out - 1] != df->plist[i])
            df->plist[out++] = df->plist[i];
    }
    df->plist_len = out;
    if (out >= data_pbas) {   /* nothing left for the user */
        free(df->plist);
        df->plist = NULL;
        return DISK_EINVAL;
    }
    return DISK_OK;
}

void defects_free(defects_t *df)
{
    free(df->plist);
    free(df->glist);
    memset(df, 0, sizeof(*df));
}

uint64_t defects_capacity(const defects_t *df)
{
    return df->data_pbas - df->plist_len;
}

size_t defects_plist_count_le(const defects_t *df, uint64_t pba)
{
    /* upper_bound: first index whose value is > pba */
    size_t lo = 0, hi = df->plist_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (df->plist[mid] <= pba)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

uint64_t defects_slip(const defects_t *df, uint64_t lba)
{
    /*
     * We want the (lba+1)-th good sector, i.e. the smallest PBA p with
     *     p == lba + (number of P-list defects <= p).
     * Start at p = lba and keep pushing p forward by the number of defects
     * we have stepped over. The function is monotone, so this fixed-point
     * iteration converges to the smallest solution, and that solution is
     * never itself a defect (if it were, p-1 would also be a solution).
     * Typically 1-2 iterations, each a binary search: O(log P).
     */
    uint64_t skipped = 0;
    for (;;) {
        size_t k = defects_plist_count_le(df, lba + skipped);
        if (k == skipped)
            return lba + skipped;
        skipped = k;
    }
}

const glist_entry_t *defects_glist_find(const defects_t *df, uint64_t lba)
{
    size_t lo = 0, hi = df->glist_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (df->glist[mid].lba < lba)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < df->glist_len && df->glist[lo].lba == lba)
        return &df->glist[lo];
    return NULL;
}

uint64_t defects_lba_to_pba(const defects_t *df, uint64_t lba, int *remapped)
{
    /* G-list wins: if the sector was reallocated, its slipped home is bad. */
    const glist_entry_t *g = defects_glist_find(df, lba);
    if (remapped)
        *remapped = (g != NULL);
    return g ? g->spare_pba : defects_slip(df, lba);
}

int defects_pba_to_lba(const defects_t *df, uint64_t pba, uint64_t *lba)
{
    if (pba >= df->spare_end)
        return DISK_ERANGE;

    if (pba >= df->data_pbas) {
        /* Spare area: only meaningful if some LBA was reallocated here. The
         * G-list is sorted by LBA, not spare PBA, so this is a linear scan;
         * reverse lookups are rare (diagnostics, tests). */
        for (size_t i = 0; i < df->glist_len; i++) {
            if (df->glist[i].spare_pba == pba) {
                *lba = df->glist[i].lba;
                return DISK_OK;
            }
        }
        return DISK_EDEFECT;
    }

    size_t below = defects_plist_count_le(df, pba);
    if (below > 0 && df->plist[below - 1] == pba)
        return DISK_EDEFECT;              /* slipped factory defect */

    uint64_t l = pba - below;
    if (defects_glist_find(df, l))
        return DISK_EDEFECT;              /* grown defect: data moved away */
    *lba = l;
    return DISK_OK;
}

int defects_add_grown(defects_t *df, uint64_t lba)
{
    if (lba >= defects_capacity(df))
        return DISK_ERANGE;
    if (df->spare_next >= df->spare_end)
        return DISK_ENOSPC;

    /* Spare that fails gets replaced: the old spare is simply retired. */
    glist_entry_t *existing = (glist_entry_t *)defects_glist_find(df, lba);
    if (existing) {
        existing->spare_pba = df->spare_next++;
        return DISK_OK;
    }

    if (df->glist_len == df->glist_cap) {
        size_t cap = df->glist_cap ? df->glist_cap * 2 : 16;
        glist_entry_t *p = realloc(df->glist, cap * sizeof(*p));
        if (!p)
            return DISK_ENOMEM;
        df->glist = p;
        df->glist_cap = cap;
    }

    /* Insertion point keeps the array sorted by LBA. */
    size_t lo = 0, hi = df->glist_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (df->glist[mid].lba < lba)
            lo = mid + 1;
        else
            hi = mid;
    }
    memmove(&df->glist[lo + 1], &df->glist[lo],
            (df->glist_len - lo) * sizeof(*df->glist));
    df->glist[lo].lba = lba;
    df->glist[lo].spare_pba = df->spare_next++;
    df->glist_len++;
    return DISK_OK;
}
