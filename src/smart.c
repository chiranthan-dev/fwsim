/*
 * smart.c - SMART attribute bookkeeping and integer-only model inference.
 */
#include "smart.h"

#include <string.h>

/* Attribute table order in the SMART_READ payload. */
static const uint8_t attr_ids[FW_SMART_NATTR] = { 5, 9, 187, 188, 194, 197, 198 };

/* ---- history ---------------------------------------------------------------- */

void smart_init(smart_t *s)
{
    memset(s, 0, sizeof(*s));
    memset(s->worst, 0xFF, sizeof(s->worst));
}

static smart_sample_t *newest(smart_t *s)
{
    return &s->hist[s->head];
}

static void push(smart_t *s, uint64_t day)
{
    s->head = (s->head + 1) % SMART_HIST_DAYS;
    if (s->n < SMART_HIST_DAYS)
        s->n++;
    s->hist[s->head].day = day;
}

void smart_update(smart_t *s, uint64_t now_ms, const uint64_t c[SMART_NCOUNTERS],
                  uint32_t temp_c)
{
    if (now_ms < s->now_ms)
        now_ms = s->now_ms;                 /* time never runs backwards */
    uint64_t day = now_ms / SMART_MS_PER_DAY;
    if (s->n == 0) {
        s->head = 0;
        s->n = 1;
        s->hist[0].day = day;
    } else {
        uint64_t last = newest(s)->day;
        uint64_t prev[SMART_NCOUNTERS];
        memcpy(prev, newest(s)->c, sizeof(prev));
        /* Skipped days carry the previous values (a gap longer than the
         * ring only needs enough samples to cover the ring); today then
         * gets the current ones below. */
        uint64_t gap = day - last;
        uint64_t first = gap > SMART_HIST_DAYS ? day - SMART_HIST_DAYS + 1 : last + 1;
        for (uint64_t d = first; d <= day && gap > 0; d++) {
            push(s, d);
            memcpy(newest(s)->c, prev, sizeof(prev));
        }
    }
    memcpy(s->c, c, sizeof(s->c));
    memcpy(newest(s)->c, c, sizeof(newest(s)->c));
    s->now_ms = now_ms;
    s->temp_c = temp_c;
}

uint64_t smart_delta(const smart_t *s, int k, uint32_t days)
{
    if (s->n == 0 || k < 0 || k >= SMART_NCOUNTERS)
        return 0;
    uint64_t today = s->hist[s->head].day;
    uint64_t from = today >= days ? today - days : 0;
    /* Walk from the oldest sample to the newest; the first one inside the
     * window is the baseline. */
    uint32_t oldest = (s->head + SMART_HIST_DAYS + 1 - s->n) % SMART_HIST_DAYS;
    for (uint32_t i = 0; i < s->n; i++) {
        const smart_sample_t *x = &s->hist[(oldest + i) % SMART_HIST_DAYS];
        if (x->day >= from)
            return s->c[k] > x->c[k] ? s->c[k] - x->c[k] : 0;
    }
    return 0;
}

/* ---- features and model ------------------------------------------------------ */

#define LG_FRAC_BITS 4
#define LG_CAP ((UINT64_C(1) << 40) - 1)
#define TEMP_MAX 80

int32_t smart_lg(uint64_t x)
{
    if (x > LG_CAP)
        x = LG_CAP;
    uint64_t v = x + 1;
    int32_t m = 0;
    while (v >> (m + 1))
        m++;                                   /* m = index of v's top bit */
    uint64_t frac = ((v - (UINT64_C(1) << m)) << LG_FRAC_BITS) >> m;
    return (m << LG_FRAC_BITS) + (int32_t)frac;
}

void smart_features(const smart_t *s, int16_t f[SMART_MODEL_NFEAT])
{
    /* Order = smartlib.DEPLOY_FEATURES: lg raw, lg 7-day delta, lg 30-day
     * delta for each counter, then lg power-on hours, temperature. */
    int i = 0;
    for (int k = 0; k < SMART_NCOUNTERS; k++)
        f[i++] = (int16_t)smart_lg(s->c[k]);
    for (int k = 0; k < SMART_NCOUNTERS; k++)
        f[i++] = (int16_t)smart_lg(smart_delta(s, k, 7));
    for (int k = 0; k < SMART_NCOUNTERS; k++)
        f[i++] = (int16_t)smart_lg(smart_delta(s, k, 30));
    f[i++] = (int16_t)smart_lg(s->now_ms / SMART_MS_PER_HOUR);
    f[i++] = (int16_t)(s->temp_c > TEMP_MAX ? TEMP_MAX : s->temp_c);
}

