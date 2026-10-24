/*
 * mmio.c - address decoding for the simulated peripheral bus. See mmio.h.
 */
#include "hal/mmio.h"

#include <string.h>

int mmio_bus_init(mmio_bus_t *bus)
{
    memset(bus, 0, sizeof(*bus));
    return pthread_mutex_init(&bus->lock, NULL) == 0 ? 0 : -1;
}

void mmio_bus_destroy(mmio_bus_t *bus)
{
    pthread_mutex_destroy(&bus->lock);
}

int mmio_map(mmio_bus_t *bus, uint32_t base, uint32_t size, mmio_read_fn rd,
             mmio_write_fn wr, void *dev, const char *name)
{
    if (bus->nregions == MMIO_MAX_REGIONS || size == 0 || (base & 3u) ||
        (uint64_t)base + size > 0x100000000ull)
        return -1;
    for (unsigned i = 0; i < bus->nregions; i++) {
        const mmio_region_t *r = &bus->region[i];
        if (base < r->base + r->size && r->base < base + size)
            return -1;                       /* overlap */
    }
    bus->region[bus->nregions++] = (mmio_region_t){ base, size, rd, wr, dev, name };
    return 0;
}

/* Linear search: a handful of regions, and the decode is not what we are
 * measuring. Hardware does this with comparators in parallel. */
static const mmio_region_t *decode(const mmio_bus_t *bus, uint32_t addr)
{
    if (addr & 3u)
        return NULL;
    for (unsigned i = 0; i < bus->nregions; i++) {
        const mmio_region_t *r = &bus->region[i];
        if (addr >= r->base && addr - r->base < r->size)
            return r;
    }
    return NULL;
}

uint32_t mmio_read32(mmio_bus_t *bus, uint32_t addr)
{
    pthread_mutex_lock(&bus->lock);
    bus->accesses++;
    const mmio_region_t *r = decode(bus, addr);
    uint32_t v = MMIO_BUS_ERROR;
    if (r && r->read)
        v = r->read(r->dev, addr - r->base);
    else
        bus->bad_accesses++;
    pthread_mutex_unlock(&bus->lock);
    return v;
}

void mmio_write32(mmio_bus_t *bus, uint32_t addr, uint32_t val)
{
    pthread_mutex_lock(&bus->lock);
    bus->accesses++;
    const mmio_region_t *r = decode(bus, addr);
    if (r && r->write)
        r->write(r->dev, addr - r->base, val);
    else
        bus->bad_accesses++;
    pthread_mutex_unlock(&bus->lock);
}

void mmio_locked(mmio_bus_t *bus, void (*fn)(void *arg), void *arg)
{
    pthread_mutex_lock(&bus->lock);
    fn(arg);
    pthread_mutex_unlock(&bus->lock);
}

uint64_t mmio_bad_accesses(mmio_bus_t *bus)
{
    pthread_mutex_lock(&bus->lock);
    uint64_t n = bus->bad_accesses;
    pthread_mutex_unlock(&bus->lock);
    return n;
}
