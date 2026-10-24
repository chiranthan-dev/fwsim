/*
 * env_sim.c - synthetic vibration environment and off-track physics.
 * The model, its constants and the reasons for them are in env_sim.h.
 */
#include "periph/env_sim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define TWO_PI 6.283185307179586
#define LN10   2.302585092994046

/* Segment class probabilities per mix: QUIET, SHOCK, VIBRATION, TAP; and the
 * probability that a vibration segment also carries a shock. */
static const struct {
    const char *name;
    double      p[ENV_NCLASSES];
    double      overlay_shock;
} mixes[ENV_MIX_COUNT] = {
    [ENV_MIX_OFF]       = { "off",       { 1.00, 0.00, 0.00, 0.00 }, 0.00 },
    [ENV_MIX_QUIET]     = { "quiet",     { 0.85, 0.00, 0.05, 0.10 }, 0.00 },
    [ENV_MIX_SHOCK]     = { "shock",     { 0.35, 0.45, 0.05, 0.15 }, 0.10 },
    [ENV_MIX_VIBRATION] = { "vibration", { 0.25, 0.05, 0.60, 0.10 }, 0.10 },
    [ENV_MIX_MIXED]     = { "mixed",     { 0.25, 0.25, 0.25, 0.25 }, 0.15 },
    [ENV_MIX_TRAIN]     = { "train",     { 0.25, 0.25, 0.25, 0.25 }, 0.15 },
};

static const char *class_names[ENV_NCLASSES] = { "quiet", "shock", "vibration", "tap" };

const char *env_class_str(int c)
{
    return (c >= 0 && c < ENV_NCLASSES) ? class_names[c] : "?";
}

const char *env_mix_str(int m)
{
    return (m >= 0 && m < ENV_MIX_COUNT) ? mixes[m].name : "?";
}

int env_mix_parse(const char *s)
{
    for (int m = 0; m < ENV_MIX_COUNT; m++)
        if (strcmp(s, mixes[m].name) == 0)
            return m;
    return -1;
}

/* ---- random helpers ----------------------------------------------------------- */

static double uni(env_sim_t *e, double lo, double hi)
{
    return lo + (hi - lo) * rng_unit64(&e->rng);
}

static double log_uni(env_sim_t *e, double lo, double hi)
{
    return exp(uni(e, log(lo), log(hi)));
}

static double gauss(env_sim_t *e)
{
    double u1 = rng_unit64(&e->rng), u2 = rng_unit64(&e->rng);
    if (u1 < 1e-300)
        u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(TWO_PI * u2);
}

static int64_t ms_to_samples(double ms)
{
    return (int64_t)(ms * ENV_FS_HZ / 1000.0);
}

/* ---- filters (RBJ cookbook biquads) ---------------------------------------------- */

static void biquad_hp(double *c, double f0, double q)
{
    double w = TWO_PI * f0 / ENV_FS_HZ, cw = cos(w), al = sin(w) / (2 * q), a0 = 1 + al;
    c[0] = (1 + cw) / 2 / a0;
    c[1] = -(1 + cw) / a0;
    c[2] = (1 + cw) / 2 / a0;
    c[3] = -2 * cw / a0;
    c[4] = (1 - al) / a0;
}

static void biquad_lp(double *c, double f0, double q)
{
    double w = TWO_PI * f0 / ENV_FS_HZ, cw = cos(w), al = sin(w) / (2 * q), a0 = 1 + al;
    c[0] = (1 - cw) / 2 / a0;
    c[1] = (1 - cw) / a0;
    c[2] = (1 - cw) / 2 / a0;
    c[3] = -2 * cw / a0;
    c[4] = (1 - al) / a0;
}

/* ---- segments and bursts ------------------------------------------------------------ */