/* floor(x / 2^sh) for negative x too: C leaves >> of a negative signed
 * value implementation-defined, so do it explicitly. */
static int64_t floor_shift(int64_t x, int sh)
{
    if (x >= 0)
        return x >> sh;
    return -(int64_t)(((uint64_t)(-x) + ((UINT64_C(1) << sh) - 1)) >> sh);
}

int32_t smart_logit_q8(const int16_t f[SMART_MODEL_NFEAT])
{
    int32_t acc = 0;
    for (int i = 0; i < SMART_MODEL_NFEAT; i++)
        acc += (int32_t)SMART_MODEL_W[i] * ((int32_t)f[i] - SMART_MODEL_ZP[i]);
    int64_t z = floor_shift((int64_t)acc * SMART_MODEL_MULT + SMART_MODEL_BIAS, SMART_MODEL_SHIFT);
    if (z > INT32_MAX)
        z = INT32_MAX;
    if (z < INT32_MIN)
        z = INT32_MIN;
    return (int32_t)z;
}

uint16_t smart_risk_bp(int32_t logit_q8)
{
    const int64_t hi = (int64_t)(SMART_LUT_N - 1) << SMART_LUT_STEP_SHIFT;
    int64_t t = (int64_t)logit_q8 - SMART_LUT_LO_Q8;
    if (t < 0)
        t = 0;
    if (t > hi)
        t = hi;
    uint32_t i = (uint32_t)(t >> SMART_LUT_STEP_SHIFT);
    uint32_t frac = (uint32_t)(t & ((1 << SMART_LUT_STEP_SHIFT) - 1));
    uint32_t j = i + 1 < SMART_LUT_N ? i + 1 : i;
    uint32_t a = SMART_SIGMOID_LUT[i], b = SMART_SIGMOID_LUT[j];
    return (uint16_t)(a + (((b - a) * frac) >> SMART_LUT_STEP_SHIFT));
}

/* ---- the SMART_READ payload -------------------------------------------------- */

static uint8_t norm_from_count(uint64_t raw)
{
    return (uint8_t)(raw >= 99 ? 1 : 100 - raw);
}

void smart_report(smart_t *s, fw_smart_data_t *out)
{
    uint64_t poh = s->now_ms / SMART_MS_PER_HOUR;
    memset(out, 0, sizeof(*out));
    out->version = FW_SMART_VERSION;
    out->num_attrs = FW_SMART_NATTR;
    out->power_on_hours = poh > UINT32_MAX ? UINT32_MAX : (uint32_t)poh;
    out->history_days = (uint16_t)s->n;
    for (int a = 0; a < FW_SMART_NATTR; a++) {
        fw_smart_attr_t *x = &out->attr[a];
        x->id = attr_ids[a];
        /* Normalised values: fwsim's own vendor convention (every vendor
         * scales differently, which is why the deployed model uses raw
         * counters only). 100 = new, lower = worse, fail at <= threshold. */
        switch (x->id) {
        case 5:   x->raw = s->c[SMART_C_5];   x->normalized = norm_from_count(x->raw); x->threshold = 10; break;
        case 9:   x->raw = poh; x->normalized = (uint8_t)(poh / 876 >= 99 ? 1 : 100 - poh / 876); break;
        case 187: x->raw = s->c[SMART_C_187]; x->normalized = norm_from_count(x->raw); break;
        case 188: x->raw = s->c[SMART_C_188]; x->normalized = norm_from_count(x->raw); break;
        case 194: x->raw = s->temp_c; x->normalized = (uint8_t)(s->temp_c > 100 ? 0 : 100 - s->temp_c); break;
        case 197: x->raw = s->c[SMART_C_197]; x->normalized = norm_from_count(x->raw); break;
        case 198: x->raw = s->c[SMART_C_198]; x->normalized = norm_from_count(x->raw); break;
        default:  break;
        }
        if (x->normalized < s->worst[a])
            s->worst[a] = x->normalized;
        x->worst = s->worst[a];
    }
    int16_t f[SMART_MODEL_NFEAT];
    smart_features(s, f);
    out->logit_q8 = smart_logit_q8(f);
    out->threshold_q8 = SMART_MODEL_THRESH_Q8;
    out->risk_bp = smart_risk_bp(out->logit_q8);
    out->predicted_failure = out->logit_q8 >= SMART_MODEL_THRESH_Q8;
    out->model_id = SMART_MODEL_ID;
}
