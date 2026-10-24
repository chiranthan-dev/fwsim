/*
 * test_drivers.c - the peripheral layer: MMIO decode, SPI ID read, the
 * accelerometer FIFO (order, watermark, overflow), GPIO edge interrupts,
 * interrupt masking, and the ISR -> SPSC ring -> bottom half chain in both
 * the deterministic and the threaded interrupt mode (the latter is what
 * `make tsan` checks for races).
 */
#include "check.h"
#include "drivers/accel.h"
#include "drivers/gpio.h"
#include "drivers/spi.h"
#include "periph/board.h"

/* A controllable physical signal: either a constant, or a ramp that makes
 * sample k (produced at t = k * 1000 us at 1 kHz) read exactly k * step mg.
 * The four oversampling points of sample k all lie in ((k-1) ms, k ms], so
 * ceil(t / 1000) == k for each of them. */
typedef struct {
    double level;
    int    ramp;
    double step;
    int    wrap;     /* ramp modulo this many samples (0 = no wrap) */
} sig_t;

static double sig_source(void *ctx, uint64_t t_us)
{
    sig_t *s = ctx;
    if (!s->ramp)
        return s->level;
    uint64_t k = (t_us + 999) / 1000;
    if (s->wrap)
        k %= (uint64_t)s->wrap;
    return (double)k * s->step;
}

typedef struct {
    board_t         b;
    spi_drv_t       spi;
    gpio_drv_t      gpio;
    accel_drv_t     acc;
    ring_spsc_t     ring;
    accel_isr_ctx_t isr;
} rig_t;

static int rig_open(rig_t *r, irq_mode_t mode, sig_t *sig)
{
    memset(r, 0, sizeof(*r));
    if (board_init(&r->b, mode, sig_source, sig, 0.0, 1) != 0)
        return -1;
    if (spi_init(&r->spi, &r->b.bus, SPI0_BASE, 4) != 0 ||
        gpio_init(&r->gpio, &r->b.bus, GPIO0_BASE) != 0 ||
        accel_probe(&r->acc, &r->spi) != 0 || spsc_init(&r->ring, 64) != 0)
        return -1;
    r->isr.gpio = &r->gpio;
    r->isr.bus = &r->b.bus;
    r->isr.timer_base = TIMER0_BASE;
    r->isr.ring = &r->ring;
    irq_set_handler(&r->b.irq, IRQ_GPIO0, accel_isr, &r->isr);
    gpio_irq_config(&r->gpio, ACCEL_INT_PIN, 1, 0, 1);
    irq_enable(&r->b.irq, IRQ_GPIO0);
    return 0;
}

static void rig_close(rig_t *r)
{
    board_destroy(&r->b);         /* stops the interrupt thread first */
    spsc_destroy(&r->ring);
}

