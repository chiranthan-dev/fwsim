/*
 * accel_model.h - device model of a single-axis SPI accelerometer used as
 * the drive's shock / vibration sensor.
 *
 * ORIGINAL part and register map ("SIMACC-1"), invented for this project;
 * it resembles the general shape of real MEMS parts (WHO_AM_I, ODR,
 * threshold interrupt, FIFO with watermark) but copies none of them.
 *
 * Measurement: 1 LSB = 1 mg, signed 16-bit (+-32 g). One axis: the
 * in-plane acceleration that rotates the actuator about its pivot, which
 * is the component that pushes the head off-track. (Real drives often use
 * two linear sensors and take their difference to get rotation.)
 *
 * The analog world -> digital sample path, per output sample:
 *   1. average ACCEL_OVERSAMPLE evenly spaced readings of the physical
 *      signal over the sample period (a crude decimation / anti-alias
 *      filter, like a sigma-delta MEMS front end),
 *   2. add white sensor noise (Gaussian, noise_mg RMS, seeded RNG),
 *   3. round and saturate to int16.
 *
 * ---- SPI protocol ----------------------------------------------------------
 *   CS low, then a command byte:  [7] R/W (1 = read)  [6] MB (auto-
 *   increment the address after each data byte)  [5:0] register address,
 *   then data bytes. The command byte clocks out 0x00. Reads of FIFO_DATA
 *   never auto-increment: a burst read at FIFO_DATA streams the FIFO,
 *   low byte then high byte of each sample, popping a sample after its
 *   high byte.
 *
 * ---- registers (8-bit) -----------------------------------------------------
 *   0x00 WHO_AM_I   RO  0xA7
 *   0x01 REV_ID     RO  0x01
 *   0x10 CTRL1      RW  [0] PWR (sampling on)  [3:1] ODR code
 *                       (0 250 Hz, 1 500, 2 1000, 3 2000, 4 4000)
 *                       [7] SOFT_RESET (self-clearing)
 *   0x11 CTRL2      RW  [0] FIFO_EN  [1] FIFO_FLUSH (self-clearing)
 *   0x12 THS_L      RW  shock threshold, mg, low byte   (|sample| >= THS)
 *   0x13 THS_H      RW  shock threshold, high byte
 *   0x14 THS_DUR    RW  consecutive samples at/over THS before SHOCK
 *                       (0 behaves as 1)
 *   0x15 FIFO_WTM   RW  watermark, samples (0 = no watermark)
 *   0x16 FIFO_LEVEL RO  samples in the FIFO (0..128)
 *   0x17 FIFO_FLAGS RO  [0] WTM  [1] OVR (sticky)  [2] EMPTY
 *   0x18 INT_CFG    RW  [0] WTM_EN  [1] SHOCK_EN  [2] OVR_EN
 *                       [6] LATCH (SHOCK held until INT_SRC is read)
 *                       [7] POL (1 = INT pin active low)
 *   0x19 INT_SRC    RO  [0] WTM  [1] SHOCK  [2] OVR; reading clears the
 *                       latched SHOCK and OVR bits (WTM follows the level)
 *   0x1A OUT_L      RO  latest sample, low byte
 *   0x1B OUT_H      RO  latest sample, high byte
 *   0x1C..0x1F SAMPLE_CNT  RO  32-bit count of samples produced (LE)
 *   0x20 FIFO_DATA  RO  FIFO read port (see protocol)
 *
 * FIFO: 128 samples, "stream" mode - when full, the oldest sample is
 * discarded and OVR is set, so the FIFO always holds the newest data.
 *
 * INT pin: asserted while (INT_SRC & enabled sources) != 0, inverted if
 * POL. The pin is a wire to a GPIO input (see board.c); the model calls
 * pin_cb on every level change.
 */
#ifndef FWSIM_PERIPH_ACCEL_MODEL_H
#define FWSIM_PERIPH_ACCEL_MODEL_H

#include <stdint.h>

#include "periph/spi_ctrl.h"

