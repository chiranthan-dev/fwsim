/*
 * timing.h - the three mechanical delays of a disk access (internal).
 *
 *   service = seek + rotational latency + transfer
 *
 * Kept as small pure functions (no disk state) so each can be unit tested
 * against a hand calculation, and so the formulas are easy to point at in
 * an interview.
 */
#ifndef DISKSIM_TIMING_H
#define DISKSIM_TIMING_H

#include <stdint.h>

/* Fraction-of-a-revolution tolerance used to absorb floating point noise
 * when the head is exactly on a sector boundary (1e-7 rev ~ 1 ns). */
#define TIMING_ANGLE_EPS 1e-7

/* Milliseconds per revolution: 60 000 ms/min / rpm. */
double timing_rev_ms(uint32_t rpm);

/* 0 if distance is 0, else settle + k * sqrt(distance). */
double timing_seek_ms(double settle_ms, double k_ms, uint64_t distance);

/* Platter angle at time t, as a fraction of a revolution in [0, 1). */
double timing_angle_at(double t_ms, double rev_ms);

/* Wait until `target_angle` (fraction in [0,1)) comes under the head. */
double timing_rot_wait_ms(double t_ms, double target_angle, double rev_ms);

/* sectors / SPT * revolution time. */
double timing_transfer_ms(uint64_t sectors, uint32_t spt, double rev_ms);

/* Angle where sector `s` of a track with `spt` sectors starts, given the
 * track's skew offset (fraction of a revolution). */
double timing_sector_angle(uint32_t s, uint32_t spt, double skew);

#endif
