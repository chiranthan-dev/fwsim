/*
 * cache.h - the drive's DRAM sector cache: fixed number of entries, LRU
 * replacement via hash map + doubly linked list, per-entry dirty bit.
 *
 * One entry caches one sector. WHY per-sector and not per-segment: real
 * drives organise cache in segments (runs of sectors) because it saves
 * metadata, but per-sector entries make LRU, dirty tracking and the tests
 * exact and easy to reason about. Destaging coalesces adjacent dirty
 * sectors back into one media write anyway (see firmware.c), so the media
 * still sees large sequential transfers.
 *
 * Data structures and their costs:
 *   - hash map (chained, power-of-two buckets, ~2 buckets per entry):
 *     lba -> entry in O(1) expected. Needed so a read or write can check
 *     "is this LBA cached?" without scanning.
 *   - doubly linked LRU list with a sentinel node: MRU at the front, LRU
 *     at the back. Moving an entry to the front on access, unlinking it,
 *     and finding the LRU end are all O(1). A singly linked list would
 *     need an O(n) walk to unlink from the middle.
 *   - entries come from a preallocated pool + free list, and sector data
 *     lives in one slab: no malloc on the I/O path (firmware does not have
 *     a general-purpose heap either), and the memory footprint is fixed.
 *
 * Eviction picks the least recently used CLEAN entry. A dirty entry is
 * never silently dropped: its data exists nowhere else. If every entry is
 * dirty, cache_insert() fails and the caller must destage first.
 *
 * Thread-safety: none, on purpose. Only the firmware thread touches the
 * cache, exactly like a single-core drive controller running its cache
 * manager. No lock means no lock overhead and no lock-order bugs.
 */
#ifndef FWSIM_CACHE_H
#define FWSIM_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cache_entry {
    uint64_t            lba;
    struct cache_entry *hnext;        /* hash chain                       */
    struct cache_entry *prev, *next;  /* LRU list (or free list via next) */
    uint8_t            *data;         /* sector_size bytes in the slab    */
    bool                dirty;
} cache_entry_t;

typedef struct {
    size_t          capacity;     /* max entries                        */
    size_t          count;        /* entries in use                     */
    size_t          dirty_count;
    uint32_t        sector_size;
    size_t          nbuckets;     /* power of two                       */
    unsigned        hash_shift;   /* 64 - log2(nbuckets)                */
    cache_entry_t **buckets;
    cache_entry_t  *pool;
    cache_entry_t  *free_list;
    cache_entry_t   lru;          /* sentinel: lru.next = MRU, lru.prev = LRU */
    uint8_t        *slab;
    uint64_t        evictions;
} cache_t;

int  cache_init(cache_t *c, size_t entries, uint32_t sector_size);  /* 0/-1 */
void cache_free(cache_t *c);

/* Find an entry. touch=true marks it most recently used (a real access);
 * touch=false is a pure lookup (used by read-ahead and tests). */
cache_entry_t *cache_lookup(cache_t *c, uint64_t lba, bool touch);

/*
 * Insert (or overwrite) the sector `lba` with `data`, as MRU.
 *   - If already cached: data is replaced and the dirty bit is set to
 *     `dirty`. Writing clean data over a dirty entry therefore clears it:
 *     that is used after a write-through / FUA write, when the media now
 *     holds exactly this data and the older dirty copy is superseded.
 *   - If not cached and the cache is full: evicts the LRU clean entry.
 *     If there is none (all dirty), returns NULL and changes nothing.
 */
cache_entry_t *cache_insert(cache_t *c, uint64_t lba, const void *data, bool dirty);

void cache_mark_clean(cache_t *c, cache_entry_t *e);
void cache_remove(cache_t *c, cache_entry_t *e);

/* Up to `max` dirty entries, oldest (least recently used) first. WHY oldest
 * first: a recently written sector is the most likely to be written again,
 * so destaging it now would waste a media write. */
size_t cache_collect_dirty(cache_t *c, cache_entry_t **out, size_t max);

/* Drop everything, dirty or not. This is what a power cut does to DRAM. */
void cache_discard_all(cache_t *c);

/* Walk the LRU list from MRU to LRU (for tests and debugging). */
cache_entry_t *cache_mru(cache_t *c);
cache_entry_t *cache_next_older(cache_t *c, cache_entry_t *e);

/* Verify every invariant (list links, counts, hash membership). Returns
 * NULL if consistent, else a description of the first violation. */
const char *cache_check(const cache_t *c);

#endif
