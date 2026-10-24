/*
 * spi.h - register-level driver for the SPI0 master (periph/spi_ctrl.h).
 * Polled, blocking, one chip select: the accelerometer is the only slave,
 * and its transfers are a few dozen bytes.
 */
#ifndef FWSIM_DRIVERS_SPI_H
#define FWSIM_DRIVERS_SPI_H

#include <stddef.h>
#include <stdint.h>

#include "hal/mmio.h"

typedef struct {
    mmio_bus_t *bus;
    uint32_t    base;
    uint64_t    bytes;          /* bytes exchanged by this driver */
} spi_drv_t;

/* Check the ID register, enable the controller. 0, or -1 if absent. */
int  spi_init(spi_drv_t *d, mmio_bus_t *bus, uint32_t base, uint8_t clkdiv);
void spi_select(spi_drv_t *d, int on);
uint8_t spi_xfer_byte(spi_drv_t *d, uint8_t tx);
/* One frame: CS low, n bytes out of tx (NULL = 0x00s), into rx (NULL =
 * discard), CS high. */
void spi_transfer(spi_drv_t *d, const uint8_t *tx, uint8_t *rx, size_t n);

#endif
