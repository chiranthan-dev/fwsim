/*
 * spi.c - SPI0 master driver: everything goes through mmio_read32 /
 * mmio_write32, exactly as it would on silicon.
 */
#include "drivers/spi.h"

#include "periph/spi_ctrl.h"

/* Bound every poll loop: firmware must never hang on a dead peripheral. */
#define SPI_POLL_LIMIT 100000u

int spi_init(spi_drv_t *d, mmio_bus_t *bus, uint32_t base, uint8_t clkdiv)
{
    d->bus = bus;
    d->base = base;
    d->bytes = 0;
    if (mmio_read32(bus, base + SPI_ID) != SPI_ID_VALUE)
        return -1;
    mmio_write32(bus, base + SPI_CTRL, SPI_CTRL_EN | (uint32_t)clkdiv << 8);
    return 0;
}

void spi_select(spi_drv_t *d, int on)
{
    uint32_t c = mmio_read32(d->bus, d->base + SPI_CTRL);
    c = on ? (c | SPI_CTRL_CS) : (c & ~SPI_CTRL_CS);
    mmio_write32(d->bus, d->base + SPI_CTRL, c);
}

uint8_t spi_xfer_byte(spi_drv_t *d, uint8_t tx)
{
    mmio_write32(d->bus, d->base + SPI_DATA, tx);
    /* Wait for the byte to finish shifting: BUSY clear and RXNE set. */
    for (unsigned i = 0; i < SPI_POLL_LIMIT; i++) {
        uint32_t st = mmio_read32(d->bus, d->base + SPI_STATUS);
        if (!(st & SPI_STATUS_BUSY) && (st & SPI_STATUS_RXNE))
            break;
    }
    d->bytes++;
    return (uint8_t)mmio_read32(d->bus, d->base + SPI_DATA);
}

void spi_transfer(spi_drv_t *d, const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_select(d, 1);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = spi_xfer_byte(d, tx ? tx[i] : 0x00u);
        if (rx)
            rx[i] = b;
    }
    spi_select(d, 0);
}
