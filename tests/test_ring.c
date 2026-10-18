/*
 * test_ring.c - both ring buffers: empty, full, FIFO order, wrap-around,
 * timeouts, close, and real producer/consumer threads (which is what
 * `make tsan` checks for data races and ordering bugs).
 */
#include <pthread.h>
#include <stdint.h>

#include "check.h"
#include "clock.h"
#include "ring_mutex.h"
#include "ring_spsc.h"

/* Items are pointers; encode small integers (offset by 1 so 0 != NULL). */
#define ITEM(i) ((void *)(uintptr_t)((i) + 1))
#define VAL(p)  ((uintptr_t)(p) - 1)

/* Under TSan everything runs ~10x slower; keep the threaded runs sane. */
#if defined(__SANITIZE_THREAD__)
#define THREAD_ITEMS 100000u
#else
#define THREAD_ITEMS 2000000u
#endif

/* ---- mutex ring --------------------------------------------------------- */

static void test_mutex_init_rejects_zero(void)
{
    ring_mutex_t r;
    CHECK(rbm_init(&r, 0) != 0);
}

static void test_mutex_empty_full_order(void)
{
    ring_mutex_t r;
    void *it;
    CHECK(rbm_init(&r, 5) == 0);          /* not a power of two: allowed */
    CHECK(!rbm_try_pop(&r, &it));         /* empty */
    for (int i = 0; i < 5; i++)
        CHECK(rbm_try_push(&r, ITEM(i)));
    CHECK(!rbm_try_push(&r, ITEM(99)));   /* full */
    CHECK_EQ(rbm_count(&r), 5);
    for (int i = 0; i < 5; i++) {
        CHECK(rbm_try_pop(&r, &it));
        CHECK_EQ(VAL(it), i);             /* FIFO */
    }
    CHECK(!rbm_try_pop(&r, &it));
    CHECK_EQ(rbm_count(&r), 0);
    rbm_destroy(&r);
}

static void test_mutex_wraparound(void)
{
    ring_mutex_t r;
    void *it;
    CHECK(rbm_init(&r, 3) == 0);
    unsigned next_in = 0, next_out = 0;
    /* Push 2 / pop 2 repeatedly: head wraps the 3-slot array many times
     * and lands on every slot as the start position. */
    for (int round = 0; round < 1000; round++) {
        CHECK(rbm_try_push(&r, ITEM(next_in++)));
        CHECK(rbm_try_push(&r, ITEM(next_in++)));
        CHECK(rbm_try_pop(&r, &it));
        CHECK_EQ(VAL(it), next_out++);
        CHECK(rbm_try_pop(&r, &it));
        CHECK_EQ(VAL(it), next_out++);
    }
    /* Fill completely while the head is mid-array, then drain. */
    for (int i = 0; i < 3; i++)
        CHECK(rbm_try_push(&r, ITEM(next_in++)));
    CHECK(!rbm_try_push(&r, ITEM(0)));
    for (int i = 0; i < 3; i++) {
        CHECK(rbm_try_pop(&r, &it));
        CHECK_EQ(VAL(it), next_out++);
    }
    rbm_destroy(&r);
}

static void test_mutex_timeout(void)
{
    ring_mutex_t r;
    void *it;
    CHECK(rbm_init(&r, 2) == 0);
    uint64_t t0 = now_ns();
    CHECK(!rbm_pop_timed(&r, &it, 20 * 1000000ull));   /* 20 ms */
    uint64_t waited = now_ns() - t0;
    CHECK(waited >= 19 * 1000000ull);                  /* really waited */
    CHECK(rbm_try_push(&r, ITEM(7)));
    CHECK(rbm_pop_timed(&r, &it, 1000000));
    CHECK_EQ(VAL(it), 7);
    rbm_destroy(&r);
}

static void *blocked_popper(void *arg)
{
    void *it;
    /* Would wait 10 s; rbm_close must wake it long before. */
    return rbm_pop_timed(arg, &it, 10ull * 1000000000ull) ? ITEM(1) : NULL;
}

static void test_mutex_close_wakes_waiter(void)
{
    ring_mutex_t r;
    CHECK(rbm_init(&r, 2) == 0);
    pthread_t t;
    uint64_t t0 = now_ns();
    pthread_create(&t, NULL, blocked_popper, &r);
    struct timespec ts = { 0, 20 * 1000000 };
    nanosleep(&ts, NULL);
    rbm_close(&r);
    void *ret;
    pthread_join(t, &ret);
    CHECK(ret == NULL);
    CHECK(now_ns() - t0 < 2000000000ull);
    CHECK(!rbm_push(&r, ITEM(1)));         /* closed: refuses */
    rbm_destroy(&r);
}

/* Many producers and consumers: the mutex ring is MPMC-safe. */
#define MP_PRODUCERS 4
#define MP_CONSUMERS 4
#define MP_PER_PRODUCER (THREAD_ITEMS / 16)

