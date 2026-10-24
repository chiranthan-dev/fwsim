/*
 * env_monitor.c - shock / vibration monitor and write-protection policies.
 * Design in env_monitor.h; peripherals in periph/, drivers in drivers/.
 */
#include "env_monitor.h"

#include <stdlib.h>
#include <string.h>

#include "drivers/accel.h"
#include "edgeai/features.h"
#include "edgeai/infer.h"
#include "periph/board.h"

/* Defaults for config fields left at 0 (the policy parameters fixed before
 * any system benchmark ran; README "Edge AI vibration classifier").
 * Threshold: the acceleration at which a disturbance at the displacement
 * filter's ~110 Hz peak moves the head by the 15% write-fault limit is
 * 15 / (0.10 %/mg * ~0.7) ~ 210 mg, so 250 mg is "trip where harm starts".
 * (500 mg was the first choice; tests/test_policy showed it releases the
 * writes while a big shock's tail still pushes the head off-track.) */
#define DEF_THRESHOLD_MG    250u
#define DEF_SETTLE_MS       20.0
#define DEF_EXTRA_SETTLE_MS 1.5
#define DEF_VERIFY_PASSES   2u
#define HOP                 16u
#define RING_EVENTS         64u

struct env_monitor {
    media_t       *m;
    fw_counters_t *ctr;
    int            policy;
    int            use_thr, use_ai;

    board_t         board;
    spi_drv_t       spi;
    gpio_drv_t      gpio;
    accel_drv_t     acc;
    ring_spsc_t     ring;
    accel_isr_ctx_t isr;

    const edgeai_model_t *model;
    unsigned       window;
    int16_t        hist[EDGEAI_MAX_WINDOW];    /* last `window` samples (ring) */
    unsigned       hpos;
    uint64_t       nsamples;                   /* samples drained so far      */
    double         period_ms;

    unsigned       thr_mg;
    double         settle_ms, extra_settle_ms;
    uint32_t       verify;

    double         inhibit_until;      /* no write transfer before this     */
    int            inhibit_thr_only;   /* current inhibit came from P1's IRQ */
    double         last_thr_ms;        /* last threshold interrupt           */
    int            protect;            /* vibration: settle + verify          */
    uint8_t        last_cls, last_conf;

    fw_trace_fn    trace;
    void          *trace_ctx;
};

static const char *policy_names[] = { "none", "threshold", "ai", "hybrid" };

const char *vib_policy_str(int p)
{
    return (p >= 0 && p < 4) ? policy_names[p] : "?";
}

int vib_policy_parse(const char *s)
{
    for (int p = 0; p < 4; p++)
        if (!strcmp(s, policy_names[p]))
            return p;
    return -1;
}

/* ---- trace ---------------------------------------------------------------------- */

static void emit(env_monitor_t *v, double t_ms, uint8_t cls, uint8_t conf, uint32_t rms,
                 uint32_t peak)
{
    if (!v->trace)
        return;
    fw_trace_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = FW_TRACE_VIBRATION;
    ev.sim_ms = t_ms;
    ev.vib_class = cls;
    ev.vib_conf = conf;
    ev.vib_inhibit = v->inhibit_until > t_ms;
    ev.vib_protect = (uint8_t)v->protect;
    ev.vib_rms_mg = rms;
    ev.vib_peak_mg = peak;
    ev.vib_inhibit_until_ms = v->inhibit_until;
    v->trace(v->trace_ctx, &ev);
}

/* ---- policy actions ---------------------------------------------------------------- */

static void inhibit(env_monitor_t *v, double now_ms, double until, int from_thr)
{
    if (v->inhibit_until <= now_ms) {
        ctr_add(&v->ctr->vib_inhibits, 1);           /* a new episode */
        v->inhibit_thr_only = from_thr;
    } else if (!from_thr) {
        v->inhibit_thr_only = 0;                     /* the model confirmed it */
    }
    if (until > v->inhibit_until)
        v->inhibit_until = until;
}

