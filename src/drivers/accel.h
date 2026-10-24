/*
 * accel.h - driver for the SIMACC-1 accelerometer (periph/accel_model.h)
 * over SPI0, plus its interrupt service routine.
 *
 * Top half / bottom half:
 *
 *   accel INT pin -> GPIO0 edge -> IRQ_GPIO0 -> accel_isr()        [top half]
 *        accel_isr: read + clear GPIO0's edge status, read TIMER0,
 *                   push ONE packed event onto an SPSC ring. Nothing else:
 *                   no SPI traffic, no FIFO drain, no classification.
 *   firmware loop -> pops events -> accel_int_source() over SPI,
 *                   accel_fifo_read() of the whole FIFO, features, model,
 *                   policy                                     [bottom half]
 *
 * WHY so little in the ISR: an ISR runs with (at least) its own line
 * blocked and preempts the firmware's real work; every microsecond in it
 * is latency added to everything else, and SPI transfers are slow (a
 * 16-sample FIFO burst is ~33 bytes = ~33 us at 8 MHz). The ISR only
 * records THAT something happened and WHEN; the expensive part runs in
 * thread context where it can be scheduled, and where it may block on
 * the bus without stalling other interrupts.
 *
 * WHY an SPSC ring between them: exactly one producer (the ISR) and one
 * consumer (the firmware loop), so ring_spsc.h's lock-free ring applies
 * unchanged, and an ISR must never take a lock the interrupted code might
 * hold. ring_spsc carries a pointer-sized word, so the event is packed
 * into it rather than pointing at storage that the ISR could overwrite
 * while the consumer is still reading it.
 */
#ifndef FWSIM_DRIVERS_ACCEL_H
#define FWSIM_DRIVERS_ACCEL_H

#include <stdint.h>

#include "drivers/gpio.h"
#include "drivers/spi.h"
#include "ring_spsc.h"
#include "util.h"

typedef struct {
    spi_drv_t *spi;
} accel_drv_t;

typedef struct {
    uint8_t  odr_code;      /* ACCEL_ODR_* code for CTRL1                */
    uint8_t  fifo_wtm;      /* samples; 0 = no watermark interrupt       */
    uint16_t shock_mg;      /* threshold; 0 = threshold disabled          */
    uint8_t  shock_dur;     /* consecutive samples                         */
    uint8_t  int_cfg;       /* ACCEL_INT_* enable bits (+ LATCH / POL)    */
} accel_cfg_t;

int      accel_probe(accel_drv_t *d, spi_drv_t *spi);   /* 0 if WHO_AM_I matches */
uint8_t  accel_read_reg(accel_drv_t *d, uint8_t addr);
void     accel_write_reg(accel_drv_t *d, uint8_t addr, uint8_t v);
void     accel_read_regs(accel_drv_t *d, uint8_t addr, uint8_t *buf, unsigned n);
/* Soft reset, program every register, verify by read-back, start
 * sampling. 0, or -1 if a register did not read back. */
int      accel_configure(accel_drv_t *d, const accel_cfg_t *cfg);
uint8_t  accel_int_source(accel_drv_t *d);        /* reading clears latched bits */
unsigned accel_fifo_level(accel_drv_t *d);
/* Drain up to max samples (oldest first) in ONE SPI burst. Returns count. */
unsigned accel_fifo_read(accel_drv_t *d, int16_t *out, unsigned max);
uint32_t accel_sample_count(accel_drv_t *d);

/* ---- interrupt service routine ------------------------------------------------ */

/* Packed ring event: [63:16] TIMER0 microseconds, [15:0] GPIO0 edge bits. */
static inline uintptr_t accel_ev_pack(uint64_t t_us, uint32_t gpio_bits)
{
    return (uintptr_t)((t_us << 16) | (gpio_bits & 0xFFFFu));
}
static inline uint64_t accel_ev_time_us(uintptr_t ev) { return (uint64_t)ev >> 16; }
static inline uint32_t accel_ev_bits(uintptr_t ev)    { return (uint32_t)(ev & 0xFFFFu); }

typedef struct {
    gpio_drv_t  *gpio;
    mmio_bus_t  *bus;          /* for TIMER0                                 */
    uint32_t     timer_base;
    ring_spsc_t *ring;         /* ISR = producer, firmware loop = consumer   */
    counter_t    runs;         /* counters: written by the ISR only          */
    counter_t    spurious;     /* ran with no edge bit set                   */
    counter_t    drops;        /* ring full: event dropped (the bottom half
                                  re-reads the FIFO level anyway)            */
} accel_isr_ctx_t;

void accel_isr(void *ctx);     /* irq_handler_t */

#endif