typedef struct {
    ring_mutex_t *r;
    unsigned      id;
    uint64_t      sum, count;
} mp_arg_t;

static void *mp_producer(void *arg)
{
    mp_arg_t *a = arg;
    for (uint64_t i = 0; i < MP_PER_PRODUCER; i++)
        rbm_push(a->r, ITEM(a->id * MP_PER_PRODUCER + i));
    return NULL;
}

static void *mp_consumer(void *arg)
{
    mp_arg_t *a = arg;
    void *it;
    /* Runs until the ring is closed AND drained. */
    while (rbm_pop_timed(a->r, &it, 5ull * 1000000000ull)) {
        a->sum += VAL(it);
        a->count++;
    }
    return NULL;
}

static void test_mutex_mpmc_threads(void)
{
    ring_mutex_t r;
    CHECK(rbm_init(&r, 64) == 0);
    pthread_t p[MP_PRODUCERS], c[MP_CONSUMERS];
    mp_arg_t pa[MP_PRODUCERS], ca[MP_CONSUMERS];
    for (unsigned i = 0; i < MP_CONSUMERS; i++) {
        ca[i] = (mp_arg_t){ &r, i, 0, 0 };
        pthread_create(&c[i], NULL, mp_consumer, &ca[i]);
    }
    for (unsigned i = 0; i < MP_PRODUCERS; i++) {
        pa[i] = (mp_arg_t){ &r, i, 0, 0 };
        pthread_create(&p[i], NULL, mp_producer, &pa[i]);
    }
    for (unsigned i = 0; i < MP_PRODUCERS; i++)
        pthread_join(p[i], NULL);
    rbm_close(&r);                         /* consumers exit once drained */
    for (unsigned i = 0; i < MP_CONSUMERS; i++)
        pthread_join(c[i], NULL);
    uint64_t n = (uint64_t)MP_PRODUCERS * MP_PER_PRODUCER, sum = 0, count = 0;
    for (unsigned i = 0; i < MP_CONSUMERS; i++) {
        sum += ca[i].sum;
        count += ca[i].count;
    }
    CHECK_EQ(count, n);                    /* nothing lost or duplicated */
    CHECK_EQ(sum, n * (n - 1) / 2);
    rbm_destroy(&r);
}

/* ---- lock-free SPSC ring -------------------------------------------------- */

static void test_spsc_init_rejects_bad_capacity(void)
{
    ring_spsc_t r;
    CHECK(spsc_init(&r, 0) != 0);
    CHECK(spsc_init(&r, 1) != 0);
    CHECK(spsc_init(&r, 6) != 0);          /* must be a power of two */
    CHECK(spsc_init(&r, 8) == 0);
    spsc_destroy(&r);
}

static void test_spsc_empty_full_order(void)
{
    ring_spsc_t r;
    void *it;
    CHECK(spsc_init(&r, 8) == 0);
    CHECK(!spsc_try_pop(&r, &it));
    for (int i = 0; i < 8; i++)
        CHECK(spsc_try_push(&r, ITEM(i)));
    /* All 8 slots usable: free-running counters need no wasted slot. */
    CHECK(!spsc_try_push(&r, ITEM(99)));
    CHECK_EQ(spsc_count(&r), 8);
    for (int i = 0; i < 8; i++) {
        CHECK(spsc_try_pop(&r, &it));
        CHECK_EQ(VAL(it), i);
    }
    CHECK(!spsc_try_pop(&r, &it));
    CHECK_EQ(spsc_count(&r), 0);
    spsc_destroy(&r);
}

static void test_spsc_wraparound(void)
{
    ring_spsc_t r;
    void *it;
    CHECK(spsc_init(&r, 4) == 0);
    unsigned in = 0, out = 0;
    for (int round = 0; round < 1000; round++) {
        for (int k = 0; k < 3; k++)
            CHECK(spsc_try_push(&r, ITEM(in++)));
        for (int k = 0; k < 3; k++) {
            CHECK(spsc_try_pop(&r, &it));
            CHECK_EQ(VAL(it), out++);
        }
    }
    for (int k = 0; k < 4; k++)
        CHECK(spsc_try_push(&r, ITEM(in++)));
    CHECK(!spsc_try_push(&r, ITEM(0)));
    for (int k = 0; k < 4; k++) {
        CHECK(spsc_try_pop(&r, &it));
        CHECK_EQ(VAL(it), out++);
    }
    spsc_destroy(&r);
}

/* The counters are free-running size_t: check the arithmetic still holds
 * when they overflow past SIZE_MAX back to 0. */
