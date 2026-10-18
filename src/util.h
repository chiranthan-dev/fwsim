/*
 * util.h - tiny shared helpers: a deterministic PRNG and single-writer
 * statistics counters.
 */
#ifndef FWSIM_UTIL_H
#define FWSIM_UTIL_H

#include <stdatomic.h>
#include <stdint.h>

/* ---- splitmix64 ----------------------------------------------------------
 * WHY our own PRNG: rand() is not thread-safe, differs between C libraries
 * (so results would not reproduce across machines), and has a tiny
 * RAND_MAX on some platforms. splitmix64 is 3 lines, 8 bytes of state,
 * and statistically solid. Each thread owns its own state - no sharing. */
static inline uint64_t rng_next64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Uniform in [0, n). The modulo bias is < n / 2^64: irrelevant here. */
static inline uint64_t rng_below64(uint64_t *s, uint64_t n)
{
    return rng_next64(s) % n;
}

/* Uniform double in [0, 1): top 53 bits -> exactly representable. */
static inline double rng_unit64(uint64_t *s)
{
    return (double)(rng_next64(s) >> 11) * (1.0 / 9007199254740992.0);
}

/* ---- single-writer counters ----------------------------------------------
 * Statistics are written by the firmware thread and may be read by the
 * host at any time. A plain uint64_t would be a data race (undefined
 * behaviour, and TSan reports it). An atomic_fetch_add would be correct
 * but compiles to a LOCK-prefixed instruction, which is needless: there is
 * only ONE writer. So we do a relaxed load + relaxed store. That is still
 * an atomic access (no torn reads, no UB) but costs the same as a plain
 * increment. Relaxed ordering is fine because nothing else is published
 * through a counter - a reader just wants a recent value. */
typedef struct {
    _Atomic uint64_t v;
} counter_t;

static inline void ctr_add(counter_t *c, uint64_t n)
{
    atomic_store_explicit(&c->v,
                          atomic_load_explicit(&c->v, memory_order_relaxed) + n,
                          memory_order_relaxed);
}

static inline void ctr_set(counter_t *c, uint64_t n)
{
    atomic_store_explicit(&c->v, n, memory_order_relaxed);
}

static inline uint64_t ctr_get(counter_t *c)
{
    return atomic_load_explicit(&c->v, memory_order_relaxed);
}

#endif
