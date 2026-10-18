/*
 * test_cache.c - LRU eviction order, dirty handling, and a randomized
 * comparison against a trivially correct reference model.
 */
#include "cache.h"
#include "check.h"
#include "util.h"

#define SS 16   /* tiny "sectors" keep the test readable */

static void fill(uint8_t *buf, uint8_t v)
{
    memset(buf, v, SS);
}

/* Collect the cache's LBAs in MRU -> LRU order. */
static size_t order(cache_t *c, uint64_t *out, size_t max)
{
    size_t n = 0;
    for (cache_entry_t *e = cache_mru(c); e && n < max; e = cache_next_older(c, e))
        out[n++] = e->lba;
    return n;
}

static void test_lru_order_and_eviction(void)
{
    cache_t c;
    uint8_t d[SS];
    uint64_t o[8];
    CHECK(cache_init(&c, 3, SS) == 0);
    fill(d, 1);
    cache_insert(&c, 10, d, false);
    cache_insert(&c, 20, d, false);
    cache_insert(&c, 30, d, false);
    CHECK_EQ(order(&c, o, 8), 3);
    CHECK(o[0] == 30 && o[1] == 20 && o[2] == 10);

    /* A touch moves 10 to the front: 10, 30, 20. */
    CHECK(cache_lookup(&c, 10, true) != NULL);
    order(&c, o, 8);
    CHECK(o[0] == 10 && o[1] == 30 && o[2] == 20);

    /* A non-touching lookup must NOT change the order. */
    CHECK(cache_lookup(&c, 20, false) != NULL);
    order(&c, o, 8);
    CHECK(o[0] == 10 && o[1] == 30 && o[2] == 20);

    /* Full: inserting 40 evicts the LRU entry, 20. */
    cache_insert(&c, 40, d, false);
    CHECK(cache_lookup(&c, 20, false) == NULL);
    CHECK_EQ(c.evictions, 1);
    order(&c, o, 8);
    CHECK(o[0] == 40 && o[1] == 10 && o[2] == 30);
    CHECK(cache_check(&c) == NULL);
    cache_free(&c);
}

static void test_dirty_entries_are_never_evicted(void)
{
    cache_t c;
    uint8_t d[SS];
    CHECK(cache_init(&c, 3, SS) == 0);
    fill(d, 2);
    cache_insert(&c, 1, d, true);    /* LRU, but dirty */
    cache_insert(&c, 2, d, false);
    cache_insert(&c, 3, d, false);
    CHECK_EQ(c.dirty_count, 1);

    /* The victim must be 2 (oldest CLEAN entry), not 1. */
    cache_insert(&c, 4, d, false);
    CHECK(cache_lookup(&c, 1, false) != NULL);
    CHECK(cache_lookup(&c, 2, false) == NULL);

    /* Make everything dirty: an insert of a NEW lba must fail... */
    cache_insert(&c, 3, d, true);
    cache_insert(&c, 4, d, true);
    CHECK_EQ(c.dirty_count, 3);
    CHECK(cache_insert(&c, 5, d, false) == NULL);
    CHECK(cache_lookup(&c, 5, false) == NULL);
    CHECK_EQ(c.count, 3);
    /* ...but overwriting an existing one still works. */
    CHECK(cache_insert(&c, 3, d, true) != NULL);

    /* After destaging (mark clean) one entry, it becomes the victim. */
    cache_mark_clean(&c, cache_lookup(&c, 1, false));
    CHECK_EQ(c.dirty_count, 2);
    CHECK(cache_insert(&c, 5, d, false) != NULL);
    CHECK(cache_lookup(&c, 1, false) == NULL);
    CHECK(cache_check(&c) == NULL);
    cache_free(&c);
}