static void add_burst(env_sim_t *e, int64_t start, uint8_t cls)
{
    if (e->nburst == ENV_MAX_BURSTS)
        return;                                 /* cannot happen at these rates */
    env_burst_t *b = &e->burst[e->nburst++];
    b->start = start;
    b->cls = cls;
    b->phase = uni(e, 0, TWO_PI);
    if (cls == ENV_SHOCK) {
        b->amp = log_uni(e, 400.0, 6000.0);
        b->w = TWO_PI * uni(e, 60.0, 320.0);
        b->tau_s = uni(e, 3.0, 25.0) / 1000.0;
        b->w2 = b->w * 2.3 * uni(e, 0.9, 1.1);
        b->amp2 = 0.3 * b->amp * uni(e, 0.5, 1.0);
    } else {
        b->amp = log_uni(e, 200.0, 1500.0);
        b->w = TWO_PI * uni(e, 280.0, 480.0);
        b->tau_s = uni(e, 0.5, 4.0) / 1000.0;
        b->w2 = 0.0;
        b->amp2 = 0.0;
    }
}

static void start_segment(env_sim_t *e, int64_t i)
{
    const double *p = mixes[e->mix].p;
    double u = rng_unit64(&e->rng), acc = 0;
    int cls = ENV_QUIET;
    for (int c = 0; c < ENV_NCLASSES; c++) {
        acc += p[c];
        if (u < acc) {
            cls = c;
            break;
        }
    }
    static const double dur_s[ENV_NCLASSES][2] = {
        { 0.3, 3.0 }, { 0.4, 1.5 }, { 1.0, 4.0 }, { 0.5, 3.0 } };
    e->seg_cls = (uint8_t)cls;
    e->seg_start = i;
    e->seg_end = i + ms_to_samples(1000.0 * uni(e, dur_s[cls][0], dur_s[cls][1]));

    if (cls == ENV_SHOCK) {
        int n = 1 + (int)rng_below64(&e->rng, 3);
        int64_t t = i + ms_to_samples(20.0 + uni(e, 0, 200.0));
        for (int k = 0; k < n && t < e->seg_end; k++) {
            add_burst(e, t, ENV_SHOCK);
            t += ms_to_samples(150.0 + uni(e, 0, 300.0));
        }
    } else if (cls == ENV_TAP) {
        e->tap_period = uni(e, 150.0, 600.0);
        e->next_tap = i + ms_to_samples(uni(e, 0, e->tap_period));
    } else if (cls == ENV_VIBRATION) {
        double rms = log_uni(e, 15.0, 200.0);
        e->ncomp = 2 + (int)rng_below64(&e->rng, 3);
        double w[4], wsum = 0;
        for (int k = 0; k < e->ncomp; k++)
            wsum += (w[k] = uni(e, 0.2, 1.0));
        double tone_power = 0.7 * rms * rms;
        for (int k = 0; k < e->ncomp; k++) {
            e->comp_amp[k] = sqrt(2.0 * tone_power * w[k] / wsum);
            e->comp_f[k] = uni(e, 30.0, 300.0);
            e->comp_ph[k] = uni(e, 0, TWO_PI);
            e->comp_df[k] = uni(e, 0.02, 0.10);
            e->comp_dfw[k] = TWO_PI * uni(e, 0.1, 0.5);
            e->comp_da[k] = uni(e, 0.1, 0.5);
            e->comp_daw[k] = TWO_PI * uni(e, 0.2, 1.0);
            e->comp_m0[k] = uni(e, 0, TWO_PI);
        }
        /* Band-limited noise: a constant-peak-gain band-pass on white
         * noise. Its noise bandwidth is ~ (pi/2) f0/Q, which sets the gain
         * that gives the remaining 30% of the power. */
        double f0 = uni(e, 40.0, 250.0), q = 2.0;
        double ww = TWO_PI * f0 / ENV_FS_HZ, al = sin(ww) / (2 * q), a0 = 1 + al;
        e->bn_b0 = al / a0;
        e->bn_a1 = -2 * cos(ww) / a0;
        e->bn_a2 = (1 - al) / a0;
        e->bn_gain = sqrt(0.3) * rms / sqrt(3.141592653589793 * f0 / (q * ENV_FS_HZ));
        e->bn_x1 = e->bn_x2 = e->bn_y1 = e->bn_y2 = 0;
        if (rng_unit64(&e->rng) < mixes[e->mix].overlay_shock)
            add_burst(e, i + ms_to_samples(uni(e, 100.0, 900.0)), ENV_SHOCK);
    }
}