static unsigned drain_events(rig_t *r)
{
    unsigned n = 0;
    void *ev;
    while (spsc_try_pop(&r->ring, &ev))
        n++;
    return n;
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_mmio_decode_and_bus_error(void)
{
    sig_t sig = { 0 };
    rig_t r;
    CHECK_EQ(rig_open(&r, IRQ_MODE_DETERMINISTIC, &sig), 0);
    CHECK_EQ(mmio_read32(&r.b.bus, SPI0_BASE + SPI_ID), SPI_ID_VALUE);
    CHECK_EQ(mmio_read32(&r.b.bus, GPIO0_BASE + GPIO_ID), GPIO_ID_VALUE);
    uint64_t bad0 = mmio_bad_accesses(&r.b.bus);
    CHECK_EQ(mmio_read32(&r.b.bus, 0x50000000u), MMIO_BUS_ERROR);   /* unmapped */
    CHECK_EQ(mmio_read32(&r.b.bus, SPI0_BASE + 2), MMIO_BUS_ERROR); /* misaligned */
    mmio_write32(&r.b.bus, 0x50000000u, 1);
    CHECK_EQ(mmio_bad_accesses(&r.b.bus), bad0 + 3);
    /* Overlapping regions are refused. */
    CHECK(mmio_map(&r.b.bus, SPI0_BASE + 0x10, 0x10, spi_ctrl_read, spi_ctrl_write,
                   &r.b.spi, "dup") != 0);
    rig_close(&r);
}

static void test_spi_id_read(void)
{
    sig_t sig = { 0 };
    rig_t r;
    CHECK_EQ(rig_open(&r, IRQ_MODE_DETERMINISTIC, &sig), 0);
    /* rig_open already probed; read the IDs again explicitly. */
    CHECK_EQ(accel_read_reg(&r.acc, ACCEL_WHO_AM_I), ACCEL_ID_VALUE);
    CHECK_EQ(accel_read_reg(&r.acc, ACCEL_REV_ID), ACCEL_REV_VALUE);
    /* Two bytes per register read: command + data. */
    uint64_t before = r.spi.bytes;
    (void)accel_read_reg(&r.acc, ACCEL_WHO_AM_I);
    CHECK_EQ(r.spi.bytes - before, 2);
    /* With chip select released, nobody drives MISO: 0xFF. */
    CHECK_EQ(spi_xfer_byte(&r.spi, 0x80), 0xFF);
    rig_close(&r);
}

static void test_register_readback_and_soft_reset(void)
{
    sig_t sig = { 0 };
    rig_t r;
    CHECK_EQ(rig_open(&r, IRQ_MODE_DETERMINISTIC, &sig), 0);
    accel_cfg_t cfg = { ACCEL_ODR_1000HZ, 16, 0x1234, 3, ACCEL_INT_WTM };
    CHECK_EQ(accel_configure(&r.acc, &cfg), 0);
    uint8_t b[3];
    accel_read_regs(&r.acc, ACCEL_THS_L, b, 3);              /* auto-increment */
    CHECK_EQ(b[0], 0x34);
    CHECK_EQ(b[1], 0x12);
    CHECK_EQ(b[2], 3);
    CHECK_EQ(r.b.accel.reg[ACCEL_CTRL1] & ACCEL_CTRL1_PWR, ACCEL_CTRL1_PWR);
    accel_write_reg(&r.acc, ACCEL_CTRL1, ACCEL_CTRL1_SOFT_RESET);
    CHECK_EQ(accel_read_reg(&r.acc, ACCEL_THS_L), 0);
    CHECK_EQ(accel_read_reg(&r.acc, ACCEL_CTRL1) & ACCEL_CTRL1_PWR, 0);
    rig_close(&r);
}

static void test_fifo_order(void)
{
    sig_t sig = { 0, 1, 10.0, 0 };
    rig_t r;
    CHECK_EQ(rig_open(&r, IRQ_MODE_DETERMINISTIC, &sig), 0);
    accel_cfg_t cfg = { ACCEL_ODR_1000HZ, 0, 0, 0, 0 };
    CHECK_EQ(accel_configure(&r.acc, &cfg), 0);
    board_advance(&r.b, 40000, NULL, NULL);                  /* 40 samples */
    CHECK_EQ(accel_fifo_level(&r.acc), 40);
    CHECK_EQ(accel_sample_count(&r.acc), 40);
    int16_t s[128];
    CHECK_EQ(accel_fifo_read(&r.acc, s, 25), 25);            /* partial burst */
    CHECK_EQ(accel_fifo_read(&r.acc, s + 25, 128), 15);
    int in_order = 1;
    for (int k = 0; k < 40; k++)
        in_order &= s[k] == (int16_t)((k + 1) * 10);
    CHECK(in_order);
    CHECK_EQ(accel_fifo_level(&r.acc), 0);
    CHECK(accel_read_reg(&r.acc, ACCEL_FIFO_FLAGS) & ACCEL_FLAG_EMPTY);

    /* Overflow in stream mode: 200 more samples into a 128-deep FIFO keeps
     * the newest 128 (samples 113..240) and sets OVR. */
    board_advance(&r.b, 240000, NULL, NULL);
    CHECK_EQ(accel_fifo_level(&r.acc), ACCEL_FIFO_DEPTH);
    CHECK(accel_read_reg(&r.acc, ACCEL_FIFO_FLAGS) & ACCEL_FLAG_OVR);
    CHECK_EQ(accel_fifo_read(&r.acc, s, 128), 128);
    CHECK_EQ(s[0], 1130);
    CHECK_EQ(s[127], 2400);
    CHECK_EQ(r.b.accel.fifo_overruns, 72);
    CHECK(accel_int_source(&r.acc) & ACCEL_INT_OVR);         /* read-to-clear */
    CHECK_EQ(accel_int_source(&r.acc) & ACCEL_INT_OVR, 0);
    rig_close(&r);
}

static void test_gpio_edge_interrupt_fires_once(void)
{
    /* Non-latched shock threshold: the INT pin is HIGH for as long as the
     * signal stays over threshold. Edge triggering must give ONE interrupt
     * for the whole episode, not one per sample. */
    sig_t sig = { 500.0, 0, 0, 0 };
    rig_t r;
    CHECK_EQ(rig_open(&r, IRQ_MODE_DETERMINISTIC, &sig), 0);
    accel_cfg_t cfg = { ACCEL_ODR_1000HZ, 0, 100, 1, ACCEL_INT_SHOCK };
    CHECK_EQ(accel_configure(&r.acc, &cfg), 0);
    board_advance(&r.b, 50000, NULL, NULL);
    CHECK_EQ(irq_delivered(&r.b.irq, IRQ_GPIO0), 1);
    CHECK_EQ(drain_events(&r), 1);
    CHECK(gpio_read_inputs(&r.gpio) & (1u << ACCEL_INT_PIN));   /* still high */

    sig.level = 0.0;                      /* pin falls: rising-only, no IRQ */
    board_advance(&r.b, 60000, NULL, NULL);
    CHECK_EQ(irq_delivered(&r.b.irq, IRQ_GPIO0), 1);
    CHECK_EQ(gpio_read_inputs(&r.gpio) & (1u << ACCEL_INT_PIN), 0);
    sig.level = -800.0;                   /* |sample| over threshold again */
    board_advance(&r.b, 70000, NULL, NULL);
    CHECK_EQ(irq_delivered(&r.b.irq, IRQ_GPIO0), 2);

    /* Directly at the GPIO model: repeated "high" is not an edge. */
    gpio_ctrl_t g;
    irq_ctrl_t ic;
    CHECK_EQ(irq_init(&ic, IRQ_MODE_DETERMINISTIC), 0);
    gpio_ctrl_init(&g, &ic, 1);
    gpio_ctrl_write(&g, GPIO_IRQ_RISE, 1u << 2);
    gpio_ctrl_write(&g, GPIO_IRQ_EN, 1u << 2);
    for (int i = 0; i < 5; i++)
        gpio_ctrl_set_input(&g, 2, 1);
    CHECK_EQ(g.edges, 1);
    CHECK_EQ(gpio_ctrl_read(&g, GPIO_IRQ_STATUS), 1u << 2);
    gpio_ctrl_write(&g, GPIO_IRQ_STATUS, 1u << 2);          /* W1C */
    CHECK_EQ(gpio_ctrl_read(&g, GPIO_IRQ_STATUS), 0);
    irq_destroy(&ic);
    rig_close(&r);
}

static void test_masked_interrupt_stays_pending(void)
{
    sig_t sig = { 0.0, 0, 0, 0 };
    rig_t r;
    CHECK_EQ(rig_open(&r, IRQ_MODE_DETERMINISTIC, &sig), 0);
    accel_cfg_t cfg = { ACCEL_ODR_1000HZ, 0, 100, 1, ACCEL_INT_SHOCK | ACCEL_INT_LATCH };
    CHECK_EQ(accel_configure(&r.acc, &cfg), 0);
    irq_mask(&r.b.irq, IRQ_GPIO0);
    sig.level = 1000.0;
    board_advance(&r.b, 10000, NULL, NULL);
    CHECK_EQ(irq_delivered(&r.b.irq, IRQ_GPIO0), 0);         /* blocked ...  */
    CHECK_EQ(irq_is_pending(&r.b.irq, IRQ_GPIO0), 1);        /* ... not lost */
    CHECK_EQ(irq_dispatch(&r.b.irq), 0);
    CHECK_EQ(irq_is_pending(&r.b.irq, IRQ_GPIO0), 1);
    irq_unmask(&r.b.irq, IRQ_GPIO0);
    CHECK_EQ(irq_dispatch(&r.b.irq), 1);
    CHECK_EQ(irq_delivered(&r.b.irq, IRQ_GPIO0), 1);
    CHECK_EQ(irq_is_pending(&r.b.irq, IRQ_GPIO0), 0);
    /* The ISR saw the edge bit and timestamped it at the first sample. */
    void *ev = NULL;
    CHECK(spsc_try_pop(&r.ring, &ev));
    CHECK_EQ(accel_ev_bits((uintptr_t)ev), 1u << ACCEL_INT_PIN);
    CHECK_EQ(accel_ev_time_us((uintptr_t)ev), 10000);   /* serviced at unmask time */
    /* A disabled line also keeps its pending bit. */
    CHECK(accel_int_source(&r.acc) & ACCEL_INT_SHOCK);   /* clear latch: pin low */
    irq_disable(&r.b.irq, IRQ_GPIO0);
    board_advance(&r.b, 11000, NULL, NULL);              /* re-latches: new edge */
    CHECK_EQ(irq_is_pending(&r.b.irq, IRQ_GPIO0), 1);
    CHECK_EQ(irq_delivered(&r.b.irq, IRQ_GPIO0), 1);
    rig_close(&r);
}

/* Bottom half used by the chain tests: on each event read INT_SRC and
 * drain the FIFO; check every sample is the next one of the ramp. */
typedef struct {
    rig_t   *r;
    unsigned got, events, bad_order;
    int      expect;
} bh_t;

static void bottom_half(bh_t *h)
{
    void *ev;
    while (spsc_try_pop(&h->r->ring, &ev)) {
        h->events++;
        (void)accel_int_source(&h->r->acc);
        int16_t s[ACCEL_FIFO_DEPTH];
        unsigned n = accel_fifo_read(&h->r->acc, s, ACCEL_FIFO_DEPTH);
        for (unsigned i = 0; i < n; i++) {
            if (s[i] < h->expect)            /* must be strictly increasing */
                h->bad_order++;
            h->expect = s[i] + 1;
        }
        h->got += n;
    }
}

static void on_tick(void *ctx, uint64_t t_us)
{
    (void)t_us;
    bottom_half(ctx);
}

static void test_watermark_isr_bottom_half(void)
{
    sig_t sig = { 0, 1, 1.0, 30000 };
    rig_t r;
    CHECK_EQ(rig_open(&r, IRQ_MODE_DETERMINISTIC, &sig), 0);
    accel_cfg_t cfg = { ACCEL_ODR_1000HZ, 16, 0, 0, ACCEL_INT_WTM };
    CHECK_EQ(accel_configure(&r.acc, &cfg), 0);
    bh_t h = { &r, 0, 0, 0, 1 };
    board_advance(&r.b, 1600000, on_tick, &h);          /* 1600 samples */
    CHECK_EQ(h.events, 100);                            /* one per 16 samples */
    CHECK_EQ(h.got, 1600);
    CHECK_EQ(h.bad_order, 0);
    CHECK_EQ(r.b.accel.fifo_overruns, 0);
    CHECK_EQ(ctr_get(&r.isr.spurious), 0);
    CHECK_EQ(ctr_get(&r.isr.drops), 0);
    rig_close(&r);
}

static void test_threaded_irq_chain(void)
{
    /* The ISR runs on the interrupt thread, the bottom half on this one.
     * Under TSan this is the race check of the whole peripheral layer. */
    sig_t sig = { 0, 1, 1.0, 30000 };
    rig_t r;
    CHECK_EQ(rig_open(&r, IRQ_MODE_THREADED, &sig), 0);
    accel_cfg_t cfg = { ACCEL_ODR_1000HZ, 16, 0, 0, ACCEL_INT_WTM };
    CHECK_EQ(accel_configure(&r.acc, &cfg), 0);
    bh_t h = { &r, 0, 0, 0, 1 };
    for (uint64_t t = 1000; t <= 6000000; t += 1000) {
        board_advance(&r.b, t, NULL, NULL);
        bottom_half(&h);
    }
    irq_quiesce(&r.b.irq);
    bottom_half(&h);
    /* Drain whatever is left below the watermark. */
    int16_t s[ACCEL_FIFO_DEPTH];
    unsigned n = accel_fifo_read(&r.acc, s, ACCEL_FIFO_DEPTH);
    for (unsigned i = 0; i < n; i++) {
        h.bad_order += s[i] < h.expect;
        h.expect = s[i] + 1;
    }
    h.got += n;
    pthread_mutex_lock(&r.b.bus.lock);
    uint64_t lost = r.b.accel.fifo_overruns;
    pthread_mutex_unlock(&r.b.bus.lock);
    CHECK_EQ(h.got + lost, 6000);                      /* nothing vanishes */
    CHECK_EQ(h.bad_order, 0);
    CHECK(h.events > 0);
    CHECK_EQ(ctr_get(&r.isr.runs), irq_delivered(&r.b.irq, IRQ_GPIO0));
    rig_close(&r);
}

int main(void)
{
    printf("test_drivers\n");
    RUN_TEST(test_mmio_decode_and_bus_error);
    RUN_TEST(test_spi_id_read);
    RUN_TEST(test_register_readback_and_soft_reset);
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_gpio_edge_interrupt_fires_once);
    RUN_TEST(test_masked_interrupt_stays_pending);
    RUN_TEST(test_watermark_isr_bottom_half);
    RUN_TEST(test_threaded_irq_chain);
    return test_summary("test_drivers");
}
