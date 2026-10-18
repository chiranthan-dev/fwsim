/*
 * clock.h - monotonic time and precise, interruptible delays.
 *
 * WHY a module for this: every number fwsim reports (latency, throughput)
 * is measured with these two functions, so they must be right:
 *
 *   - CLOCK_MONOTONIC, never CLOCK_REALTIME: wall-clock time can jump when
 *     NTP adjusts it, which would produce negative or huge latencies.
 *
 *   - Media time from disk-sim is "played back" in real time (scaled down,
 *     see fw_config_t.time_scale) so that the host, the firmware thread and
 *     the power-loss trigger really run concurrently. nanosleep() alone
 *     overshoots by 50-100 us on Linux, which would distort a 300 us media
 *     access by 30%. So we sleep for most of the interval and spin for the
 *     last stretch.
 *
 *   - The delay can be interrupted by a flag. A power cut must stop the
 *     drive *in the middle* of a seek or a transfer, not after it.
 */
#ifndef FWSIM_CLOCK_H
#define FWSIM_CLOCK_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Hint to the CPU that we are in a spin-wait loop (reduces power and frees
 * pipeline resources for a sibling hyper-thread). No-op elsewhere. */
static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

/*
 * Wait until now_ns() >= deadline_ns, or until *keep_going becomes 0
 * (keep_going may be NULL). Returns the time at which the wait ended, so a
 * caller that was interrupted can tell how far it got.
 */
uint64_t delay_until(uint64_t deadline_ns, const atomic_int *keep_going);

#endif
