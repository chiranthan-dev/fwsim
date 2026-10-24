/*
 * gpio_ctrl.h - device model of a 16-pin GPIO input controller with edge
 * interrupts.
 *
 * ORIGINAL register map, 32-bit registers, one bit per pin:
 *
 *   off   name             access  meaning
 *   0x00  GPIO_IN          RO      current input levels
 *   0x04  GPIO_IRQ_EN      RW      pin may raise an interrupt
 *   0x08  GPIO_IRQ_RISE    RW      a rising edge sets the status bit
 *   0x0C  GPIO_IRQ_FALL    RW      a falling edge sets the status bit
 *   0x10  GPIO_IRQ_STATUS  W1C     edge seen; write 1 to clear
 *   0x14  GPIO_ID          RO      0x4750494F ("GPIO")
 *
 * Edge detection runs on every input change (the pins are driven by other
 * device models through gpio_ctrl_set_input - a wire, not a bus access).
 * A selected edge on an enabled pin sets its status bit and raises the
 * controller's interrupt line. A level that merely STAYS high produces no
 * further edges: one interrupt per edge, which is the point of edge
 * triggering (a level-triggered line would re-fire until the source is
 * cleared).
 */
#ifndef FWSIM_PERIPH_GPIO_CTRL_H
#define FWSIM_PERIPH_GPIO_CTRL_H

#include <stdint.h>

#include "hal/irq.h"

#define GPIO_IN          0x00u
#define GPIO_IRQ_EN      0x04u
#define GPIO_IRQ_RISE    0x08u
#define GPIO_IRQ_FALL    0x0Cu
#define GPIO_IRQ_STATUS  0x10u
#define GPIO_ID          0x14u
#define GPIO_REG_SPAN    0x20u
#define GPIO_ID_VALUE    0x4750494Fu
#define GPIO_PINS        16u

typedef struct {
    uint32_t    in, en, rise, fall, status;
    irq_ctrl_t *irq;
    unsigned    irq_line;
    uint64_t    edges;            /* qualifying edges seen (debug)        */
} gpio_ctrl_t;

void     gpio_ctrl_init(gpio_ctrl_t *g, irq_ctrl_t *irq, unsigned irq_line);
void     gpio_ctrl_set_input(gpio_ctrl_t *g, unsigned pin, int level);
uint32_t gpio_ctrl_read(void *dev, uint32_t off);
void     gpio_ctrl_write(void *dev, uint32_t off, uint32_t v);

#endif