static void classify(env_monitor_t *v, double t_ms)
{
    int16_t w[EDGEAI_MAX_WINDOW];
    unsigned n = v->window;
    for (unsigned i = 0; i < n; i++)                 /* oldest first */
        w[i] = v->hist[(v->hpos + i) % n];
    edgeai_feat_t f;
    edgeai_out_t o;
    if (edgeai_features(w, n, &f) != 0 || edgeai_classify(v->model, f.vec, &o) != 0)
        return;
    ctr_add(&v->ctr->vib_windows, 1);
    ctr_add(&v->ctr->vib_class_windows[o.cls & 3], 1);
    v->last_cls = o.cls;
    v->last_conf = o.conf_pct;

    switch (o.cls) {
    case ENV_SHOCK:
        if (v->policy == FW_VIB_AI)
            inhibit(v, t_ms, t_ms + v->settle_ms, 0);
        else
            v->inhibit_thr_only = 0;   /* hybrid: the model confirms the IRQ's
                                          inhibit, so it must not be released */
        v->protect = 0;
        break;
    case ENV_VIBRATION:
        v->protect = 1;
        break;
    default:                                          /* tap, quiet */
        v->protect = 0;
        /* Hybrid: a threshold inhibit whose trigger lies inside this window
         * was a harmless tap (or noise): let writes resume now. */
        if (v->policy == FW_VIB_HYBRID && v->inhibit_thr_only && v->inhibit_until > t_ms &&
            v->last_thr_ms >= t_ms - n * v->period_ms) {
            v->inhibit_until = t_ms;
            ctr_add(&v->ctr->vib_early_releases, 1);
        }
        break;
    }
    emit(v, t_ms, o.cls, o.conf_pct, f.rms, f.peak);
}

static void drain_fifo(env_monitor_t *v)
{
    int16_t s[ACCEL_FIFO_DEPTH];
    unsigned got = accel_fifo_read(&v->acc, s, ACCEL_FIFO_DEPTH);
    for (unsigned i = 0; i < got; i++) {
        v->hist[v->hpos] = s[i];
        v->hpos = (v->hpos + 1) % v->window;
        v->nsamples++;
        /* Sample k (1-based) was taken at k sample periods: the sensor
         * started with the drive and the FIFO is drained without loss. */
        if (v->nsamples >= v->window && v->nsamples % HOP == 0)
            classify(v, (double)v->nsamples * v->period_ms);
    }
}

/* The bottom half: everything the ISR deferred. */
static void bottom_half(env_monitor_t *v)
{
    void *item;
    while (spsc_try_pop(&v->ring, &item)) {
        uintptr_t ev = (uintptr_t)item;
        double t_ev = (double)accel_ev_time_us(ev) / 1000.0;
        uint8_t src = accel_int_source(&v->acc);      /* also clears the latch */
        if ((src & ACCEL_INT_SHOCK) && v->use_thr) {
            ctr_add(&v->ctr->vib_shock_irqs, 1);
            v->last_thr_ms = t_ev;
            inhibit(v, t_ev, t_ev + v->settle_ms, 1);
            emit(v, t_ev, 255, 100, 0, 0);
        }
        if (v->use_ai)
            drain_fifo(v);
    }
}

static void after_tick(void *ctx, uint64_t t_us)
{
    (void)t_us;
    bottom_half(ctx);
}

static void advance_to(env_monitor_t *v, uint64_t t_us)
{
    if (t_us > board_now_us(&v->board))
        board_advance(&v->board, t_us, after_tick, v);
    bottom_half(v);                     /* threaded mode: late ISR events */
}

void envmon_poll(env_monitor_t *v, double t_ms)
{
    /* Bounded: this keeps an idle drive's monitor (and the dashboard) live,
     * but must never starve host commands. When simulated time runs ahead
     * of what the CPU can simulate (time_scale > 0 under a sanitizer, say)
     * the full catch-up happens in envmon_gate, before the next write. */
    uint64_t t_us = t_ms <= 0 ? 0 : (uint64_t)(t_ms * 1000.0);
    uint64_t cap = board_now_us(&v->board) + (uint64_t)(ENVMON_POLL_MAX_MS * 1000.0);
    advance_to(v, t_us < cap ? t_us : cap);
}

