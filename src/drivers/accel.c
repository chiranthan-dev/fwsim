/*
 * accel.c - SIMACC-1 driver and ISR. Design notes in accel.h; register
 * map in periph/accel_model.h.
 */
#include "drivers/accel.h"

#include "periph/accel_model.h"
#include "periph/board.h"

#define CMD_READ  0x80u
#define CMD_MB    0x40u

int accel_probe(accel_drv_t *d, spi_drv_t *spi)
{
    d->spi = spi;
    return accel_read_reg(d, ACCEL_WHO_AM_I) == ACCEL_ID_VALUE ? 0 : -1;
}

uint8_t accel_read_reg(accel_drv_t *d, uint8_t addr)
{
    uint8_t tx[2] = { (uint8_t)(CMD_READ | (addr & 0x3Fu)), 0 }, rx[2];
    spi_transfer(d->spi, tx, rx, 2);
    return rx[1];
}

void accel_write_reg(accel_drv_t *d, uint8_t addr, uint8_t v)
{
    uint8_t tx[2] = { (uint8_t)(addr & 0x3Fu), v };
    spi_transfer(d->spi, tx, NULL, 2);
}

void accel_read_regs(accel_drv_t *d, uint8_t addr, uint8_t *buf, unsigned n)
{
    spi_select(d->spi, 1);
    spi_xfer_byte(d->spi, (uint8_t)(CMD_READ | CMD_MB | (addr & 0x3Fu)));
    for (unsigned i = 0; i < n; i++)
        buf[i] = spi_xfer_byte(d->spi, 0);
    spi_select(d->spi, 0);
}

int accel_configure(accel_drv_t *d, const accel_cfg_t *c)
{
    accel_write_reg(d, ACCEL_CTRL1, ACCEL_CTRL1_SOFT_RESET);
    accel_write_reg(d, ACCEL_INT_CFG, 0);               /* pin quiet while we set up */
    accel_write_reg(d, ACCEL_THS_L, (uint8_t)c->shock_mg);
    accel_write_reg(d, ACCEL_THS_H, (uint8_t)(c->shock_mg >> 8));
    accel_write_reg(d, ACCEL_THS_DUR, c->shock_dur);
    accel_write_reg(d, ACCEL_FIFO_WTM, c->fifo_wtm);
    accel_write_reg(d, ACCEL_CTRL2, ACCEL_CTRL2_FIFO_EN | ACCEL_CTRL2_FIFO_FLUSH);

    /* Read back before enabling anything: a miswired or absent part shows
     * up here, not as mysteriously missing interrupts later. */
    if (accel_read_reg(d, ACCEL_THS_L) != (uint8_t)c->shock_mg ||
        accel_read_reg(d, ACCEL_THS_H) != (uint8_t)(c->shock_mg >> 8) ||
        accel_read_reg(d, ACCEL_FIFO_WTM) != c->fifo_wtm ||
        accel_read_reg(d, ACCEL_CTRL2) != ACCEL_CTRL2_FIFO_EN)
        return -1;

    (void)accel_int_source(d);                          /* clear anything stale */
    accel_write_reg(d, ACCEL_INT_CFG, c->int_cfg);
    uint8_t ctrl1 = (uint8_t)(ACCEL_CTRL1_PWR | ((c->odr_code & 7u) << ACCEL_CTRL1_ODR_SHIFT));
    accel_write_reg(d, ACCEL_CTRL1, ctrl1);
    return accel_read_reg(d, ACCEL_CTRL1) == ctrl1 ? 0 : -1;
}

uint8_t accel_int_source(accel_drv_t *d)
{
    return accel_read_reg(d, ACCEL_INT_SRC);
}

unsigned accel_fifo_level(accel_drv_t *d)
{
    return accel_read_reg(d, ACCEL_FIFO_LEVEL);
}

unsigned accel_fifo_read(accel_drv_t *d, int16_t *out, unsigned max)
{
    unsigned n = accel_fifo_level(d);
    if (n > max)
        n = max;
    if (n == 0)
        return 0;
    /* One frame: command byte, then 2 bytes per sample. FIFO_DATA does not
     * auto-increment, so the whole burst streams the FIFO. */
    spi_select(d->spi, 1);
    spi_xfer_byte(d->spi, (uint8_t)(CMD_READ | CMD_MB | ACCEL_FIFO_DATA));
    for (unsigned i = 0; i < n; i++) {
        uint8_t lo = spi_xfer_byte(d->spi, 0);
        uint8_t hi = spi_xfer_byte(d->spi, 0);
        out[i] = (int16_t)(uint16_t)((uint16_t)lo | (uint16_t)hi << 8);
    }
    spi_select(d->spi, 0);
    return n;
}

uint32_t accel_sample_count(accel_drv_t *d)
{
    uint8_t b[4];
    accel_read_regs(d, ACCEL_SAMPLE_CNT0, b, 4);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* ---- ISR --------------------------------------------------------------------------- */

void accel_isr(void *ctx)
{
    accel_isr_ctx_t *c = ctx;
    ctr_add(&c->runs, 1);
    uint32_t bits = gpio_irq_take(c->gpio);         /* read + W1C */
    if (!bits) {
        ctr_add(&c->spurious, 1);
        return;
    }
    uint32_t lo = mmio_read32(c->bus, c->timer_base + TIMER_LO);
    uint32_t hi = mmio_read32(c->bus, c->timer_base + TIMER_HI);
    uint64_t t = (uint64_t)hi << 32 | lo;
    if (!spsc_try_push(c->ring, (void *)accel_ev_pack(t, bits)))
        ctr_add(&c->drops, 1);
}
