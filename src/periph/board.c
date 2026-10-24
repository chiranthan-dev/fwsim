/*
 * board.c - memory map, wiring and the sample clock. See board.h.
 */
#include "periph/board.h"

#include <string.h>

static uint32_t timer_read(void *dev, uint32_t off)
{
    timer_dev_t *t = dev;
    if (off == TIMER_LO) {
        t->latched_hi = (uint32_t)(t->now_us >> 32);
        return (uint32_t)t->now_us;
    }
    if (off == TIMER_HI)
        return t->latched_hi;
    return 0;
}

/* The accelerometer's INT pin is a wire into GPIO0. Called with the bus
 * lock held (from a register access or a sample tick). */
static void accel_pin(void *ctx, int level)
{
    board_t *b = ctx;
    gpio_ctrl_set_input(&b->gpio, ACCEL_INT_PIN, level);
}

int board_init(board_t *b, irq_mode_t mode, accel_source_fn src, void *src_ctx,
               double noise_mg, uint64_t seed)
{
    memset(b, 0, sizeof(*b));
    if (mmio_bus_init(&b->bus) != 0)
        return -1;
    if (irq_init(&b->irq, mode) != 0) {
        mmio_bus_destroy(&b->bus);
        return -1;
    }
    accel_model_init(&b->accel, src, src_ctx, noise_mg, seed, accel_pin, b);
    spi_slave_t slave = accel_model_spi(&b->accel);
    spi_ctrl_init(&b->spi, &slave);
    gpio_ctrl_init(&b->gpio, &b->irq, IRQ_GPIO0);
    if (mmio_map(&b->bus, SPI0_BASE, SPI_REG_SPAN, spi_ctrl_read, spi_ctrl_write, &b->spi,
                 "SPI0") != 0 ||
        mmio_map(&b->bus, GPIO0_BASE, GPIO_REG_SPAN, gpio_ctrl_read, gpio_ctrl_write,
                 &b->gpio, "GPIO0") != 0 ||
        mmio_map(&b->bus, TIMER0_BASE, TIMER_REG_SPAN, timer_read, NULL, &b->timer,
                 "TIMER0") != 0) {
        board_destroy(b);
        return -1;
    }
    return 0;
}

void board_destroy(board_t *b)
{
    irq_destroy(&b->irq);
    mmio_bus_destroy(&b->bus);
}

typedef struct {
    board_t *b;
    uint64_t t_us;
    int      sampled;
} tick_arg_t;

/* One sample period boundary, under the bus lock: move the timer and let
 * the accelerometer produce its sample (if it is powered). */
static void tick_locked(void *arg)
{
    tick_arg_t *a = arg;
    board_t *b = a->b;
    b->timer.now_us = a->t_us;
    a->sampled = 0;
    if (!accel_model_powered(&b->accel))
        return;
    accel_model_sample(&b->accel, a->t_us);
    a->sampled = 1;
}

static void set_time_locked(void *arg)
{
    tick_arg_t *a = arg;
    if (a->t_us > a->b->timer.now_us)
        a->b->timer.now_us = a->t_us;
}

void board_advance(board_t *b, uint64_t t_us, board_tick_fn after_tick, void *ctx)
{
    for (;;) {
        /* The sample period can change at run time (CTRL1.ODR), so derive
         * it per sample. Reading the register outside the bus lock would
         * race with a driver write in threaded mode, hence via the bus. */
        uint32_t odr = 0;
        pthread_mutex_lock(&b->bus.lock);
        int on = accel_model_powered(&b->accel);
        if (on)
            odr = accel_model_odr_hz(&b->accel);
        pthread_mutex_unlock(&b->bus.lock);
        uint64_t period = on ? 1000000u / odr : 0;

        if (!on) {
            /* Sensor off: no samples; keep the schedule aligned to now. */
            b->next_sample_us = t_us;
            break;
        }
        if (b->next_sample_us == 0)
            b->next_sample_us = period;
        if (b->next_sample_us > t_us)
            break;
        tick_arg_t a = { b, b->next_sample_us, 0 };
        mmio_locked(&b->bus, tick_locked, &a);
        b->next_sample_us += period;
        if (a.sampled)
            b->samples++;
        irq_dispatch(&b->irq);       /* deterministic mode: ISRs run here */
        if (after_tick)
            after_tick(ctx, a.t_us);
    }
    tick_arg_t a = { b, t_us, 0 };
    mmio_locked(&b->bus, set_time_locked, &a);
}

uint64_t board_now_us(board_t *b)
{
    pthread_mutex_lock(&b->bus.lock);
    uint64_t t = b->timer.now_us;
    pthread_mutex_unlock(&b->bus.lock);
    return t;
}
