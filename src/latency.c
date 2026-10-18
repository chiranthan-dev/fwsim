#include "latency.h"

#include <stdlib.h>
#include <string.h>

void lat_init(lat_rec_t *r)
{
    memset(r, 0, sizeof(*r));
}

void lat_free(lat_rec_t *r)
{
    free(r->v);
    memset(r, 0, sizeof(*r));
}

int lat_add(lat_rec_t *r, uint64_t ns)
{
    if (r->n == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 1024;
        uint64_t *nv = realloc(r->v, nc * sizeof(*nv));
        if (!nv)
            return -1;
        r->v = nv;
        r->cap = nc;
    }
    r->v[r->n++] = ns;
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest rank: the smallest sample with at least p% of samples <= it. */
static double rank(const lat_rec_t *r, double p)
{
    size_t k = (size_t)((p / 100.0) * (double)r->n + 0.999999);
    if (k < 1)
        k = 1;
    if (k > r->n)
        k = r->n;
    return (double)r->v[k - 1];
}

void lat_summarize(lat_rec_t *r, lat_summary_t *s)
{
    memset(s, 0, sizeof(*s));
    if (r->n == 0)
        return;
    qsort(r->v, r->n, sizeof(*r->v), cmp_u64);
    double sum = 0.0;
    for (size_t i = 0; i < r->n; i++)
        sum += (double)r->v[i];
    s->n = r->n;
    s->avg_ns = sum / (double)r->n;
    s->p50_ns = rank(r, 50.0);
    s->p95_ns = rank(r, 95.0);
    s->p99_ns = rank(r, 99.0);
    s->max_ns = (double)r->v[r->n - 1];
}