#define ACCEL_WHO_AM_I    0x00u
#define ACCEL_REV_ID      0x01u
#define ACCEL_CTRL1       0x10u
#define ACCEL_CTRL2       0x11u
#define ACCEL_THS_L       0x12u
#define ACCEL_THS_H       0x13u
#define ACCEL_THS_DUR     0x14u
#define ACCEL_FIFO_WTM    0x15u
#define ACCEL_FIFO_LEVEL  0x16u
#define ACCEL_FIFO_FLAGS  0x17u
#define ACCEL_INT_CFG     0x18u
#define ACCEL_INT_SRC     0x19u
#define ACCEL_OUT_L       0x1Au
#define ACCEL_OUT_H       0x1Bu
#define ACCEL_SAMPLE_CNT0 0x1Cu
#define ACCEL_FIFO_DATA   0x20u

#define ACCEL_ID_VALUE    0xA7u
#define ACCEL_REV_VALUE   0x01u

#define ACCEL_CTRL1_PWR        (1u << 0)
#define ACCEL_CTRL1_ODR_SHIFT  1
#define ACCEL_CTRL1_ODR_MASK   (7u << 1)
#define ACCEL_CTRL1_SOFT_RESET (1u << 7)
#define ACCEL_CTRL2_FIFO_EN    (1u << 0)
#define ACCEL_CTRL2_FIFO_FLUSH (1u << 1)
#define ACCEL_FLAG_WTM         (1u << 0)
#define ACCEL_FLAG_OVR         (1u << 1)
#define ACCEL_FLAG_EMPTY       (1u << 2)
#define ACCEL_INT_WTM          (1u << 0)
#define ACCEL_INT_SHOCK        (1u << 1)
#define ACCEL_INT_OVR          (1u << 2)
#define ACCEL_INT_LATCH        (1u << 6)
#define ACCEL_INT_POL_LOW      (1u << 7)

#define ACCEL_ODR_1000HZ  2u
#define ACCEL_FIFO_DEPTH  128u
#define ACCEL_OVERSAMPLE  4u

/* The physical acceleration (mg) at simulated time t_us. */
typedef double (*accel_source_fn)(void *ctx, uint64_t t_us);
/* The INT pin changed level. */
typedef void (*accel_pin_fn)(void *ctx, int level);

typedef struct {
    uint8_t  reg[0x40];             /* plain storage for RW registers     */
    int16_t  fifo[ACCEL_FIFO_DEPTH];
    unsigned fifo_head, fifo_n;
    int      fifo_hi_phase;         /* next FIFO_DATA read = high byte    */
    int      ovr;                   /* sticky overflow flag               */
    int      shock_latched;
    unsigned over_run;              /* consecutive samples at/over THS    */
    int16_t  out;                   /* latest sample                      */
    uint32_t sample_cnt;
    int      pin;                   /* current INT pin level              */

    /* SPI frame state */
    int      in_frame, have_cmd, is_read, auto_inc;
    uint8_t  addr;

    /* analog front end */
    accel_source_fn src;
    void    *src_ctx;
    double   noise_mg;
    uint64_t rng;
    accel_pin_fn pin_cb;
    void    *pin_ctx;

    uint64_t fifo_overruns;         /* samples discarded (stream mode)    */
} accel_model_t;

void accel_model_init(accel_model_t *a, accel_source_fn src, void *src_ctx,
                      double noise_mg, uint64_t seed, accel_pin_fn pin_cb, void *pin_ctx);

/* SPI slave interface for spi_ctrl. */
spi_slave_t accel_model_spi(accel_model_t *a);

/* Output data rate configured in CTRL1 (Hz), and whether PWR is on. */
uint32_t accel_model_odr_hz(const accel_model_t *a);
int      accel_model_powered(const accel_model_t *a);

/* Produce one output sample for the period ending at t_us (the board
 * calls this every 1/ODR). Updates FIFO, SHOCK/WTM/OVR and the INT pin. */
void accel_model_sample(accel_model_t *a, uint64_t t_us);

/* The digitisation step alone (oversample-average + noise + int16), shared
 * with the dataset generator so training data takes the same path. */
int16_t accel_model_digitize(accel_source_fn src, void *src_ctx, uint64_t t_us,
                             uint32_t odr_hz, double noise_mg, uint64_t *rng);

#endif
