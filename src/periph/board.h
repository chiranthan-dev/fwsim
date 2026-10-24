/*
 * board.h - the simulated controller board: which device sits at which
 * address, which pin is wired to what, and the simulated sample clock.
 *
 *   memory map (original)           wiring
 *   0x4001_0000  SPI0   (spi_ctrl)  SPI0 <-> accelerometer (CS0)
 *   0x4002_0000  GPIO0  (gpio_ctrl) accelerometer INT -> GPIO0 pin 3
 *   0x4003_0000  TIMER0 (timer)     GPIO0 interrupt   -> IRQ line 7
 *
 * TIMER0 is a free-running microsecond counter of SIMULATED time:
 *   0x00 TIMER_LO  low 32 bits (reading it latches the high half)
 *   0x04 TIMER_HI  high 32 bits, as latched by the last TIMER_LO read
 *
 * Time only moves when the firmware calls board_advance(): every
 * accelerometer sample that falls due up to that instant is produced in
 * order, and after each one (deterministic interrupt mode) the pending
 * ISRs run and the caller's bottom-half hook is invoked. So an interrupt
 * is serviced "at" the sample that caused it, regardless of how coarsely
 * the firmware happens to call board_advance - and regardless of the
 * time_scale.
 */
#ifndef FWSIM_PERIPH_BOARD_H
#define FWSIM_PERIPH_BOARD_H

#include <stdint.h>

#include "hal/irq.h"
#include "hal/mmio.h"
#include "periph/accel_model.h"
#include "periph/gpio_ctrl.h"
#include "periph/spi_ctrl.h"

#define SPI0_BASE        0x40010000u
#define GPIO0_BASE       0x40020000u
#define TIMER0_BASE      0x40030000u
#define TIMER_LO         0x00u
#define TIMER_HI         0x04u
#define TIMER_REG_SPAN   0x10u
#define IRQ_GPIO0        7u
#define ACCEL_INT_PIN    3u

typedef struct {
    uint64_t now_us;
    uint32_t latched_hi;
} timer_dev_t;

typedef struct {
    mmio_bus_t    bus;
    irq_ctrl_t    irq;
    spi_ctrl_t    spi;
    gpio_ctrl_t   gpio;
    accel_model_t accel;
    timer_dev_t   timer;
    uint64_t      next_sample_us;   /* when the next accelerometer sample is due */
    uint64_t      samples;
} board_t;

/* src provides the physical acceleration; noise_mg / seed configure the
 * sensor. 0 or -1. */
int  board_init(board_t *b, irq_mode_t mode, accel_source_fn src, void *src_ctx,
                double noise_mg, uint64_t seed);
void board_destroy(board_t *b);

/* Hook run after each sample tick (after any deterministic ISRs). */
typedef void (*board_tick_fn)(void *ctx, uint64_t t_us);

/* Advance simulated time to t_us (never backwards). */
void board_advance(board_t *b, uint64_t t_us, board_tick_fn after_tick, void *ctx);

uint64_t board_now_us(board_t *b);

#endif
