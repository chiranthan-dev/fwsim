/*
 * media.c - disk-sim timing + backing file + per-sector CRC-32 + faults.
 */
#include "media.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clock.h"
#include "crc32.h"

#define RECBUF_SECTORS 256u

/* ---- small helpers ------------------------------------------------------ */

static int powered(const media_t *m)
{
    return atomic_load_explicit(m->power_on, memory_order_acquire) != 0;
}

/* pread/pwrite may transfer less than asked (signals, huge requests); a
 * real driver loops, so do we. */
static int pread_full(int fd, void *buf, size_t len, off_t off)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t r = pread(fd, p, len, off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        if (r == 0) {              /* past EOF of a sparse file: zeros */
            memset(p, 0, len);
            return 0;
        }
        p += r;
        len -= (size_t)r;
        off += r;
    }
    return 0;
}

static int pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t r = pwrite(fd, p, len, off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        len -= (size_t)r;
        off += r;
    }
    return 0;
}

static off_t rec_offset(const media_t *m, uint64_t lba)
{
    return (off_t)(lba * m->rec_size);
}

static uint32_t sector_crc(const uint8_t *data, uint32_t size, uint64_t lba)
{
    uint8_t le[8];
    for (int i = 0; i < 8; i++)
        le[i] = (uint8_t)(lba >> (8 * i));
    uint32_t c = crc32_update(CRC32_INIT, data, size);
    return crc32_final(crc32_update(c, le, sizeof(le)));
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

/* Simulated "now" for disk-sim. The platter keeps spinning while the drive
 * is idle, so when media time is played back in real time the simulated
 * clock follows the wall clock; it never runs backwards. */
static double sim_now(media_t *m)
{
    if (m->time_scale > 0.0) {
        double wall_ms = (double)(now_ns() - m->t0_ns) / 1e6 / m->time_scale;
        if (wall_ms > m->sim_ms)
            m->sim_ms = wall_ms;
    }
    return m->sim_ms;
}

static uint64_t sim_ms_to_wall_ns(const media_t *m, double ms)
{
    return (uint64_t)(ms * 1e6 * m->time_scale);
}

static void account(media_t *m, double ms)
{
    m->sim_ms += ms;
    ctr_add(&m->ctr->media_busy_us, (uint64_t)(ms * 1000.0 + 0.5));
}

/* ---- lifecycle ----------------------------------------------------------- */

int media_open(media_t *m, const fw_config_t *cfg, const atomic_int *power_on,
               fw_counters_t *ctr)
{
    memset(m, 0, sizeof(*m));
    m->fd = -1;

    int derr = 0;
    m->disk = disk_open(&cfg->disk, &derr);
    if (!m->disk)
        return -EINVAL;

    disk_info_t info;
    disk_get_info(m->disk, &info);
    m->capacity = info.capacity_lbas;
    if (cfg->capacity_lbas && cfg->capacity_lbas < m->capacity)
        m->capacity = cfg->capacity_lbas;   /* like ATA SET MAX ADDRESS */
    m->sector_size = info.sector_size;
    m->rec_size = (size_t)info.sector_size + 4;
    m->rev_ms = info.rev_ms;
    m->time_scale = cfg->time_scale;
    m->t0_ns = now_ns();
    m->power_on = power_on;
    m->ctr = ctr;
    m->read_retries = cfg->read_retries;
    m->flip_prob = cfg->fault_read_flip_prob;
    m->rng = cfg->seed ^ 0x6D656469615F726Eull;
    atomic_init(&m->inject_rng, cfg->seed ^ 0x696E6A6563740000ull);

    m->recbuf_sectors = RECBUF_SECTORS;
    m->recbuf = malloc(m->recbuf_sectors * m->rec_size);
    if (!m->recbuf)
        goto fail;

    int flags = O_RDWR | O_CREAT | (cfg->media_create ? O_TRUNC : 0);
    m->fd = open(cfg->media_path, flags, 0644);
    if (m->fd < 0)
        goto fail;
    /* Set the full size up front. On Linux filesystems this creates a
     * sparse file: blocks are only allocated when written, so a 100 GB
     * simulated drive costs only what the workload touches. */
    struct stat st;
    off_t want = rec_offset(m, m->capacity);
    if (fstat(m->fd, &st) != 0 || (st.st_size < want && ftruncate(m->fd, want) != 0))
        goto fail;
    return 0;

fail:
    media_close(m);
    return -EIO;
}

void media_close(media_t *m)
{
    if (m->fd >= 0)
        close(m->fd);
    m->fd = -1;
    disk_close(m->disk);
    m->disk = NULL;
    free(m->recbuf);
    m->recbuf = NULL;
    free(m->pending);
    m->pending = NULL;
}

/* ---- pending (unrecoverable) sectors --------------------------------------
 * A sector that failed every read retry still holds the host's data, as
 * far as the host knows - we must not silently move it. Real drives put it
 * on a "pending" list and reallocate it to a spare the next time the host
 * WRITES it (the host is supplying fresh data, so nothing is lost). */

static void pending_add(media_t *m, uint64_t lba)
{
    for (size_t i = 0; i < m->npending; i++)
        if (m->pending[i] == lba)
            return;
    if (m->npending == m->pending_cap) {
        size_t nc = m->pending_cap ? m->pending_cap * 2 : 16;
        uint64_t *np = realloc(m->pending, nc * sizeof(*np));
        if (!np)
            return;   /* best effort: worst case we just don't reallocate */
        m->pending = np;
        m->pending_cap = nc;
    }
    m->pending[m->npending++] = lba;
}

static void reallocate_pending(media_t *m, uint64_t lba, uint32_t n)
{
    for (size_t i = 0; i < m->npending;) {
        uint64_t p = m->pending[i];
        if (p >= lba && p - lba < n) {
            /* disk-sim moves the LBA to the spare area: from now on its
             * access time includes the trip to the spare cylinders. */
            if (disk_add_grown_defect(m->disk, p) == DISK_OK)
                ctr_add(&m->ctr->reallocations, 1);
            m->pending[i] = m->pending[--m->npending];
        } else {
            i++;
        }
    }
}

/* ---- I/O ----------------------------------------------------------------- */

/*
 * Run the mechanics for one access: ask disk-sim how long it takes, then
 * play that time back (scaled). Returns 1 if it completed, 0 if power was
 * cut part-way; *elapsed_ms tells how far it got.
 */
static int mechanics(media_t *m, uint64_t lba, uint32_t n, disk_op_t op,
                     disk_timing_t *t, double *elapsed_ms)
{
    disk_request_t req = { .lba = lba, .count = n, .op = op };
    if (disk_service_request(m->disk, &req, sim_now(m), t) != DISK_OK) {
        memset(t, 0, sizeof(*t));
        *elapsed_ms = 0.0;
        return -1;
    }
    *elapsed_ms = t->total_ms;
    if (m->time_scale <= 0.0) {
        account(m, t->total_ms);
        return 1;
    }
    uint64_t start = now_ns();
    uint64_t deadline = start + sim_ms_to_wall_ns(m, t->total_ms);
    uint64_t end = delay_until(deadline, m->power_on);
    if (end < deadline) {
        *elapsed_ms = (double)(end - start) / 1e6 / m->time_scale;
        account(m, *elapsed_ms);
        return 0;
    }
    account(m, t->total_ms);
    return 1;
}

/* Wait one more revolution (a read retry waits for the sector to come
 * round again). Returns 0 if power was cut. */
static int one_revolution(media_t *m)
{
    account(m, m->rev_ms);
    if (m->time_scale <= 0.0)
        return powered(m);
    uint64_t deadline = now_ns() + sim_ms_to_wall_ns(m, m->rev_ms);
    return delay_until(deadline, m->power_on) >= deadline;
}

/* Check one record. 1 = good (data copied), 0 = CRC mismatch. */
static int verify_record(media_t *m, const uint8_t *rec, uint64_t lba, uint8_t *out)
{
    if (all_zero(rec, m->rec_size)) {
        memset(out, 0, m->sector_size);       /* never written */
        return 1;
    }
    if (sector_crc(rec, m->sector_size, lba) != get_le32(rec + m->sector_size))
        return 0;
    memcpy(out, rec, m->sector_size);
    return 1;
}

/* Model read-channel noise: occasionally the bits we *read* are wrong
 * even though the bits on the platter are fine. A retry re-reads and will
 * usually succeed - this is why drives retry before giving up. */
static void maybe_transient_flip(media_t *m, uint8_t *rec)
{
    if (m->flip_prob > 0.0 && rng_unit64(&m->rng) < m->flip_prob) {
        uint64_t bit = rng_below64(&m->rng, (uint64_t)m->rec_size * 8);
        rec[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        ctr_add(&m->ctr->transient_flips, 1);
    }
}

fw_status_t media_read(media_t *m, uint64_t lba, uint32_t n, void *buf,
                       uint64_t *bad_lba)
{
    if (!powered(m))
        return FW_ERR_ABORTED;

    disk_timing_t t;
    double elapsed;
    int mr = mechanics(m, lba, n, DISK_OP_READ, &t, &elapsed);
    if (mr < 0)
        return FW_ERR_HARDWARE;
    if (mr == 0)
        return FW_ERR_ABORTED;
    ctr_add(&m->ctr->media_reads, 1);
    ctr_add(&m->ctr->media_sectors_read, n);

    uint8_t *out = buf;
    for (uint32_t done = 0; done < n;) {
        uint32_t chunk = n - done;
        if (chunk > m->recbuf_sectors)
            chunk = (uint32_t)m->recbuf_sectors;
        if (pread_full(m->fd, m->recbuf, chunk * m->rec_size, rec_offset(m, lba + done)) != 0)
            return FW_ERR_HARDWARE;

        for (uint32_t i = 0; i < chunk; i++) {
            uint64_t s = lba + done + i;
            uint8_t *rec = m->recbuf + (size_t)i * m->rec_size;
            uint8_t *dst = out + (size_t)(done + i) * m->sector_size;

            maybe_transient_flip(m, rec);
            if (verify_record(m, rec, s, dst))
                continue;

            /* CRC mismatch: retry by re-reading on later revolutions. */
            ctr_add(&m->ctr->crc_errors, 1);
            int ok = 0;
            for (uint32_t r = 0; r < m->read_retries && !ok; r++) {
                ctr_add(&m->ctr->crc_retries, 1);
                if (!one_revolution(m))
                    return FW_ERR_ABORTED;
                if (pread_full(m->fd, rec, m->rec_size, rec_offset(m, s)) != 0)
                    return FW_ERR_HARDWARE;
                maybe_transient_flip(m, rec);
                ok = verify_record(m, rec, s, dst);
            }
            if (ok) {
                ctr_add(&m->ctr->crc_recovered, 1);
                continue;
            }
            /* Never return data we know is wrong: report the LBA instead. */
            ctr_add(&m->ctr->crc_unrecovered, 1);
            pending_add(m, s);
            *bad_lba = s;
            return FW_ERR_MEDIUM;
        }
        done += chunk;
    }
    return FW_OK;
}

static int write_records(media_t *m, uint64_t lba, uint32_t n, const uint8_t *src)
{
    for (uint32_t done = 0; done < n;) {
        uint32_t chunk = n - done;
        if (chunk > m->recbuf_sectors)
            chunk = (uint32_t)m->recbuf_sectors;
        for (uint32_t i = 0; i < chunk; i++) {
            uint8_t *rec = m->recbuf + (size_t)i * m->rec_size;
            const uint8_t *d = src + (size_t)(done + i) * m->sector_size;
            memcpy(rec, d, m->sector_size);
            put_le32(rec + m->sector_size, sector_crc(d, m->sector_size, lba + done + i));
        }
        if (pwrite_full(m->fd, m->recbuf, chunk * m->rec_size, rec_offset(m, lba + done)) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

fw_status_t media_write(media_t *m, uint64_t lba, uint32_t n, const void *buf)
{
    if (!powered(m))
        return FW_ERR_ABORTED;

    reallocate_pending(m, lba, n);

    disk_timing_t t;
    double elapsed;
    int mr = mechanics(m, lba, n, DISK_OP_WRITE, &t, &elapsed);
    if (mr < 0)
        return FW_ERR_HARDWARE;

    uint32_t todo = n;
    if (mr == 0) {
        /*
         * Power died during this access. Sectors are written as they pass
         * under the head, so the ones that already passed are on the
         * platter and the rest are not: a torn write. (We treat the
         * request as positioning followed by one continuous transfer; a
         * request spanning several tracks is approximated.)
         */
        double positioning = t.seek_ms + t.rotate_ms;
        double per_sector = t.transfer_ms / n;
        todo = 0;
        if (elapsed > positioning && per_sector > 0.0) {
            double k = (elapsed - positioning) / per_sector;
            todo = k >= n ? n - 1 : (uint32_t)k;
        }
        ctr_add(&m->ctr->torn_writes, 1);
    }

    if (todo && write_records(m, lba, todo, buf) != 0)
        return FW_ERR_WRITE_FAULT;
    if (mr == 0)
        return FW_ERR_ABORTED;

    ctr_add(&m->ctr->media_writes, 1);
    ctr_add(&m->ctr->media_sectors_written, n);
    return FW_OK;
}

int media_inject_bitflip(media_t *m, uint64_t lba, int bit)
{
    if (lba >= m->capacity)
        return -1;
    uint8_t *rec = malloc(m->rec_size);
    if (!rec)
        return -1;
    int rc = -1;
    if (pread_full(m->fd, rec, m->rec_size, rec_offset(m, lba)) == 0) {
        uint64_t b;
        if (bit < 0) {
            /* Host-thread RNG: atomic so concurrent injectors stay race-free. */
            uint64_t s = atomic_fetch_add_explicit(&m->inject_rng, 0x9E3779B97F4A7C15ull,
                                                   memory_order_relaxed);
            b = rng_next64(&s) % ((uint64_t)m->rec_size * 8);
        } else {
            b = (uint64_t)bit % ((uint64_t)m->rec_size * 8);
        }
        rec[b / 8] ^= (uint8_t)(1u << (b % 8));
        rc = pwrite_full(m->fd, rec, m->rec_size, rec_offset(m, lba));
    }
    free(rec);
    return rc;
}