static void test_overwrite_and_dirty_transitions(void)
{
    cache_t c;
    uint8_t a[SS], b[SS];
    CHECK(cache_init(&c, 4, SS) == 0);
    fill(a, 0xAA);
    fill(b, 0xBB);
    cache_insert(&c, 7, a, false);
    cache_entry_t *e = cache_insert(&c, 7, b, true);    /* write hit */
    CHECK(e != NULL);
    CHECK_EQ(c.count, 1);
    CHECK_EQ(c.dirty_count, 1);
    CHECK(e->dirty);
    CHECK(memcmp(e->data, b, SS) == 0);

    /* Clean data over dirty (what a FUA / write-through write does)
     * clears the dirty bit: the media now holds this exact data. */
    cache_insert(&c, 7, a, false);
    CHECK_EQ(c.dirty_count, 0);
    CHECK(memcmp(cache_lookup(&c, 7, false)->data, a, SS) == 0);

    cache_mark_clean(&c, e);          /* idempotent */
    CHECK_EQ(c.dirty_count, 0);
    CHECK(cache_check(&c) == NULL);
    cache_free(&c);
}

static void test_collect_dirty_oldest_first(void)
{
    cache_t c;
    uint8_t d[SS];
    cache_entry_t *out[8];
    CHECK(cache_init(&c, 8, SS) == 0);
    fill(d, 3);
    cache_insert(&c, 50, d, true);
    cache_insert(&c, 10, d, false);
    cache_insert(&c, 40, d, true);
    cache_insert(&c, 30, d, true);
    cache_lookup(&c, 50, true);       /* 50 is now the newest */
    size_t n = cache_collect_dirty(&c, out, 8);
    CHECK_EQ(n, 3);
    CHECK(out[0]->lba == 40 && out[1]->lba == 30 && out[2]->lba == 50);
    CHECK_EQ(cache_collect_dirty(&c, out, 2), 2);   /* respects max */
    cache_free(&c);
}

static void test_remove_and_discard(void)
{
    cache_t c;
    uint8_t d[SS];
    CHECK(cache_init(&c, 4, SS) == 0);
    fill(d, 4);
    for (uint64_t l = 0; l < 4; l++)
        cache_insert(&c, l, d, l % 2 == 0);
    cache_remove(&c, cache_lookup(&c, 2, false));
    CHECK_EQ(c.count, 3);
    CHECK_EQ(c.dirty_count, 1);
    CHECK(cache_check(&c) == NULL);
    cache_discard_all(&c);            /* power loss: dirty data gone too */
    CHECK_EQ(c.count, 0);
    CHECK_EQ(c.dirty_count, 0);
    CHECK(cache_mru(&c) == NULL);
    CHECK(cache_check(&c) == NULL);
    /* Fully reusable afterwards. */
    for (uint64_t l = 100; l < 104; l++)
        CHECK(cache_insert(&c, l, d, true) != NULL);
    CHECK(cache_check(&c) == NULL);
    cache_free(&c);
}

/*
 * Randomized test against a reference model: an array kept in recency
 * order (index 0 = MRU), updated with the obvious O(n) operations. After
 * every operation the real cache must hold exactly the same LBAs in
 * exactly the same order with the same dirty bits and data.
 */
#define MODEL_CAP 32
typedef struct {
    uint64_t lba;
    int      dirty;
    uint8_t  val;
} model_e;
static model_e model[MODEL_CAP];
static size_t model_n;

static int model_find(uint64_t lba)
{
    for (size_t i = 0; i < model_n; i++)
        if (model[i].lba == lba)
            return (int)i;
    return -1;
}

static void model_to_front(size_t i)
{
    model_e e = model[i];
    memmove(&model[1], &model[0], i * sizeof(model_e));
    model[0] = e;
}

/* Returns 1 if inserted, 0 if refused (everything dirty). */
static int model_insert(uint64_t lba, int dirty, uint8_t val)
{
    int i = model_find(lba);
    if (i >= 0) {
        model[i].dirty = dirty;
        model[i].val = val;
        model_to_front((size_t)i);
        return 1;
    }
    if (model_n == MODEL_CAP) {
        int victim = -1;
        for (int k = (int)model_n - 1; k >= 0; k--)
            if (!model[k].dirty) {
                victim = k;
                break;
            }
        if (victim < 0)
            return 0;
        memmove(&model[victim], &model[victim + 1],
                (model_n - (size_t)victim - 1) * sizeof(model_e));
        model_n--;
    }
    memmove(&model[1], &model[0], model_n * sizeof(model_e));
    model[0] = (model_e){ lba, dirty, val };
    model_n++;
    return 1;
}

