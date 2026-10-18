#include "rng.h"

#include <math.h>

void rng_seed(rng_t *r, uint64_t seed)
{
    r->state = seed;
}

uint64_t rng_next(rng_t *r)
{
    /* splitmix64 (Steele, Lea, Flood 2014) */
    uint64_t z = (r->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t rng_below(rng_t *r, uint64_t n)
{
    /* Modulo bias is (n / 2^64), i.e. < 1e-10 for any disk we model:
     * not worth a rejection loop here. */
    return rng_next(r) % n;
}

double rng_unit(rng_t *r)
{
    /* top 53 bits -> exactly representable double in [0, 1) */
    return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

double rng_exp(rng_t *r, double mean)
{
    /* inverse CDF; 1 - u is in (0, 1] so log() never sees 0 */
    return -mean * log(1.0 - rng_unit(r));
}
