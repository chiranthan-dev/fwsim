/*
 * mmio.h - memory-mapped register access for the simulated peripherals.
 *
 * Drivers never touch a device model's state directly. They call
 *
 *     uint32_t v = mmio_read32(bus, SPI0_BASE + SPI_STATUS);
 *     mmio_write32(bus, SPI0_BASE + SPI_TXDATA, byte);
 *
 * and the bus dispatches the access, by address range, to the callback of
 * whichever device model is mapped there - the way an AHB/APB interconnect
 * decodes an address and routes it to one peripheral.
 *
 * WHY a dispatch function instead of `*(volatile uint32_t *)addr`:
 *   - The model SEES every access. Reading a FIFO data register pops the
 *     FIFO; writing 1 to a W1C status bit clears it; reading a
 *     read-to-clear register clears it. Those side effects are the whole
 *     point of a register map, and a plain memory array cannot have them.
 *   - Accesses to unmapped addresses are caught (counted, and read as
 *     0xFFFFFFFF like a bus error on many SoCs) instead of scribbling on
 *     host memory.
 *   - In the optional threaded-interrupt mode (irq.h) an ISR and the
 *     firmware loop touch the same device from two threads; the bus lock
 *     serialises them exactly as a single-ported bus would.
 *
 * On real silicon the whole file collapses to one macro, and the drivers
 * do not change:
 *
 *     #define mmio_read32(bus, a)     (*(volatile uint32_t *)(uintptr_t)(a))
 *     #define mmio_write32(bus, a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (v))
 *
 * (`volatile` is right THERE - it forces exactly one bus access per read or
 * write, in program order, which is what a device register needs - even
 * though it is the wrong tool for inter-thread synchronisation; see
 * ring_spsc.h.)
 *
 * Addresses are 32-bit and accesses are 32-bit aligned, like most
 * microcontroller peripheral buses.
 */
#ifndef FWSIM_HAL_MMIO_H
#define FWSIM_HAL_MMIO_H

#include <pthread.h>
#include <stdint.h>

#define MMIO_MAX_REGIONS 8
#define MMIO_BUS_ERROR   0xFFFFFFFFu   /* value read from an unmapped address */

/* A device model's register handlers. `off` is relative to the region base. */
typedef uint32_t (*mmio_read_fn)(void *dev, uint32_t off);
typedef void (*mmio_write_fn)(void *dev, uint32_t off, uint32_t val);

typedef struct {
    uint32_t      base, size;
    mmio_read_fn  read;
    mmio_write_fn write;
    void         *dev;
    const char   *name;
} mmio_region_t;

typedef struct {
    mmio_region_t   region[MMIO_MAX_REGIONS];
    unsigned        nregions;
    pthread_mutex_t lock;         /* one access at a time, see above        */
    uint64_t        bad_accesses; /* unmapped or misaligned (under lock)    */
    uint64_t        accesses;     /* total, for the cost report             */
} mmio_bus_t;

int  mmio_bus_init(mmio_bus_t *bus);
void mmio_bus_destroy(mmio_bus_t *bus);

/* Map a device. Regions must not overlap. 0 or -1. Call before any access. */
int mmio_map(mmio_bus_t *bus, uint32_t base, uint32_t size, mmio_read_fn rd,
             mmio_write_fn wr, void *dev, const char *name);

uint32_t mmio_read32(mmio_bus_t *bus, uint32_t addr);
void     mmio_write32(mmio_bus_t *bus, uint32_t addr, uint32_t val);

/* Run fn(arg) while holding the bus lock. Device models use this for work
 * that is not a register access but must not interleave with one - e.g.
 * the accelerometer producing a sample into its FIFO. */
void mmio_locked(mmio_bus_t *bus, void (*fn)(void *arg), void *arg);

uint64_t mmio_bad_accesses(mmio_bus_t *bus);

#endif
