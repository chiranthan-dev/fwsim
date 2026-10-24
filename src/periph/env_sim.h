/*
 * env_sim.h - the drive's mechanical environment: a SYNTHETIC, seeded
 * vibration generator plus the head-displacement / off-track physics the
 * media layer uses.
 *
 * !! Everything here is synthetic and every constant is ILLUSTRATIVE. !!
 * The numbers are chosen to be physically plausible in shape (frequency
 * bands, decay times, relative amplitudes), not measured from any real
 * drive or chassis. Results built on this model show how policies compare
 * UNDER THIS MODEL; they are not predictions for real hardware.
 *
 * FROZEN: this model (generator + displacement filter + off-track
 * probability) was written and committed, with its own tests, BEFORE the
 * classifier existed, and has not been tuned since to favour any policy.
 * See README "Edge AI vibration classifier".
 *
 * ---- what is generated -----------------------------------------------------
 * The physical in-plane acceleration a(t) in mg at ENV_FS_HZ (4 kHz), as a
 * sequence of labelled SEGMENTS. The segment class is drawn from the mix's
 * probabilities, durations and every parameter are randomised, and the
 * parameter ranges of different classes OVERLAP on purpose (a hard tap and
 * a soft shock look alike; a weak vibration is close to the noise floor),
 * so no single feature separates the classes perfectly.
 *
 *   QUIET      background only: a weak fan hum (0..8 mg, 40..120 Hz) and a
 *              1 mg physical noise floor (sensor noise is added separately
 *              by the accelerometer model).
 *   SHOCK      1..3 impulse shocks per segment: damped sinusoids, peak
 *              400..6000 mg (log-uniform), 60..320 Hz, decay tau 3..25 ms,
 *              plus a weaker second mode at ~2.3x the frequency.
 *   VIBRATION  sustained rotational vibration from neighbouring drives and
 *              fans: 2..4 sinusoids at 30..300 Hz whose amplitude and
 *              frequency drift slowly, plus band-limited noise; total RMS
 *              15..200 mg (log-uniform); 60 ms fades at the segment edges.
 *   TAP        periodic harmless tapping (a finger, a door, a cable):
 *              impulses every 150..600 ms (+-10% jitter), peak 200..1500 mg,
 *              280..480 Hz, tau 0.5..4 ms. (Overlaps shocks at 280..320 Hz,
 *              tau 3..4 ms and 400..1500 mg.)
 * Overlays: a VIBRATION segment may additionally carry a shock (per-mix
 * probability).
 *
 * Per-sample ground-truth label: SHOCK while any shock burst's envelope is
 * above min(10% of its peak, 100 mg); else VIBRATION inside a vibration
 * segment; else TAP while a tap's envelope is above that level; else
 * QUIET. (So the gaps between taps, or after a shock has died away, are
 * QUIET: the label describes what is happening NOW.)
 *
 * ---- head-displacement proxy -------------------------------------------------
 * d(t) [% of track pitch] = K * BP(a)(t), where BP is a 2nd-order high-pass
 * at 80 Hz (the track-following servo rejects slow disturbances) cascaded
 * with a 2nd-order low-pass at 120 Hz (the arm's inertia: displacement
 * falls as 1/f^2 for fast accelerations). The result peaks at ~100 Hz,
 * which is why sustained 60..200 Hz vibration matters and short 300+ Hz
 * taps mostly do not - an outcome of the filter, not a hard-coded rule.
 *
 * ---- off-track probability ----------------------------------------------------
 * A sector written while the head is displaced by d lands off-track if the
 * true position error |d + n| exceeds the write-fault threshold W, with
 * n ~ N(0, sigma) servo noise:
 *
 *     p(d) = Q((W - d) / sigma) + Q((W + d) / sigma),   W = 15%, sigma = 3%
 *
 * Just after a seek the servo has not fully settled: a residual error
 * R0 * exp(-tau / tau_s) (R0 = 5%, tau_s = 0.5 ms; tau = time since the
 * seek ended) adds to |d|. Off-track sectors are stored corrupted, so they
 * fail CRC when read: damage is detected, never silent.
 */
#ifndef FWSIM_PERIPH_ENV_SIM_H
#define FWSIM_PERIPH_ENV_SIM_H

