/*
 * gpio.c - GPIO0 driver.
 */
#include "drivers/gpio.h"

#include "periph/gpio_ctrl.h"

int gpio_init(gpio_drv_t *d, mmio_bus_t *bus, uint32_t base)
{
    d->bus = bus;
    d->base = base;
    if (mmio_read32(bus, base + GPIO_ID) != GPIO_ID_VALUE)
        return -1;
    mmio_write32(bus, base + GPIO_IRQ_EN, 0);
    mmio_write32(bus, base + GPIO_IRQ_STATUS, 0xFFFFFFFFu);   /* clear stale edges */
    return 0;
}

/* Read-modify-write of one bit. Only the firmware configures pins, at
 * init, so the RMW needs no lock of its own. */
static void set_bit(gpio_drv_t *d, uint32_t off, unsigned pin, int on)
{
    uint32_t v = mmio_read32(d->bus, d->base + off);
    v = on ? (v | (1u << pin)) : (v & ~(1u << pin));
    mmio_write32(d->bus, d->base + off, v);
}

void gpio_irq_config(gpio_drv_t *d, unsigned pin, int rising, int falling, int enable)
{
    set_bit(d, GPIO_IRQ_EN, pin, 0);                 /* quiet while changing */
    set_bit(d, GPIO_IRQ_RISE, pin, rising);
    set_bit(d, GPIO_IRQ_FALL, pin, falling);
    mmio_write32(d->bus, d->base + GPIO_IRQ_STATUS, 1u << pin);
    set_bit(d, GPIO_IRQ_EN, pin, enable);
}

uint32_t gpio_read_inputs(gpio_drv_t *d)
{
    return mmio_read32(d->bus, d->base + GPIO_IN);
}

uint32_t gpio_irq_take(gpio_drv_t *d)
{
    uint32_t st = mmio_read32(d->bus, d->base + GPIO_IRQ_STATUS);
    if (st)
        mmio_write32(d->bus, d->base + GPIO_IRQ_STATUS, st);
    return st;
}
