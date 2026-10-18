/*
 * cache.c - LRU sector cache. Design rationale in cache.h.
 */
#include "cache.h"

#include <stdlib.h>
#include <string.h>

/*
 * Fibonacci hashing: multiply by 2^64 / golden ratio and keep the top bits.
 * WHY: LBAs are extremely regular (sequential runs, 4 KiB-aligned strides).
 * `lba % nbuckets` would put every 8th sector of an aligned workload into
 * the same few buckets. The multiplication mixes all input bits into the
 * top bits, so strided keys spread evenly.
 */
static size_t bucket_of(const cache_t *c, uint64_t lba)
{
    return (size_t)((lba * 0x9E3779B97F4A7C15ull) >> c->hash_shift);
}

/* ---- LRU list primitives (sentinel makes every case branch-free) ------- */

static void list_unlink(cache_entry_t *e)
{
    e->prev->next = e->next;
    e->next->prev = e->prev;
    e->prev = e->next = NULL;
}

static void list_push_front(cache_t *c, cache_entry_t *e)
{
    e->next = c->lru.next;
    e->prev = &c->lru;
    c->lru.next->prev = e;
    c->lru.next = e;
}

/* ---- hash chain primitives --------------------------------------------- */

static void hash_insert(cache_t *c, cache_entry_t *e)
{
    size_t b = bucket_of(c, e->lba);
    e->hnext = c->buckets[b];
    c->buckets[b] = e;
}

static void hash_remove(cache_t *c, cache_entry_t *e)
{
    cache_entry_t **pp = &c->buckets[bucket_of(c, e->lba)];
    while (*pp != e)
        pp = &(*pp)->hnext;   /* e is guaranteed to be in this chain */
    *pp = e->hnext;
    e->hnext = NULL;
}

/* ---- lifecycle ---------------------------------------------------------- */

int cache_init(cache_t *c, size_t entries, uint32_t sector_size)
{
    memset(c, 0, sizeof(*c));
    if (entries == 0 || sector_size == 0)
        return -1;

    /* ~2 buckets per entry keeps chains at ~0.5 entries on average. */
    size_t nb = 1;
    unsigned bits = 0;
    while (nb < entries * 2) {
        nb <<= 1;
        bits++;
    }
    if (bits == 0) {      /* need at least one bit for the shift below */
        nb = 2;
        bits = 1;
    }

    c->capacity = entries;
    c->sector_size = sector_size;
    c->nbuckets = nb;
    c->hash_shift = 64u - bits;
    c->buckets = calloc(nb, sizeof(*c->buckets));
    c->pool = calloc(entries, sizeof(*c->pool));
    c->slab = malloc(entries * (size_t)sector_size);
    if (!c->buckets || !c->pool || !c->slab) {
        cache_free(c);
        return -1;
    }

    c->lru.next = c->lru.prev = &c->lru;
    for (size_t i = 0; i < entries; i++) {
        c->pool[i].data = c->slab + i * (size_t)sector_size;
        c->pool[i].next = c->free_list;
        c->free_list = &c->pool[i];
    }
    return 0;
}

void cache_free(cache_t *c)
{
    free(c->buckets);
    free(c->pool);
    free(c->slab);
    memset(c, 0, sizeof(*c));
}

/* ---- operations --------------------------------------------------------- */

cache_entry_t *cache_lookup(cache_t *c, uint64_t lba, bool touch)
{
    for (cache_entry_t *e = c->buckets[bucket_of(c, lba)]; e; e = e->hnext) {
        if (e->lba == lba) {
            if (touch && c->lru.next != e) {
                list_unlink(e);
                list_push_front(c, e);
            }
            return e;
        }
    }
    return NULL;
}

void cache_remove(cache_t *c, cache_entry_t *e)
{
    hash_remove(c, e);
    list_unlink(e);
    if (e->dirty)
        c->dirty_count--;
    e->dirty = false;
    e->next = c->free_list;
    c->free_list = e;
    c->count--;
}

