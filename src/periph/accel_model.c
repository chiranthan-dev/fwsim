/*
 * accel_model.c - SIMACC-1 accelerometer model. Register map in
 * accel_model.h.
 */
#include "periph/accel_model.h"

#include <math.h>
#include <string.h>

#include "util.h"

static const uint32_t odr_table[8] = { 250, 500, 1000, 2000, 4000, 1000, 1000, 1000 };

static void reset_regs(accel_model_t *a)
{
    memset(a->reg, 0, sizeof(a->reg));
    a->reg[ACCEL_CTRL1] = (uint8_t)(ACCEL_ODR_1000HZ << ACCEL_CTRL1_ODR_SHIFT);
    a->fifo_head = a->fifo_n = 0;
    a->fifo_hi_phase = 0;
    a->ovr = 0;
    a->shock_latched = 0;
    a->over_run = 0;
    a->out = 0;
    a->sample_cnt = 0;
}

void accel_model_init(accel_model_t *a, accel_source_fn src, void *src_ctx,
                      double noise_mg, uint64_t seed, accel_pin_fn pin_cb, void *pin_ctx)
{
    memset(a, 0, sizeof(*a));
    a->src = src;
    a->src_ctx = src_ctx;
    a->noise_mg = noise_mg;
    a->rng = seed ^ ACCEL_RNG_SALT;
    a->pin_cb = pin_cb;
    a->pin_ctx = pin_ctx;
    reset_regs(a);
}

uint32_t accel_model_odr_hz(const accel_model_t *a)
{
    return odr_table[(a->reg[ACCEL_CTRL1] & ACCEL_CTRL1_ODR_MASK) >> ACCEL_CTRL1_ODR_SHIFT];
}

int accel_model_powered(const accel_model_t *a)
{
    return (a->reg[ACCEL_CTRL1] & ACCEL_CTRL1_PWR) != 0;
}

/* ---- interrupt logic ------------------------------------------------------ */

static unsigned wtm_level(const accel_model_t *a)
{
    return a->reg[ACCEL_FIFO_WTM];
}

static uint8_t int_src(const accel_model_t *a)
{
    uint8_t s = 0;
    if (wtm_level(a) && a->fifo_n >= wtm_level(a))
        s |= ACCEL_INT_WTM;
    if (a->shock_latched)
        s |= ACCEL_INT_SHOCK;
    if (a->ovr)
        s |= ACCEL_INT_OVR;
    return s;
}

static void update_pin(accel_model_t *a)
{
    uint8_t cfg = a->reg[ACCEL_INT_CFG];
    int active = (int_src(a) & cfg & (ACCEL_INT_WTM | ACCEL_INT_SHOCK | ACCEL_INT_OVR)) != 0;
    int level = (cfg & ACCEL_INT_POL_LOW) ? !active : active;
    if (level != a->pin) {
        a->pin = level;
        if (a->pin_cb)
            a->pin_cb(a->pin_ctx, level);
    }
}

/* ---- sampling --------------------------------------------------------------- */

/* Standard normal via Box-Muller. One draw per call keeps the stream
 * simple to reason about (we throw the second value away). */
