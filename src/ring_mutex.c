/*
 * ring_mutex.c - mutex + condition variable bounded ring.
 *
 * Every wait is in a `while (condition not true) wait();` loop, never an
 * `if`. Two reasons: POSIX allows spurious wake-ups, and between being
 * signalled and re-acquiring the mutex another thread may have taken the
 * item. The loop re-checks the condition with the lock held.
 */
#include "ring_mutex.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

int rbm_init(ring_mutex_t *r, size_t capacity)
{
    if (capacity == 0)
        return -1;
    r->slots = calloc(capacity, sizeof(*r->slots));
    if (!r->slots)
        return -1;
    r->cap = capacity;
    r->head = 0;
    r->count = 0;
    r->closed = false;
    pthread_mutex_init(&r->mu, NULL);

    /*
     * Timed waits use CLOCK_MONOTONIC. The default for condvars is
     * CLOCK_REALTIME, where an NTP step during a wait would make the
     * firmware's idle timer fire early or hang for a long time.
     */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&r->not_empty, &ca);
    pthread_cond_init(&r->not_full, &ca);
    pthread_condattr_destroy(&ca);
    return 0;
}

void rbm_destroy(ring_mutex_t *r)
{
    pthread_cond_destroy(&r->not_empty);
    pthread_cond_destroy(&r->not_full);
    pthread_mutex_destroy(&r->mu);
    free(r->slots);
    r->slots = NULL;
}

/* Both helpers assume r->mu is held. */
static void put_locked(ring_mutex_t *r, void *item)
{
    r->slots[(r->head + r->count) % r->cap] = item;
    r->count++;
    /* Signal (not broadcast): one new item can satisfy one consumer. We
     * signal while holding the lock; glibc handles this without an extra
     * context switch ("wait morphing"), and it avoids a subtle race where
     * the ring is destroyed between unlock and signal. */
    pthread_cond_signal(&r->not_empty);
}

static void *take_locked(ring_mutex_t *r)
{
    void *item = r->slots[r->head];
    r->head = (r->head + 1) % r->cap;
    r->count--;
    pthread_cond_signal(&r->not_full);
    return item;
}

bool rbm_try_push(ring_mutex_t *r, void *item)
{
    pthread_mutex_lock(&r->mu);
    bool ok = r->count < r->cap;
    if (ok)
        put_locked(r, item);
    pthread_mutex_unlock(&r->mu);
    return ok;
}

bool rbm_try_pop(ring_mutex_t *r, void **item)
{
    pthread_mutex_lock(&r->mu);
    bool ok = r->count > 0;
    if (ok)
        *item = take_locked(r);
    pthread_mutex_unlock(&r->mu);
    return ok;
}

bool rbm_push(ring_mutex_t *r, void *item)
{
    pthread_mutex_lock(&r->mu);
    while (r->count == r->cap && !r->closed)
        pthread_cond_wait(&r->not_full, &r->mu);
    bool ok = !r->closed;
    if (ok)
        put_locked(r, item);
    pthread_mutex_unlock(&r->mu);
    return ok;
}

bool rbm_pop_timed(ring_mutex_t *r, void **item, uint64_t timeout_ns)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    uint64_t ns = (uint64_t)deadline.tv_nsec + timeout_ns;
    deadline.tv_sec += (time_t)(ns / 1000000000ull);
    deadline.tv_nsec = (long)(ns % 1000000000ull);

    pthread_mutex_lock(&r->mu);
    while (r->count == 0 && !r->closed && timeout_ns > 0) {
        if (pthread_cond_timedwait(&r->not_empty, &r->mu, &deadline) == ETIMEDOUT)
            break;
    }
    bool ok = r->count > 0;
    if (ok)
        *item = take_locked(r);
    pthread_mutex_unlock(&r->mu);
    return ok;
}

void rbm_close(ring_mutex_t *r)
{
    pthread_mutex_lock(&r->mu);
    r->closed = true;
    /* Broadcast: *every* waiter must wake up and notice the close. */
    pthread_cond_broadcast(&r->not_empty);
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->mu);
}

size_t rbm_count(ring_mutex_t *r)
{
    pthread_mutex_lock(&r->mu);
    size_t n = r->count;
    pthread_mutex_unlock(&r->mu);
    return n;
}

size_t rbm_capacity(const ring_mutex_t *r)
{
    return r->cap;
}