/* Generate sample number i (== e->gen). */
static void generate_one(env_sim_t *e)
{
    int64_t i = e->gen;
    if (i >= e->seg_end)
        start_segment(e, i);
    double t = (double)i / ENV_FS_HZ;

    double a = e->hum_amp * sin(e->hum_w * t + e->hum_ph) + 1.0 * gauss(e);
    int vib = e->seg_cls == ENV_VIBRATION;
    if (vib) {
        int64_t fade = ms_to_samples(60.0);
        double g = 1.0;
        if (i - e->seg_start < fade)
            g = 0.5 - 0.5 * cos(3.141592653589793 * (double)(i - e->seg_start) / (double)fade);
        else if (e->seg_end - i < fade)
            g = 0.5 - 0.5 * cos(3.141592653589793 * (double)(e->seg_end - i) / (double)fade);
        double v = 0;
        for (int k = 0; k < e->ncomp; k++) {
            double f = e->comp_f[k] * (1.0 + e->comp_df[k] * sin(e->comp_dfw[k] * t + e->comp_m0[k]));
            double amp = e->comp_amp[k] * (1.0 + e->comp_da[k] *
                                           sin(e->comp_daw[k] * t + e->comp_m0[k] + 1.3));
            e->comp_ph[k] += TWO_PI * f / ENV_FS_HZ;
            if (e->comp_ph[k] > TWO_PI)
                e->comp_ph[k] -= TWO_PI;
            v += amp * sin(e->comp_ph[k]);
        }
        double x = e->bn_gain * gauss(e);
        /* Band-pass: b1 = 0, b2 = -b0. */
        double y = e->bn_b0 * (x - e->bn_x2) - e->bn_a1 * e->bn_y1 - e->bn_a2 * e->bn_y2;
        e->bn_x2 = e->bn_x1;
        e->bn_x1 = x;
        e->bn_y2 = e->bn_y1;
        e->bn_y1 = y;
        a += g * (v + y);
    }

    if (e->seg_cls == ENV_TAP && i >= e->next_tap && i < e->seg_end) {
        add_burst(e, i, ENV_TAP);
        e->next_tap += ms_to_samples(e->tap_period * uni(e, 0.9, 1.1));
    }

    int shock_active = 0, tap_active = 0;
    for (int k = 0; k < e->nburst;) {
        env_burst_t *b = &e->burst[k];
        if (i < b->start) {
            k++;
            continue;
        }
        double dt = (double)(i - b->start) / ENV_FS_HZ;
        if (dt > 8.0 * b->tau_s) {                    /* rung out: retire */
            e->burst[k] = e->burst[--e->nburst];
            continue;
        }
        double rise = 1.0 - exp(-dt / 0.0005);
        a += rise * b->amp * exp(-dt / b->tau_s) * sin(b->w * dt + b->phase);
        if (b->amp2 > 0)
            a += rise * b->amp2 * exp(-dt / (0.6 * b->tau_s)) * sin(b->w2 * dt + b->phase);
        /* Active until the envelope is below min(10% of peak, 100 mg). */
        double act = log(b->amp / 100.0);
        if (act < LN10)
            act = LN10;
        if (dt < b->tau_s * act) {
            if (b->cls == ENV_SHOCK)
                shock_active = 1;
            else
                tap_active = 1;
        }
        k++;
    }

    uint8_t lab = shock_active ? ENV_SHOCK : vib ? ENV_VIBRATION : tap_active ? ENV_TAP : ENV_QUIET;

    /* Displacement proxy: HP then LP, scaled to % track pitch. */
    double h = e->hp[0] * a + e->hp[1] * e->hx1 + e->hp[2] * e->hx2 - e->hp[3] * e->hy1 -
               e->hp[4] * e->hy2;
    e->hx2 = e->hx1;
    e->hx1 = a;
    e->hy2 = e->hy1;
    e->hy1 = h;
    double l = e->lp[0] * h + e->lp[1] * e->lx1 + e->lp[2] * e->lx2 - e->lp[3] * e->ly1 -
               e->lp[4] * e->ly2;
    e->lx2 = e->lx1;
    e->lx1 = h;
    e->ly2 = e->ly1;
    e->ly1 = l;

    size_t slot = (size_t)(i % ENV_HISTORY);
    e->acc[slot] = (float)a;
    e->disp[slot] = (float)(ENV_DISP_K * l);
    e->label[slot] = lab;
    e->gen++;
}