void envmon_gate(void *ctx, double t_ms, media_wpolicy_t *pol)
{
    env_monitor_t *v = ctx;
    advance_to(v, t_ms <= 0 ? 0 : (uint64_t)(t_ms * 1000.0));
    pol->inhibit_until_ms = v->inhibit_until;
    pol->settle_ms = v->protect ? v->extra_settle_ms : 0.0;
    pol->verify = v->protect ? v->verify : 0;
}

uint64_t envmon_isr_runs(env_monitor_t *v)
{
    return ctr_get(&v->isr.runs);
}

/* ---- lifecycle ----------------------------------------------------------------------- */

env_monitor_t *envmon_create(media_t *m, const fw_config_t *cfg, fw_counters_t *ctr)
{
    if (!m->env || cfg->vib_policy <= FW_VIB_NONE || cfg->vib_policy > FW_VIB_HYBRID)
        return NULL;
    env_monitor_t *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->m = m;
    v->ctr = ctr;
    v->policy = cfg->vib_policy;
    v->use_thr = v->policy == FW_VIB_THRESHOLD || v->policy == FW_VIB_HYBRID;
    v->use_ai = v->policy == FW_VIB_AI || v->policy == FW_VIB_HYBRID;
    v->thr_mg = cfg->vib_threshold_mg ? cfg->vib_threshold_mg : DEF_THRESHOLD_MG;
    v->settle_ms = cfg->vib_settle_ms > 0 ? cfg->vib_settle_ms : DEF_SETTLE_MS;
    v->extra_settle_ms = cfg->vib_extra_settle_ms > 0 ? cfg->vib_extra_settle_ms : DEF_EXTRA_SETTLE_MS;
    v->verify = cfg->vib_verify_passes ? cfg->vib_verify_passes : DEF_VERIFY_PASSES;
    v->trace = cfg->trace;
    v->trace_ctx = cfg->trace_ctx;
    v->last_thr_ms = -1e9;
    v->period_ms = 1000.0 / EDGEAI_FS_HZ;
    if (v->use_ai) {
        v->window = cfg->vib_window ? cfg->vib_window : edgeai_default_window();
        v->model = edgeai_model_for(v->window);
        if (!v->model)
            goto fail_free;              /* classifier unavailable */
    }

    uint64_t seed = cfg->vib_seed ? cfg->vib_seed : cfg->seed;
    irq_mode_t mode = cfg->vib_irq_threaded ? IRQ_MODE_THREADED : IRQ_MODE_DETERMINISTIC;
    if (board_init(&v->board, mode, env_accel_source, m->env, ACCEL_NOISE_MG_DEFAULT, seed) != 0)
        goto fail_free;
    if (spsc_init(&v->ring, RING_EVENTS) != 0)
        goto fail_board;
    if (spi_init(&v->spi, &v->board.bus, SPI0_BASE, 4) != 0 ||
        gpio_init(&v->gpio, &v->board.bus, GPIO0_BASE) != 0 ||
        accel_probe(&v->acc, &v->spi) != 0)
        goto fail_ring;

    v->isr.gpio = &v->gpio;
    v->isr.bus = &v->board.bus;
    v->isr.timer_base = TIMER0_BASE;
    v->isr.ring = &v->ring;
    irq_set_handler(&v->board.irq, IRQ_GPIO0, accel_isr, &v->isr);
    gpio_irq_config(&v->gpio, ACCEL_INT_PIN, 1, 0, 1);
    irq_enable(&v->board.irq, IRQ_GPIO0);

    accel_cfg_t ac = { ACCEL_ODR_1000HZ, (uint8_t)(v->use_ai ? HOP : 0),
                       (uint16_t)(v->use_thr ? v->thr_mg : 0), 1,
                       (uint8_t)((v->use_ai ? ACCEL_INT_WTM : 0) |
                                 (v->use_thr ? ACCEL_INT_SHOCK | ACCEL_INT_LATCH : 0)) };
    if (accel_configure(&v->acc, &ac) != 0)
        goto fail_ring;
    return v;

fail_ring:
    spsc_destroy(&v->ring);
fail_board:
    board_destroy(&v->board);
fail_free:
    free(v);
    return NULL;
}

void envmon_destroy(env_monitor_t *v)
{
    if (!v)
        return;
    board_destroy(&v->board);            /* joins the interrupt thread first */
    spsc_destroy(&v->ring);
    free(v);
}
