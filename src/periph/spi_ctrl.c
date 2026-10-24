/*
 * spi_ctrl.c - SPI master controller model. Register map in spi_ctrl.h.
 */
#include "periph/spi_ctrl.h"

#include <string.h>

void spi_ctrl_init(spi_ctrl_t *s, const spi_slave_t *slave)
{
    memset(s, 0, sizeof(*s));
    if (slave)
        s->slave = *slave;
}

static int selected(const spi_ctrl_t *s)
{
    return (s->ctrl & (SPI_CTRL_EN | SPI_CTRL_CS)) == (SPI_CTRL_EN | SPI_CTRL_CS);
}

uint32_t spi_ctrl_read(void *dev, uint32_t off)
{
    spi_ctrl_t *s = dev;
    switch (off) {
    case SPI_CTRL:   return s->ctrl;
    case SPI_STATUS: return s->status;
    case SPI_DATA:
        s->status &= ~SPI_STATUS_RXNE;
        return s->rx;
    case SPI_ID:     return SPI_ID_VALUE;
    case SPI_XFERS:  return s->xfers;
    default:         return 0;          /* reserved: reads as zero */
    }
}

void spi_ctrl_write(void *dev, uint32_t off, uint32_t v)
{
    spi_ctrl_t *s = dev;
    switch (off) {
    case SPI_CTRL: {
        int was = selected(s);
        s->ctrl = v & (SPI_CTRL_EN | SPI_CTRL_CS | 0xFF00u);
        int now = selected(s);
        if (was != now && s->slave.select)
            s->slave.select(s->slave.dev, now);    /* CS edge */
        break;
    }
    case SPI_DATA:
        s->rx = (selected(s) && s->slave.xfer) ? s->slave.xfer(s->slave.dev, (uint8_t)v)
                                               : 0xFFu;
        s->status |= SPI_STATUS_RXNE;
        s->xfers++;
        break;
    default:
        break;                              /* RO / reserved: ignored */
    }
}