/*
 * Victim = least recently used clean entry: walk from the LRU end past any
 * dirty entries. This walk is O(number of dirty entries at the cold end);
 * the destage watermarks keep that bounded. (The alternative - separate
 * clean and dirty lists - makes eviction O(1) but loses a single global
 * recency order, which complicates "destage the oldest dirty first".)
 */
static cache_entry_t *find_victim(cache_t *c)
{
    for (cache_entry_t *e = c->lru.prev; e != &c->lru; e = e->prev)
        if (!e->dirty)
            return e;
    return NULL;
}

cache_entry_t *cache_insert(cache_t *c, uint64_t lba, const void *data, bool dirty)
{
    cache_entry_t *e = cache_lookup(c, lba, true);
    if (e) {
        memcpy(e->data, data, c->sector_size);
        if (dirty != e->dirty) {
            e->dirty = dirty;
            if (dirty)
                c->dirty_count++;
            else
                c->dirty_count--;
        }
        return e;
    }

    if (!c->free_list) {
        cache_entry_t *victim = find_victim(c);
        if (!victim)
            return NULL;            /* everything is dirty: caller destages */
        cache_remove(c, victim);
        c->evictions++;
    }

    e = c->free_list;
    c->free_list = e->next;
    e->lba = lba;
    e->dirty = dirty;
    memcpy(e->data, data, c->sector_size);
    hash_insert(c, e);
    list_push_front(c, e);
    c->count++;
    if (dirty)
        c->dirty_count++;
    return e;
}

void cache_mark_clean(cache_t *c, cache_entry_t *e)
{
    if (e->dirty) {
        e->dirty = false;
        c->dirty_count--;
    }
}

size_t cache_collect_dirty(cache_t *c, cache_entry_t **out, size_t max)
{
    size_t n = 0;
    for (cache_entry_t *e = c->lru.prev; e != &c->lru && n < max; e = e->prev)
        if (e->dirty)
            out[n++] = e;
    return n;
}

void cache_discard_all(cache_t *c)
{
    while (c->lru.next != &c->lru)
        cache_remove(c, c->lru.next);
}

cache_entry_t *cache_mru(cache_t *c)
{
    return c->lru.next == &c->lru ? NULL : c->lru.next;
}

cache_entry_t *cache_next_older(cache_t *c, cache_entry_t *e)
{
    return e->next == &c->lru ? NULL : e->next;
}

const char *cache_check(const cache_t *c)
{
    size_t n = 0, dirty = 0;
    const cache_entry_t *prev = &c->lru;
    for (const cache_entry_t *e = c->lru.next; e != &c->lru; e = e->next) {
        if (e->prev != prev)
            return "LRU list back-link broken";
        if (++n > c->capacity)
            return "LRU list longer than capacity (cycle?)";
        if (e->dirty)
            dirty++;
        /* Must be findable through the hash map, exactly once. */
        size_t hits = 0;
        for (const cache_entry_t *h = c->buckets[bucket_of(c, e->lba)]; h; h = h->hnext)
            if (h->lba == e->lba)
                hits++;
        if (hits != 1)
            return "entry not in hash map exactly once";
        prev = e;
    }
    if (c->lru.prev != prev)
        return "LRU tail pointer wrong";
    if (n != c->count)
        return "count does not match list length";
    if (dirty != c->dirty_count)
        return "dirty_count does not match dirty entries";

    size_t hashed = 0;
    for (size_t b = 0; b < c->nbuckets; b++)
        for (const cache_entry_t *h = c->buckets[b]; h; h = h->hnext)
            hashed++;
    if (hashed != c->count)
        return "hash map holds a different number of entries";

    size_t free_n = 0;
    for (const cache_entry_t *f = c->free_list; f; f = f->next)
        if (++free_n > c->capacity)
            return "free list cycle";
    if (free_n + c->count != c->capacity)
        return "entries leaked (free + used != capacity)";
    return NULL;
}
