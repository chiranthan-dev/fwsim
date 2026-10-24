/*
 * gpio.h - register-level driver for GPIO0 (periph/gpio_ctrl.h).
 */
#ifndef FWSIM_DRIVERS_GPIO_H
#define FWSIM_DRIVERS_GPIO_H

#include <stdint.h>

#include "hal/mmio.h"

typedef struct {
    mmio_bus_t *bus;
    uint32_t    base;
} gpio_drv_t;

int      gpio_init(gpio_drv_t *d, mmio_bus_t *bus, uint32_t base);  /* 0 / -1 */
/* Configure a pin's interrupt: which edges, and enable it. */
void     gpio_irq_config(gpio_drv_t *d, unsigned pin, int rising, int falling, int enable);
uint32_t gpio_read_inputs(gpio_drv_t *d);
/* ISR helper: read the edge status and clear exactly the bits read (W1C),
 * so an edge that lands between the read and the write is not lost. */
uint32_t gpio_irq_take(gpio_drv_t *d);

#endif