static void test_randomized_against_model(void)
{
    cache_t c;
    CHECK(cache_init(&c, MODEL_CAP, SS) == 0);
    model_n = 0;
    uint64_t rng = 2024;
    int mismatches = 0;
    for (int step = 0; step < 50000 && mismatches == 0; step++) {
        uint64_t lba = rng_below64(&rng, 100);
        unsigned op = (unsigned)rng_below64(&rng, 10);
        uint8_t val = (uint8_t)rng_next64(&rng);
        uint8_t d[SS];
        fill(d, val);
        if (op < 5) {                          /* insert (write or fill) */
            int dirty = op < 2;
            int ok_model = model_insert(lba, dirty, val);
            int ok_real = cache_insert(&c, lba, d, dirty) != NULL;
            if (ok_model != ok_real)
                mismatches++;
        } else if (op < 8) {                   /* read access */
            int i = model_find(lba);
            cache_entry_t *e = cache_lookup(&c, lba, true);
            if ((i >= 0) != (e != NULL))
                mismatches++;
            if (i >= 0)
                model_to_front((size_t)i);
        } else {                               /* destage oldest dirty */
            cache_entry_t *out[1];
            if (cache_collect_dirty(&c, out, 1) == 1) {
                int i = model_find(out[0]->lba);
                if (i < 0 || !model[i].dirty)
                    mismatches++;
                for (int k = (int)model_n - 1; k > i; k--)  /* was it the oldest? */
                    if (model[k].dirty)
                        mismatches++;
                if (i >= 0)
                    model[i].dirty = 0;
                cache_mark_clean(&c, out[0]);
            }
        }
        /* Compare full state. */
        size_t n = 0;
        for (cache_entry_t *e = cache_mru(&c); e; e = cache_next_older(&c, e), n++) {
            if (n >= model_n || e->lba != model[n].lba ||
                e->dirty != (model[n].dirty != 0) || e->data[0] != model[n].val)
                mismatches++;
        }
        if (n != model_n)
            mismatches++;
        if (step % 1000 == 0 && cache_check(&c) != NULL)
            mismatches++;
    }
    CHECK_EQ(mismatches, 0);
    CHECK(cache_check(&c) == NULL);
    cache_free(&c);
}

/* Aligned strides (every 8th LBA) are the common case for 4 KiB I/O on
 * 512-byte sectors; the hash must still find everything. */
static void test_hash_with_strided_lbas(void)
{
    cache_t c;
    uint8_t d[SS];
    CHECK(cache_init(&c, 1000, SS) == 0);
    for (uint64_t i = 0; i < 1000; i++) {
        fill(d, (uint8_t)i);
        cache_insert(&c, i * 8 + (1ull << 40), d, false);
    }
    int found = 0;
    for (uint64_t i = 0; i < 1000; i++) {
        cache_entry_t *e = cache_lookup(&c, i * 8 + (1ull << 40), false);
        found += e && e->data[0] == (uint8_t)i;
    }
    CHECK_EQ(found, 1000);
    /* Longest chain stays short (Fibonacci hashing spreads strides). */
    size_t longest = 0;
    for (size_t b = 0; b < c.nbuckets; b++) {
        size_t len = 0;
        for (cache_entry_t *e = c.buckets[b]; e; e = e->hnext)
            len++;
        if (len > longest)
            longest = len;
    }
    CHECK(longest <= 6);
    CHECK(cache_check(&c) == NULL);
    cache_free(&c);
}

int main(void)
{
    printf("test_cache\n");
    RUN_TEST(test_lru_order_and_eviction);
    RUN_TEST(test_dirty_entries_are_never_evicted);
    RUN_TEST(test_overwrite_and_dirty_transitions);
    RUN_TEST(test_collect_dirty_oldest_first);
    RUN_TEST(test_remove_and_discard);
    RUN_TEST(test_randomized_against_model);
    RUN_TEST(test_hash_with_strided_lbas);
    return test_summary("test_cache");
}