#include <stdint.h>

/* ---- frozen physical constants (illustrative) ----------------------------- */
#define ENV_FS_HZ            4000      /* physics sample rate                */
#define ENV_DISP_K           0.10      /* % track pitch per mg (band peak)   */
#define ENV_HP_HZ            80.0      /* servo rejection corner              */
#define ENV_LP_HZ            120.0     /* arm inertia corner                  */
#define ENV_WFT_PCT          15.0      /* write-fault threshold, % pitch      */
#define ENV_SERVO_SIGMA_PCT  3.0       /* servo position noise, % pitch       */
#define ENV_SETTLE_R0_PCT    5.0       /* post-seek residual at seek end      */
#define ENV_SETTLE_TAU_MS    0.5       /* ... decay time constant             */
#define ENV_HISTORY          16384     /* samples kept for look-back (4.1 s)  */

typedef enum {
    ENV_QUIET = 0,
    ENV_SHOCK,
    ENV_VIBRATION,
    ENV_TAP,
    ENV_NCLASSES
} env_class_t;

typedef enum {
    ENV_MIX_OFF = 0,        /* no environment model at all (the default)  */
    ENV_MIX_QUIET,
    ENV_MIX_SHOCK,          /* shock-heavy     */
    ENV_MIX_VIBRATION,      /* vibration-heavy */
    ENV_MIX_MIXED,
    ENV_MIX_TRAIN,          /* the training distribution (= MIXED priors) */
    ENV_MIX_COUNT
} env_mix_t;

#define ENV_MAX_BURSTS 16

typedef struct {
    int64_t start;           /* sample index                          */
    double  amp, w, tau_s, phase, w2, amp2;
    uint8_t cls;             /* ENV_SHOCK or ENV_TAP                  */
} env_burst_t;

typedef struct {
    env_mix_t mix;
    uint64_t  rng;

    /* history ring: newest sample is gen - 1 */
    float    *acc, *disp;
    uint8_t  *label;
    int64_t   gen;              /* samples generated so far               */
    uint64_t  clamped;          /* look-backs older than the history      */

    /* current segment */
    uint8_t   seg_cls;
    int64_t   seg_start, seg_end;
    /* vibration segment */
    int       ncomp;
    double    comp_amp[4], comp_f[4], comp_ph[4], comp_df[4], comp_dfw[4], comp_da[4],
              comp_daw[4], comp_m0[4];
    double    bn_gain, bn_b0, bn_a1, bn_a2, bn_x1, bn_x2, bn_y1, bn_y2;  /* band noise */
    /* tap segment */
    int64_t   next_tap;
    double    tap_period;
    /* bursts (scheduled + active) */
    env_burst_t burst[ENV_MAX_BURSTS];
    int       nburst;
    /* background */
    double    hum_amp, hum_w, hum_ph;
    /* displacement filter state (two biquads, direct form I) */
    double    hp[5], lp[5];             /* b0 b1 b2 a1 a2 */
    double    hx1, hx2, hy1, hy2, lx1, lx2, ly1, ly2;
} env_sim_t;

int  env_init(env_sim_t *e, env_mix_t mix, uint64_t seed);   /* 0 / -1 */
void env_free(env_sim_t *e);

/* Queries at simulated time t_us (microseconds since power-on). Times may
 * go forward arbitrarily (generated on demand) and back up to ENV_HISTORY
 * samples behind the newest generated one. */
double      env_accel_mg(env_sim_t *e, uint64_t t_us);
double      env_disp_pct(env_sim_t *e, uint64_t t_us);
env_class_t env_label(env_sim_t *e, uint64_t t_us);

/* p(d) above, for a total displacement |d| in % track pitch. */
double env_offtrack_prob(double d_pct);
/* Residual post-seek settle error after tau_ms (0 if there was no seek). */
double env_settle_residual_pct(double tau_ms);

const char *env_class_str(int c);
const char *env_mix_str(int m);
int         env_mix_parse(const char *s);     /* -1 if unknown */

/* accel_source_fn adapter for the accelerometer model (ctx = env_sim_t). */
double env_accel_source(void *ctx, uint64_t t_us);

#endif
