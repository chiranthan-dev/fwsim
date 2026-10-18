/*
 * workload.c - synthetic generators and the CSV trace reader.
 *
 * Arrivals follow a Poisson process (exponential inter-arrival gaps). It is
 * the standard open-system model: hosts issue I/O independently of when the
 * disk finishes, so queues can build up. That is what makes scheduling
 * matter: with an empty queue there is nothing to reorder.
 */
#include "workload.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rng.h"

void workload_config_default(workload_config_t *wc)
{
    memset(wc, 0, sizeof(*wc));
    wc->kind = WL_RANDOM;
    wc->num_requests = 5000;
    wc->rate_iops = 100.0;
    wc->req_sectors = 8;      /* 4 KiB with 512-byte sectors */
    wc->start_frac = 0.0;
    wc->hot_frac = 0.2;
    wc->hot_prob = 0.8;
    wc->write_frac = 0.0;
    wc->seed = 42;
}

static const char *kind_names[] = { "seq", "random", "hotcold", "trace" };

int workload_parse_kind(const char *s, workload_kind_t *out)
{
    for (int i = 0; i < 4; i++) {
        if (strcmp(s, kind_names[i]) == 0) {
            *out = (workload_kind_t)i;
            return 0;
        }
    }
    return -1;
}

const char *workload_kind_name(workload_kind_t k)
{
    return (unsigned)k < 4 ? kind_names[k] : "?";
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

int workload_build(const workload_config_t *wc, uint64_t capacity,
                   io_request_t **out, size_t *n, char *err, size_t errlen)
{
    if (wc->kind == WL_TRACE)
        return workload_load_trace(wc->trace_path, capacity, out, n, err, errlen);

    if (wc->num_requests == 0 || wc->req_sectors == 0 ||
        wc->req_sectors > capacity) {
        set_err(err, errlen, "bad workload size (requests=%zu, sectors=%u)",
                wc->num_requests, wc->req_sectors);
        return -1;
    }
    if (wc->start_frac < 0.0 || wc->start_frac >= 1.0 ||
        wc->hot_frac <= 0.0 || wc->hot_frac >= 1.0 ||
        wc->hot_prob < 0.0 || wc->hot_prob > 1.0) {
        set_err(err, errlen, "workload fractions out of range");
        return -1;
    }

    io_request_t *r = calloc(wc->num_requests, sizeof(*r));
    if (!r) {
        set_err(err, errlen, "out of memory");
        return -1;
    }

    rng_t rng;
    rng_seed(&rng, wc->seed);

    /* Highest legal start so a request never runs off the end. */
    uint64_t span = capacity - wc->req_sectors + 1;
    uint64_t hot_end = (uint64_t)((double)capacity * wc->hot_frac);
    if (hot_end < wc->req_sectors)
        hot_end = wc->req_sectors;
    uint64_t seq_next = (uint64_t)((double)capacity * wc->start_frac);

    double t = 0.0;
    double mean_gap_ms = wc->rate_iops > 0.0 ? 1000.0 / wc->rate_iops : 0.0;

    for (size_t i = 0; i < wc->num_requests; i++) {
        uint64_t lba = 0;
        switch (wc->kind) {
        case WL_SEQ:
            if (seq_next + wc->req_sectors > capacity)
                seq_next = 0;   /* wrap around like a looping scan */
            lba = seq_next;
            seq_next += wc->req_sectors;
            break;
        case WL_RANDOM:
            lba = rng_below(&rng, span);
            break;
        case WL_HOTCOLD:
            /* Hot set is one contiguous band at the start of the disk, like
             * a filesystem's metadata + recently written files. */
            if (rng_unit(&rng) < wc->hot_prob)
                lba = rng_below(&rng, hot_end - wc->req_sectors + 1);
            else if (span > hot_end)
                lba = hot_end + rng_below(&rng, span - hot_end);
            else
                lba = rng_below(&rng, span);   /* tiny disk: no cold band */
            break;
        case WL_TRACE:
            break;   /* handled above */
        }
        r[i].id = i;
        r[i].arrival_ms = t;
        r[i].lba = lba;
        r[i].count = wc->req_sectors;
        r[i].op = rng_unit(&rng) < wc->write_frac ? DISK_OP_WRITE : DISK_OP_READ;
        if (mean_gap_ms > 0.0)
            t += rng_exp(&rng, mean_gap_ms);
    }

    *out = r;
    *n = wc->num_requests;
    return 0;
}

/* ---- trace CSV ---------------------------------------------------------- */

typedef struct {
    io_request_t req;
    size_t       line;   /* original order, makes the sort stable */
} trace_row_t;

static int cmp_row(const void *a, const void *b)
{
    const trace_row_t *x = a, *y = b;
    if (x->req.arrival_ms < y->req.arrival_ms) return -1;
    if (x->req.arrival_ms > y->req.arrival_ms) return 1;
    return (x->line > y->line) - (x->line < y->line);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

/* Split a line on commas into at most `max` fields. Returns field count. */
static int split_csv(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *c = strchr(p, ',');
        if (!c)
            break;
        *c = '\0';
        p = c + 1;
    }
    for (int i = 0; i < n; i++)
        fields[i] = trim(fields[i]);
    return n;
}

int workload_load_trace(const char *path, uint64_t capacity,
                        io_request_t **out, size_t *n, char *err, size_t errlen)
{
    if (!path) {
        set_err(err, errlen, "no trace file given (use --trace FILE)");
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(err, errlen, "cannot open trace '%s': %s", path, strerror(errno));
        return -1;
    }

    trace_row_t *rows = NULL;
    size_t len = 0, cap = 0, lineno = 0;
    char buf[512];
    int rc = -1;
    int seen_header = 0;

    while (fgets(buf, sizeof buf, f)) {
        lineno++;
        if (!strchr(buf, '\n') && !feof(f)) {
            set_err(err, errlen, "%s:%zu: line too long", path, lineno);
            goto done;
        }
        char *line = trim(buf);
        if (*line == '\0' || *line == '#')
            continue;
        /* Allow one header row before the data; after that, a line that
         * does not start with a number is an error, not something to skip. */
        if (!isdigit((unsigned char)*line) && *line != '.') {
            if (len == 0 && !seen_header) {
                seen_header = 1;
                continue;
            }
            set_err(err, errlen, "%s:%zu: expected a number at start of line",
                    path, lineno);
            goto done;
        }

        char *fld[5];
        if (split_csv(line, fld, 5) != 4) {
            set_err(err, errlen, "%s:%zu: expected 4 fields "
                    "(arrival_time_ms,op,lba,length)", path, lineno);
            goto done;
        }

        char *end;
        io_request_t q;
        memset(&q, 0, sizeof q);

        errno = 0;
        q.arrival_ms = strtod(fld[0], &end);
        if (*end || errno || q.arrival_ms < 0.0) {
            set_err(err, errlen, "%s:%zu: bad arrival time '%s'", path, lineno, fld[0]);
            goto done;
        }

        char op = (char)tolower((unsigned char)fld[1][0]);
        if (op == 'r')
            q.op = DISK_OP_READ;
        else if (op == 'w')
            q.op = DISK_OP_WRITE;
        else {
            set_err(err, errlen, "%s:%zu: op must be R or W, got '%s'", path, lineno, fld[1]);
            goto done;
        }

        errno = 0;
        unsigned long long lba = strtoull(fld[2], &end, 10);
        if (*end || errno || fld[2][0] == '-' || fld[2][0] == '\0') {
            set_err(err, errlen, "%s:%zu: bad lba '%s'", path, lineno, fld[2]);
            goto done;
        }
        errno = 0;
        unsigned long cnt = strtoul(fld[3], &end, 10);
        if (*end || errno || cnt == 0 || cnt > UINT32_MAX || fld[3][0] == '-') {
            set_err(err, errlen, "%s:%zu: bad length '%s'", path, lineno, fld[3]);
            goto done;
        }
        if (lba >= capacity || cnt > capacity - lba) {
            set_err(err, errlen, "%s:%zu: lba %llu + %lu beyond capacity %llu",
                    path, lineno, lba, cnt, (unsigned long long)capacity);
            goto done;
        }
        q.lba = lba;
        q.count = (uint32_t)cnt;

        if (len == cap) {
            size_t nc = cap ? cap * 2 : 1024;
            trace_row_t *p = realloc(rows, nc * sizeof(*p));
            if (!p) {
                set_err(err, errlen, "out of memory");
                goto done;
            }
            rows = p;
            cap = nc;
        }
        rows[len].req = q;
        rows[len].line = lineno;
        len++;
    }
    if (ferror(f)) {
        set_err(err, errlen, "%s: read error", path);
        goto done;
    }
    if (len == 0) {
        set_err(err, errlen, "%s: no requests found", path);
        goto done;
    }

    /* Traces are usually time-ordered already, but merged traces may not
     * be. Sort by arrival, keeping file order for ties. */
    qsort(rows, len, sizeof(*rows), cmp_row);

    io_request_t *r = malloc(len * sizeof(*r));
    if (!r) {
        set_err(err, errlen, "out of memory");
        goto done;
    }
    for (size_t i = 0; i < len; i++) {
        r[i] = rows[i].req;
        r[i].id = i;
    }
    *out = r;
    *n = len;
    rc = 0;

done:
    free(rows);
    fclose(f);
    return rc;
}

int workload_write_trace(const char *path, const io_request_t *reqs, size_t n)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "arrival_time_ms,op,lba,length\n");
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%.6f,%c,%llu,%u\n", reqs[i].arrival_ms,
                reqs[i].op == DISK_OP_WRITE ? 'W' : 'R',
                (unsigned long long)reqs[i].lba, reqs[i].count);
    return fclose(f) == 0 ? 0 : -1;
}
