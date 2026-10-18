/*
 * ring_spsc.h - single-producer / single-consumer lock-free ring buffer
 * using C11 atomics with acquire/release ordering.
 *
 * CONTRACT: exactly one thread ever pushes and exactly one thread ever
 * pops (they may be different threads, or the same one). In fwsim the host
 * thread is the only producer of the submission queue and the firmware
 * thread its only consumer; the completion queue is the mirror image. This
 * matches real hardware: an NVMe/AHCI submission queue is written by the
 * host driver and read by the drive controller, one writer per index.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS CORRECT
 * ---------------------------------------------------------------------------
 * State: slots[cap] (cap is a power of two), and two free-running counters:
 *   tail  - number of items ever pushed.  Written ONLY by the producer.
 *   head  - number of items ever popped.  Written ONLY by the consumer.
 * Items currently in the ring = tail - head (unsigned arithmetic, so it is
 * correct even after the counters wrap around 2^64). Slot index = n & mask.
 *
 * Because each counter has a single writer, no read-modify-write (CAS,
 * fetch_add) is ever needed: a writer can load its own counter "relaxed"
 * (nobody else changes it) and publish the new value with a plain store.
 * That is what makes SPSC so much cheaper than an MPMC queue.
 *
 * Two orderings must hold:
 *
 * (1) Data is visible before the item is. Push writes slots[t] and THEN
 *     stores tail = t+1 with memory_order_release. Pop loads tail with
 *     memory_order_acquire; if it sees t+1 it is guaranteed to also see
 *     the slot write (release/acquire "synchronises-with"). Without this,
 *     the CPU or compiler could make tail visible first, and the consumer
 *     would read a stale pointer - or, worse, a fw_cmd_t whose fields are
 *     not yet written. The release also publishes everything the producer
 *     wrote *before* the push (the whole command struct), which is why the
 *     command struct needs no locks of its own.
 *
 * (2) A slot is not overwritten while it is still being read. Pop reads
 *     slots[h] and THEN stores head = h+1 with release. Push loads head
 *     with acquire before deciding a slot is free. So the consumer's read
 *     of the slot happens-before the producer's later overwrite of it.
 *
 * Full/empty need no extra flag: empty <=> tail == head, full <=>
 * tail - head == cap. (A ring that stores indices modulo cap instead has
 * to waste one slot to tell full from empty; free-running counters avoid
 * that.)
 *
 * WHY NOT `volatile`: volatile only stops the compiler from caching the
 * value in a register. It gives no atomicity and no ordering between the
 * slot write and the tail write, neither for the compiler's other
 * accesses nor for the CPU (ARM reorders stores freely; even x86 needs the
 * compiler not to reorder). C11 atomics give both. On x86 the acquire load
 * and release store compile to plain MOVs - the cost is only a compiler
 * barrier - which is exactly why this beats a mutex.
 *
 * Performance details:
 *  - head and tail live on different cache lines (64-byte padding), so the
 *    producer writing tail does not keep invalidating the consumer's line
 *    holding head ("false sharing").
 *  - Each side keeps a private cached copy of the other side's counter and
 *    re-reads the shared one only when the cached copy says full/empty.
 *    In steady state this removes most cross-core cache-line traffic.
 * ---------------------------------------------------------------------------
 */
#ifndef FWSIM_RING_SPSC_H
#define FWSIM_RING_SPSC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPSC_CACHE_LINE 64

typedef struct {
    /* --- consumer's cache line --- */
    _Atomic size_t head;          /* items popped; written by consumer only */
    size_t         cached_tail;   /* consumer's private copy of tail        */
    char           pad0[SPSC_CACHE_LINE - sizeof(size_t) * 2];

    /* --- producer's cache line --- */
    _Atomic size_t tail;          /* items pushed; written by producer only */
    size_t         cached_head;   /* producer's private copy of head        */
    char           pad1[SPSC_CACHE_LINE - sizeof(size_t) * 2];

    /* --- read-only after init --- */
    size_t         cap;           /* power of two */
    size_t         mask;          /* cap - 1      */
    void         **slots;
    atomic_int     closed;        /* set once at shutdown / power loss      */
} ring_spsc_t;

/* capacity must be a power of two >= 2. Returns 0 or -1. */
int    spsc_init(ring_spsc_t *r, size_t capacity);
void   spsc_destroy(ring_spsc_t *r);

/* Producer side only. false = full. Wait-free. */
bool   spsc_try_push(ring_spsc_t *r, void *item);
/* Consumer side only. false = empty. Wait-free. */
bool   spsc_try_pop(ring_spsc_t *r, void **item);

/* Producer: retry until there is space (spin, then yield). false if closed. */
bool   spsc_push(ring_spsc_t *r, void *item);
/* Consumer: poll for up to timeout_ns (spin briefly, then yield).
 * false on timeout or if closed and empty. */
bool   spsc_pop_timed(ring_spsc_t *r, void **item, uint64_t timeout_ns);

void   spsc_close(ring_spsc_t *r);

/* Approximate when called concurrently (a snapshot of two moving
 * counters); exact when the ring is quiescent. Any thread may call it. */
size_t spsc_count(ring_spsc_t *r);
size_t spsc_capacity(const ring_spsc_t *r);

#endif
