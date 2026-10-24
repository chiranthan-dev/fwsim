/*
 * gpio_ctrl.c - GPIO controller model. Register map in gpio_ctrl.h.
 */
#include "periph/gpio_ctrl.h"

#include <string.h>

#define PIN_MASK ((1u << GPIO_PINS) - 1u)

void gpio_ctrl_init(gpio_ctrl_t *g, irq_ctrl_t *irq, unsigned irq_line)
{
    memset(g, 0, sizeof(*g));
    g->irq = irq;
    g->irq_line = irq_line;
}

void gpio_ctrl_set_input(gpio_ctrl_t *g, unsigned pin, int level)
{
    if (pin >= GPIO_PINS)
        return;
    uint32_t bit = 1u << pin;
    uint32_t old = g->in;
    g->in = level ? (old | bit) : (old & ~bit);
    if (old == g->in)
        return;                                 /* no edge */
    uint32_t rose = g->in & ~old, fell = old & ~g->in;
    uint32_t hit = ((rose & g->rise) | (fell & g->fall)) & g->en;
    if (!hit)
        return;
    g->status |= hit;
    g->edges++;
    if (g->irq)
        irq_raise(g->irq, g->irq_line);
}

uint32_t gpio_ctrl_read(void *dev, uint32_t off)
{
    gpio_ctrl_t *g = dev;
    switch (off) {
    case GPIO_IN:         return g->in;
    case GPIO_IRQ_EN:     return g->en;
    case GPIO_IRQ_RISE:   return g->rise;
    case GPIO_IRQ_FALL:   return g->fall;
    case GPIO_IRQ_STATUS: return g->status;
    case GPIO_ID:         return GPIO_ID_VALUE;
    default:              return 0;
    }
}

void gpio_ctrl_write(void *dev, uint32_t off, uint32_t v)
{
    gpio_ctrl_t *g = dev;
    switch (off) {
    case GPIO_IRQ_EN:     g->en = v & PIN_MASK; break;
    case GPIO_IRQ_RISE:   g->rise = v & PIN_MASK; break;
    case GPIO_IRQ_FALL:   g->fall = v & PIN_MASK; break;
    case GPIO_IRQ_STATUS: g->status &= ~v; break;      /* write-1-to-clear */
    default:              break;
    }
}
