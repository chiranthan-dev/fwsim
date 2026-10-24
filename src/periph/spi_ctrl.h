/*
 * spi_ctrl.h - device model of a minimal SPI master controller.
 *
 * ORIGINAL register map (not any vendor's part), 32-bit registers:
 *
 *   off   name        access  bits
 *   0x00  SPI_CTRL    RW      [0] EN   [1] CS (1 = chip select asserted)
 *                             [15:8] CLKDIV (stored; the model is untimed)
 *   0x04  SPI_STATUS  RO      [0] BUSY  [1] RXNE (received byte waiting)
 *   0x08  SPI_DATA    RW      write: send a byte (full duplex: one byte
 *                             comes back from the slave at the same time)
 *                             read:  the received byte, clears RXNE
 *   0x0C  SPI_ID      RO      0x53504930 ("SPI0")
 *   0x10  SPI_XFERS   RO      bytes exchanged since reset (debug counter)
 *
 * One slave hangs off the controller (the accelerometer). A byte written
 * while EN=0 or CS=0 goes nowhere and 0xFF is received, as on a real bus
 * with nobody driving MISO (pull-up).
 *
 * The model completes each byte instantly, so BUSY always reads 0; the
 * driver still polls it, as it must on hardware. Bus time is NOT charged
 * to the simulated clock; README "Cost" gives the SPI time analytically.
 */
#ifndef FWSIM_PERIPH_SPI_CTRL_H
#define FWSIM_PERIPH_SPI_CTRL_H

#include <stdint.h>

#define SPI_CTRL    0x00u
#define SPI_STATUS  0x04u
#define SPI_DATA    0x08u
#define SPI_ID      0x0Cu
#define SPI_XFERS   0x10u
#define SPI_REG_SPAN 0x20u

#define SPI_CTRL_EN      (1u << 0)
#define SPI_CTRL_CS      (1u << 1)
#define SPI_STATUS_BUSY  (1u << 0)
#define SPI_STATUS_RXNE  (1u << 1)
#define SPI_ID_VALUE     0x53504930u

/* What a slave device implements: chip-select edges and byte exchange. */
typedef struct {
    void   (*select)(void *dev, int active);
    uint8_t (*xfer)(void *dev, uint8_t mosi);    /* returns MISO byte */
    void    *dev;
} spi_slave_t;

typedef struct {
    uint32_t    ctrl;
    uint32_t    status;
    uint8_t     rx;
    uint32_t    xfers;
    spi_slave_t slave;
} spi_ctrl_t;

void     spi_ctrl_init(spi_ctrl_t *s, const spi_slave_t *slave);
uint32_t spi_ctrl_read(void *dev, uint32_t off);              /* mmio_read_fn  */
void     spi_ctrl_write(void *dev, uint32_t off, uint32_t v); /* mmio_write_fn */

#endif