static void test_spsc_counter_overflow(void)
{
    ring_spsc_t r;
    void *it;
    CHECK(spsc_init(&r, 4) == 0);
    size_t start = SIZE_MAX - 5;
    atomic_store(&r.head, start);
    atomic_store(&r.tail, start);
    r.cached_head = r.cached_tail = start;
    unsigned in = 0, out = 0;
    for (int round = 0; round < 10; round++) {     /* crosses SIZE_MAX */
        for (int k = 0; k < 4; k++)
            CHECK(spsc_try_push(&r, ITEM(in++)));
        CHECK(!spsc_try_push(&r, ITEM(0)));        /* full is still detected */
        CHECK_EQ(spsc_count(&r), 4);
        for (int k = 0; k < 4; k++) {
            CHECK(spsc_try_pop(&r, &it));
            CHECK_EQ(VAL(it), out++);
        }
        CHECK(!spsc_try_pop(&r, &it));             /* empty is still detected */
    }
    CHECK(atomic_load(&r.tail) < start);           /* we really wrapped */
    spsc_destroy(&r);
}

static void test_spsc_timeout_and_close(void)
{
    ring_spsc_t r;
    void *it;
    CHECK(spsc_init(&r, 2) == 0);
    uint64_t t0 = now_ns();
    CHECK(!spsc_pop_timed(&r, &it, 10 * 1000000ull));
    CHECK(now_ns() - t0 >= 9 * 1000000ull);
    CHECK(spsc_try_push(&r, ITEM(1)));
    spsc_close(&r);
    CHECK(spsc_pop_timed(&r, &it, 1000000));   /* drains after close */
    CHECK_EQ(VAL(it), 1);
    CHECK(!spsc_pop_timed(&r, &it, 1000000000ull));  /* then returns at once */
    CHECK(!spsc_push(&r, ITEM(2)));
    spsc_destroy(&r);
}

/* One producer, one consumer, millions of items through a small ring:
 * every item must arrive exactly once and in order. This is the test that
 * would catch a missing release/acquire under TSan (or on a weakly
 * ordered CPU). */
typedef struct {
    ring_spsc_t *r;
    uint64_t     n;
    uint64_t     bad_order;
    uint64_t     received;
} spsc_arg_t;

static void *spsc_producer(void *arg)
{
    spsc_arg_t *a = arg;
    for (uint64_t i = 0; i < a->n; i++)
        spsc_push(a->r, ITEM(i));
    return NULL;
}

static void *spsc_consumer(void *arg)
{
    spsc_arg_t *a = arg;
    void *it;
    for (uint64_t expect = 0; expect < a->n;) {
        if (!spsc_pop_timed(a->r, &it, 2000000000ull))
            break;
        if (VAL(it) != expect)
            a->bad_order++;
        expect++;
        a->received++;
    }
    return NULL;
}

static void test_spsc_threads(void)
{
    ring_spsc_t r;
    CHECK(spsc_init(&r, 16) == 0);
    spsc_arg_t a = { &r, THREAD_ITEMS, 0, 0 };
    pthread_t p, c;
    pthread_create(&c, NULL, spsc_consumer, &a);
    pthread_create(&p, NULL, spsc_producer, &a);
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    CHECK_EQ(a.received, THREAD_ITEMS);
    CHECK_EQ(a.bad_order, 0);
    spsc_destroy(&r);
}

/* Payload published through the ring: the consumer reads a struct the
 * producer filled in just before pushing a pointer to it. With correct
 * release/acquire ordering the consumer always sees the finished struct;
 * TSan would report a race on msg->a/b otherwise. */
typedef struct {
    uint64_t a, b;
} msg_t;

#define MSGS 4096
static msg_t msgs[MSGS];

static void *msg_producer(void *arg)
{
    ring_spsc_t *r = arg;
    for (uint64_t i = 0; i < MSGS; i++) {
        msgs[i].a = i;
        msgs[i].b = ~i;
        spsc_push(r, &msgs[i]);
    }
    return NULL;
}

static void test_spsc_publishes_payload(void)
{
    ring_spsc_t r;
    CHECK(spsc_init(&r, 8) == 0);
    pthread_t p;
    pthread_create(&p, NULL, msg_producer, &r);
    uint64_t bad = 0;
    for (uint64_t i = 0; i < MSGS; i++) {
        void *it;
        while (!spsc_try_pop(&r, &it))
            cpu_relax();
        msg_t *m = it;
        if (m->a != i || m->b != ~i)
            bad++;
    }
    pthread_join(p, NULL);
    CHECK_EQ(bad, 0);
    spsc_destroy(&r);
}

int main(void)
{
    printf("test_ring\n");
    RUN_TEST(test_mutex_init_rejects_zero);
    RUN_TEST(test_mutex_empty_full_order);
    RUN_TEST(test_mutex_wraparound);
    RUN_TEST(test_mutex_timeout);
    RUN_TEST(test_mutex_close_wakes_waiter);
    RUN_TEST(test_mutex_mpmc_threads);
    RUN_TEST(test_spsc_init_rejects_bad_capacity);
    RUN_TEST(test_spsc_empty_full_order);
    RUN_TEST(test_spsc_wraparound);
    RUN_TEST(test_spsc_counter_overflow);
    RUN_TEST(test_spsc_timeout_and_close);
    RUN_TEST(test_spsc_threads);
    RUN_TEST(test_spsc_publishes_payload);
    return test_summary("test_ring");
}