static double gauss(uint64_t *rng)
{
    double u1 = rng_unit64(rng), u2 = rng_unit64(rng);
    if (u1 < 1e-300)
        u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

int16_t accel_model_digitize(accel_source_fn src, void *src_ctx, uint64_t t_us,
                             uint32_t odr_hz, double noise_mg, uint64_t *rng)
{
    uint64_t period = 1000000u / odr_hz;
    double acc = 0.0;
    for (unsigned k = 0; k < ACCEL_OVERSAMPLE; k++) {
        uint64_t t = t_us - period + (k + 1) * period / ACCEL_OVERSAMPLE;
        acc += src ? src(src_ctx, t) : 0.0;
    }
    double v = acc / ACCEL_OVERSAMPLE + noise_mg * gauss(rng);
    v = nearbyint(v);
    if (v > 32767.0)
        v = 32767.0;
    if (v < -32768.0)
        v = -32768.0;
    return (int16_t)v;
}

void accel_model_sample(accel_model_t *a, uint64_t t_us)
{
    if (!accel_model_powered(a))
        return;
    int16_t s = accel_model_digitize(a->src, a->src_ctx, t_us, accel_model_odr_hz(a),
                                     a->noise_mg, &a->rng);
    a->out = s;
    a->sample_cnt++;

    if (a->reg[ACCEL_CTRL2] & ACCEL_CTRL2_FIFO_EN) {
        if (a->fifo_n == ACCEL_FIFO_DEPTH) {          /* stream mode */
            a->fifo_head = (a->fifo_head + 1) % ACCEL_FIFO_DEPTH;
            a->fifo_n--;
            a->fifo_hi_phase = 0;
            a->ovr = 1;
            a->fifo_overruns++;
        }
        a->fifo[(a->fifo_head + a->fifo_n) % ACCEL_FIFO_DEPTH] = s;
        a->fifo_n++;
    }

    unsigned ths = (unsigned)a->reg[ACCEL_THS_L] | (unsigned)a->reg[ACCEL_THS_H] << 8;
    unsigned dur = a->reg[ACCEL_THS_DUR] ? a->reg[ACCEL_THS_DUR] : 1;
    unsigned mag = (unsigned)(s < 0 ? -(int)s : s);
    a->over_run = (ths && mag >= ths) ? a->over_run + 1 : 0;
    int shock_now = a->over_run >= dur;
    if (a->reg[ACCEL_INT_CFG] & ACCEL_INT_LATCH)
        a->shock_latched |= shock_now;
    else
        a->shock_latched = shock_now;
    update_pin(a);
}

/* ---- register file ------------------------------------------------------------ */

static uint8_t fifo_read_byte(accel_model_t *a)
{
    if (a->fifo_n == 0)
        return 0;                      /* empty FIFO reads as zero */
    uint16_t v = (uint16_t)a->fifo[a->fifo_head];
    if (!a->fifo_hi_phase) {
        a->fifo_hi_phase = 1;
        return (uint8_t)v;
    }
    a->fifo_hi_phase = 0;
    a->fifo_head = (a->fifo_head + 1) % ACCEL_FIFO_DEPTH;
    a->fifo_n--;
    update_pin(a);                     /* WTM may have dropped */
    return (uint8_t)(v >> 8);
}

static uint8_t reg_read(accel_model_t *a, uint8_t addr)
{
    switch (addr) {
    case ACCEL_WHO_AM_I:   return ACCEL_ID_VALUE;
    case ACCEL_REV_ID:     return ACCEL_REV_VALUE;
    case ACCEL_FIFO_LEVEL: return (uint8_t)a->fifo_n;
    case ACCEL_FIFO_FLAGS: {
        uint8_t f = 0;
        if (wtm_level(a) && a->fifo_n >= wtm_level(a))
            f |= ACCEL_FLAG_WTM;
        if (a->ovr)
            f |= ACCEL_FLAG_OVR;
        if (a->fifo_n == 0)
            f |= ACCEL_FLAG_EMPTY;
        return f;
    }
    case ACCEL_INT_SRC: {
        uint8_t s = int_src(a);
        a->shock_latched = 0;          /* read-to-clear */
        a->ovr = 0;
        update_pin(a);
        return s;
    }
    case ACCEL_OUT_L:       return (uint8_t)((uint16_t)a->out);
    case ACCEL_OUT_H:       return (uint8_t)((uint16_t)a->out >> 8);
    case ACCEL_SAMPLE_CNT0:     return (uint8_t)a->sample_cnt;
    case ACCEL_SAMPLE_CNT0 + 1: return (uint8_t)(a->sample_cnt >> 8);
    case ACCEL_SAMPLE_CNT0 + 2: return (uint8_t)(a->sample_cnt >> 16);
    case ACCEL_SAMPLE_CNT0 + 3: return (uint8_t)(a->sample_cnt >> 24);
    case ACCEL_FIFO_DATA:   return fifo_read_byte(a);
    case ACCEL_CTRL1: case ACCEL_CTRL2: case ACCEL_THS_L: case ACCEL_THS_H:
    case ACCEL_THS_DUR: case ACCEL_FIFO_WTM: case ACCEL_INT_CFG:
        return a->reg[addr];
    default:
        return 0;
    }
}

static void reg_write(accel_model_t *a, uint8_t addr, uint8_t v)
{
    switch (addr) {
    case ACCEL_CTRL1:
        if (v & ACCEL_CTRL1_SOFT_RESET) {
            reset_regs(a);
            update_pin(a);
            return;
        }
        a->reg[addr] = v;
        break;
    case ACCEL_CTRL2:
        if (v & ACCEL_CTRL2_FIFO_FLUSH) {
            a->fifo_head = a->fifo_n = 0;
            a->fifo_hi_phase = 0;
            a->ovr = 0;
        }
        a->reg[addr] = v & ACCEL_CTRL2_FIFO_EN;
        break;
    case ACCEL_FIFO_WTM:
        a->reg[addr] = v > ACCEL_FIFO_DEPTH ? (uint8_t)ACCEL_FIFO_DEPTH : v;
        break;
    case ACCEL_THS_L: case ACCEL_THS_H: case ACCEL_THS_DUR: case ACCEL_INT_CFG:
        a->reg[addr] = v;
        break;
    default:
        return;                        /* read-only / reserved */
    }
    update_pin(a);
}

/* ---- SPI slave ------------------------------------------------------------------ */

static void spi_select(void *dev, int active)
{
    accel_model_t *a = dev;
    a->in_frame = active;
    a->have_cmd = 0;
    /* A frame that ends half-way through a sample leaves it in the FIFO:
     * the next burst starts again at its low byte. */
    a->fifo_hi_phase = 0;
}

static uint8_t spi_xfer(void *dev, uint8_t mosi)
{
    accel_model_t *a = dev;
    if (!a->in_frame)
        return 0xFF;
    if (!a->have_cmd) {
        a->have_cmd = 1;
        a->is_read = (mosi & 0x80u) != 0;
        a->auto_inc = (mosi & 0x40u) != 0;
        a->addr = mosi & 0x3Fu;
        return 0x00;
    }
    uint8_t out = 0;
    if (a->is_read)
        out = reg_read(a, a->addr);
    else
        reg_write(a, a->addr, mosi);
    if (a->auto_inc && a->addr != ACCEL_FIFO_DATA)
        a->addr = (uint8_t)((a->addr + 1) & 0x3Fu);
    return out;
}

spi_slave_t accel_model_spi(accel_model_t *a)
{
    spi_slave_t s = { spi_select, spi_xfer, a };
    return s;
}
