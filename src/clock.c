/*
 * clock.c - see clock.h for why this exists.
 */
#include "clock.h"

/* Below this much remaining time we stop sleeping and spin: the kernel's
 * timer slack plus scheduler wake-up latency is roughly this large. */
#define SPIN_WINDOW_NS 120000ull
/* Never sleep longer than this in one go, so that a power cut is noticed
 * within ~1 ms even during a long full-stroke seek. */
#define MAX_SLEEP_NS 1000000ull

static int still_going(const atomic_int *keep_going)
{
    /* relaxed is enough: we only need to *eventually* see the flag flip,
     * and no other data is published through it (the firmware thread
     * re-checks power state with acquire ordering where it matters). */
    return keep_going == NULL ||
           atomic_load_explicit(keep_going, memory_order_relaxed) != 0;
}

uint64_t delay_until(uint64_t deadline_ns, const atomic_int *keep_going)
{
    uint64_t t = now_ns();
    while (t < deadline_ns && still_going(keep_going)) {
        uint64_t left = deadline_ns - t;
        if (left > SPIN_WINDOW_NS) {
            uint64_t s = left - SPIN_WINDOW_NS;
            if (s > MAX_SLEEP_NS)
                s = MAX_SLEEP_NS;
            struct timespec ts = { (time_t)(s / 1000000000ull),
                                   (long)(s % 1000000000ull) };
            nanosleep(&ts, NULL);
        } else {
            cpu_relax();
        }
        t = now_ns();
    }
    return t;
}
