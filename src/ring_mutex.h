/*
 * ring_mutex.h - bounded ring buffer protected by a mutex + two condition
 * variables. The "textbook" blocking queue, and the baseline the lock-free
 * SPSC ring (ring_spsc.h) is measured against.
 *
 * Items are opaque pointers (fwsim queues fw_cmd_t *). Passing a pointer
 * through the queue *transfers ownership* of the command: after a push the
 * producer must not touch the command until it comes back. That rule is
 * what lets the command struct itself be lock-free (see fwsim.h).
 *
 * WHY two condition variables (not_empty, not_full) instead of one: a
 * consumer waiting for data and a producer waiting for space are waiting
 * for different conditions. With one shared condvar, a signal meant for a
 * producer could wake another consumer, which finds nothing and sleeps
 * again - a lost wake-up unless we broadcast every time.
 *
 * Unlike the SPSC ring, this one is safe with any number of producers and
 * consumers, and its capacity need not be a power of two.
 */
#ifndef FWSIM_RING_MUTEX_H
#define FWSIM_RING_MUTEX_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void          **slots;
    size_t          cap;
    size_t          head;    /* index of the oldest item           */
    size_t          count;   /* items currently stored             */
    bool            closed;  /* set by rbm_close(): wake everyone  */
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} ring_mutex_t;

int    rbm_init(ring_mutex_t *r, size_t capacity);   /* 0 or -1 */
void   rbm_destroy(ring_mutex_t *r);

/* Non-blocking. false = full / empty. */
bool   rbm_try_push(ring_mutex_t *r, void *item);
bool   rbm_try_pop(ring_mutex_t *r, void **item);

/* Blocking push: waits while full. false only if the ring was closed. */
bool   rbm_push(ring_mutex_t *r, void *item);

/* Wait up to timeout_ns for an item. false on timeout, or if closed and
 * empty. timeout_ns == 0 behaves like rbm_try_pop. */
bool   rbm_pop_timed(ring_mutex_t *r, void **item, uint64_t timeout_ns);

/* Wake all waiters; later blocking calls return immediately. Items already
 * queued can still be popped. Used for shutdown and power loss. */
void   rbm_close(ring_mutex_t *r);

size_t rbm_count(ring_mutex_t *r);
size_t rbm_capacity(const ring_mutex_t *r);

#endif
