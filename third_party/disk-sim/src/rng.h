/*
 * rng.h - small deterministic PRNG (splitmix64).
 *
 * Why not rand(): its algorithm and RAND_MAX differ between C libraries,
 * so the same seed gives different workloads on Linux, macOS and Windows.
 * Benchmarks must be reproducible bit-for-bit, so we carry our own 10-line
 * generator. splitmix64 is fast, passes BigCrush, and needs 8 bytes of state.
 */
#ifndef DISKSIM_RNG_H
#define DISKSIM_RNG_H

#include <stdint.h>

typedef struct {
    uint64_t state;
} rng_t;

void     rng_seed(rng_t *r, uint64_t seed);
uint64_t rng_next(rng_t *r);
/* Uniform integer in [0, n). n must be > 0. */
uint64_t rng_below(rng_t *r, uint64_t n);
/* Uniform double in [0, 1). */
double   rng_unit(rng_t *r);
/* Exponential with the given mean (inter-arrival times of a Poisson process). */
double   rng_exp(rng_t *r, double mean);

#endif
