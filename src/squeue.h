/*
 * squeue.h - the queue type the firmware actually uses, selected at BUILD
 * time:
 *
 *     make QUEUE=lockfree   (default)  -> ring_spsc_t  (C11 atomics)
 *     make QUEUE=mutex                 -> ring_mutex_t (mutex + condvars)
 *
 * WHY build time and not a runtime function pointer: the queue sits on the
 * hottest path (two pushes and two pops per command). A static inline
 * wrapper compiles down to a direct call that the optimiser can inline; an
 * indirect call through a pointer cannot be inlined and would add its own
 * cost to the very thing we are trying to measure. Both implementations are
 * always compiled into the library, so the unit tests and the queue
 * micro-benchmark exercise both regardless of this switch.
 */
#ifndef FWSIM_SQUEUE_H
#define FWSIM_SQUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(FWSIM_QUEUE_MUTEX)

#include "ring_mutex.h"
typedef ring_mutex_t squeue_t;
#define SQUEUE_IMPL "mutex"
static inline int    sq_init(squeue_t *q, size_t cap)          { return rbm_init(q, cap); }
static inline void   sq_destroy(squeue_t *q)                   { rbm_destroy(q); }
static inline bool   sq_try_push(squeue_t *q, void *it)        { return rbm_try_push(q, it); }
static inline bool   sq_push(squeue_t *q, void *it)            { return rbm_push(q, it); }
static inline bool   sq_try_pop(squeue_t *q, void **it)        { return rbm_try_pop(q, it); }
static inline bool   sq_pop_timed(squeue_t *q, void **it, uint64_t ns) { return rbm_pop_timed(q, it, ns); }
static inline void   sq_close(squeue_t *q)                     { rbm_close(q); }
static inline size_t sq_count(squeue_t *q)                     { return rbm_count(q); }

#else

#include "ring_spsc.h"
typedef ring_spsc_t squeue_t;
#define SQUEUE_IMPL "lockfree"
static inline int    sq_init(squeue_t *q, size_t cap)          { return spsc_init(q, cap); }
static inline void   sq_destroy(squeue_t *q)                   { spsc_destroy(q); }
static inline bool   sq_try_push(squeue_t *q, void *it)        { return spsc_try_push(q, it); }
static inline bool   sq_push(squeue_t *q, void *it)            { return spsc_push(q, it); }
static inline bool   sq_try_pop(squeue_t *q, void **it)        { return spsc_try_pop(q, it); }
static inline bool   sq_pop_timed(squeue_t *q, void **it, uint64_t ns) { return spsc_pop_timed(q, it, ns); }
static inline void   sq_close(squeue_t *q)                     { spsc_close(q); }
static inline size_t sq_count(squeue_t *q)                     { return spsc_count(q); }

#endif

#endif
