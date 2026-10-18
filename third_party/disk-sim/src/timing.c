/*
 * timing.c - seek, rotation, transfer.
 *
 * Seek model: settle + k*sqrt(d).
 *   A voice-coil actuator accelerates for half the distance and decelerates
 *   for the other half. Under constant acceleration, distance grows with
 *   time squared, so time grows with sqrt(distance). The constant "settle"
 *   term is the time to stop ringing and lock onto the target track with
 *   the servo; it dominates short seeks. (Very long seeks become
 *   coast-limited and closer to linear; we ignore that, see README.)
 *
 * Rotation model: the platter angle is a pure function of simulated time,
 *   angle(t) = (t mod T) / T, with angle 0 at t = 0.
 *   We do NOT keep a random "average half revolution" latency. Deriving the
 *   angle from the clock is what makes rotational-position-aware scheduling
 *   (SPTF) possible and what makes back-to-back sequential I/O cost zero
 *   rotational latency, exactly as on a real drive.
 */
#include "timing.h"

#include <math.h>

double timing_rev_ms(uint32_t rpm)
{
    return 60000.0 / (double)rpm;
}

double timing_seek_ms(double settle_ms, double k_ms, uint64_t distance)
{
    if (distance == 0)
        return 0.0;   /* same cylinder: no arm motion, no settle */
    return settle_ms + k_ms * sqrt((double)distance);
}

double timing_angle_at(double t_ms, double rev_ms)
{
    double a = fmod(t_ms, rev_ms) / rev_ms;
    if (a < 0.0)
        a += 1.0;
    return a;
}

double timing_rot_wait_ms(double t_ms, double target_angle, double rev_ms)
{
    double diff = target_angle - timing_angle_at(t_ms, rev_ms);
    if (diff < 0.0)
        diff += 1.0;
    /* If we are "a hair past" the target only because of rounding, we are
     * really on it: do not charge a full revolution for float noise. */
    if (diff >= 1.0 - TIMING_ANGLE_EPS || diff < TIMING_ANGLE_EPS)
        diff = 0.0;
    return diff * rev_ms;
}

double timing_transfer_ms(uint64_t sectors, uint32_t spt, double rev_ms)
{
    return (double)sectors / (double)spt * rev_ms;
}

double timing_sector_angle(uint32_t s, uint32_t spt, double skew)
{
    double a = (double)s / (double)spt + skew;
    return a - floor(a);
}