/* ---- public ------------------------------------------------------------------------- */

int env_init(env_sim_t *e, env_mix_t mix, uint64_t seed)
{
    memset(e, 0, sizeof(*e));
    if (mix <= ENV_MIX_OFF || mix >= ENV_MIX_COUNT)
        return -1;
    e->mix = mix;
    /* Mix the class into the seed so "seed 7" of two mixes are unrelated. */
    e->rng = seed * 0x9E3779B97F4A7C15ull ^ ((uint64_t)mix << 56) ^ 0x656E765F73696Dull;
    (void)rng_next64(&e->rng);
    e->acc = calloc(ENV_HISTORY, sizeof(*e->acc));
    e->disp = calloc(ENV_HISTORY, sizeof(*e->disp));
    e->label = calloc(ENV_HISTORY, sizeof(*e->label));
    if (!e->acc || !e->disp || !e->label) {
        env_free(e);
        return -1;
    }
    e->hum_amp = uni(e, 0.0, 8.0);
    e->hum_w = TWO_PI * uni(e, 40.0, 120.0);
    e->hum_ph = uni(e, 0, TWO_PI);
    biquad_hp(e->hp, ENV_HP_HZ, 0.7071067811865476);
    biquad_lp(e->lp, ENV_LP_HZ, 0.7071067811865476);
    e->seg_end = 0;                       /* first sample starts a segment */
    return 0;
}

void env_free(env_sim_t *e)
{
    free(e->acc);
    free(e->disp);
    free(e->label);
    e->acc = e->disp = NULL;
    e->label = NULL;
}

static size_t slot_for(env_sim_t *e, uint64_t t_us)
{
    int64_t i = (int64_t)(t_us * ENV_FS_HZ / 1000000u);
    while (e->gen <= i)
        generate_one(e);
    if (i < e->gen - ENV_HISTORY) {       /* older than we keep: clamp */
        e->clamped++;
        i = e->gen - ENV_HISTORY;
    }
    return (size_t)(i % ENV_HISTORY);
}

double env_accel_mg(env_sim_t *e, uint64_t t_us)
{
    return e->acc[slot_for(e, t_us)];
}

double env_disp_pct(env_sim_t *e, uint64_t t_us)
{
    return e->disp[slot_for(e, t_us)];
}

env_class_t env_label(env_sim_t *e, uint64_t t_us)
{
    return (env_class_t)e->label[slot_for(e, t_us)];
}

double env_accel_source(void *ctx, uint64_t t_us)
{
    return env_accel_mg(ctx, t_us);
}

static double qfunc(double x)
{
    return 0.5 * erfc(x / 1.4142135623730951);
}

double env_offtrack_prob(double d_pct)
{
    double d = fabs(d_pct);
    double p = qfunc((ENV_WFT_PCT - d) / ENV_SERVO_SIGMA_PCT) +
               qfunc((ENV_WFT_PCT + d) / ENV_SERVO_SIGMA_PCT);
    return p > 1.0 ? 1.0 : p;
}

double env_settle_residual_pct(double tau_ms)
{
    if (tau_ms < 0)
        tau_ms = 0;
    return ENV_SETTLE_R0_PCT * exp(-tau_ms / ENV_SETTLE_TAU_MS);
}
