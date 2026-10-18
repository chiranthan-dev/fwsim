/*
 * ring_spsc.c - lock-free SPSC ring. The correctness argument is in
 * ring_spsc.h; comments here point at which rule each line implements.
 */
#include "ring_spsc.h"

#include <sched.h>
#include <stdlib.h>

#include "clock.h"

/* Spin this many times before falling back to sched_yield(). Spinning is
 * cheapest when the other side will respond within a microsecond or so;
 * yielding avoids burning a whole core when it will not. */
#define SPSC_SPIN_LIMIT 256

int spsc_init(ring_spsc_t *r, size_t capacity)
{
    if (capacity < 2 || (capacity & (capacity - 1)) != 0)
        return -1;
    r->slots = calloc(capacity, sizeof(*r->slots));
    if (!r->slots)
        return -1;
    r->cap = capacity;
    r->mask = capacity - 1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    r->cached_head = 0;
    r->cached_tail = 0;
    atomic_init(&r->closed, 0);
    return 0;
}

void spsc_destroy(ring_spsc_t *r)
{
    free(r->slots);
    r->slots = NULL;
}

bool spsc_try_push(ring_spsc_t *r, void *item)
{
    /* Only we write tail, so a relaxed load of our own counter is exact. */
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

    if (tail - r->cached_head == r->cap) {
        /* Looks full: refresh our view of head. acquire pairs with the
         * consumer's release in spsc_try_pop -> rule (2): its read of the
         * slot we are about to reuse has completed. */
        r->cached_head = atomic_load_explicit(&r->head, memory_order_acquire);
        if (tail - r->cached_head == r->cap)
            return false;
    }

    r->slots[tail & r->mask] = item;
    /* release -> rule (1): the slot write (and everything we wrote to the
     * command before pushing it) is visible before the new tail is. */
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return true;
}

bool spsc_try_pop(ring_spsc_t *r, void **item)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);

    if (head == r->cached_tail) {
        /* Looks empty: refresh. acquire pairs with the producer's release
         * -> rule (1): if we see the new tail, we see the slot contents. */
        r->cached_tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        if (head == r->cached_tail)
            return false;
    }

    *item = r->slots[head & r->mask];
    /* release -> rule (2): our read of the slot happens-before the
     * producer, after its acquire load of head, overwrites it. */
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return true;
}

static bool is_closed(ring_spsc_t *r)
{
    return atomic_load_explicit(&r->closed, memory_order_acquire) != 0;
}

bool spsc_push(ring_spsc_t *r, void *item)
{
    for (unsigned spins = 0;; spins++) {
        if (is_closed(r))
            return false;
        if (spsc_try_push(r, item))
            return true;
        if (spins < SPSC_SPIN_LIMIT)
            cpu_relax();
        else
            sched_yield();
    }
}

bool spsc_pop_timed(ring_spsc_t *r, void **item, uint64_t timeout_ns)
{
    if (spsc_try_pop(r, item))
        return true;
    if (timeout_ns == 0)
        return false;

    uint64_t deadline = now_ns() + timeout_ns;
    for (unsigned spins = 0;; spins++) {
        if (spsc_try_pop(r, item))
            return true;
        /* Closed: drain what is left first (handled above), then stop. */
        if (is_closed(r))
            return false;
        if (spins < SPSC_SPIN_LIMIT) {
            cpu_relax();
        } else {
            if (now_ns() >= deadline)
                return false;
            sched_yield();
        }
    }
}

void spsc_close(ring_spsc_t *r)
{
    atomic_store_explicit(&r->closed, 1, memory_order_release);
}

size_t spsc_count(ring_spsc_t *r)
{
    /* Load head first: tail only grows, so reading it second can only
     * over-estimate, never produce head > tail (which would underflow). */
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return tail - head;
}

size_t spsc_capacity(const ring_spsc_t *r)
{
    return r->cap;
}
