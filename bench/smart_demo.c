/*
 * smart_demo.c - `fwsim-smart-demo`: the SMART model running inside the
 * simulated drive while the drive's surface degrades.
 *
 *   fwsim-smart-demo [--days N] [--onset DAY] [--csv FILE] [--seed N] [--quiet]
 *
 * DEPLOYMENT DEMO ON SYNTHETIC COUNTERS. It shows the plumbing - counters ->
 * SMART attributes -> 7/30-day history -> integer model -> SMART_READ ->
 * flag - working end to end. It says NOTHING about how accurate the model
 * is: the defects below are scripted, not real. The accuracy numbers come
 * only from the Backblaze test set (results/smart/).
 *
 * Script: a ~20,000-hour-old drive serves a light random workload every
 * simulated day. From day `onset` on, a growing number of new grown
 * defects appears each day (a spreading scratch: adjacent LBAs). The host
 * reads each one and gets MEDIUM ERROR (SMART 187 +1, LBA pending: 197
 * +1); it rewrites 3 of every 4 (the drive reallocates them: 5 +1, 197
 * -1) and leaves the rest pending. Each day ends with a SMART_READ.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fwsim.h"
#include "util.h"

static fw_status_t exec(fw_t *fw, uint8_t op, uint64_t lba, uint32_t n, void *buf, uint8_t flags)
{
    fw_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = op;
    c.lba = lba;
    c.count = n;
    c.buf = buf;
    c.flags = flags;
    return fw_exec(fw, &c);
}

static const fw_smart_attr_t *attr(const fw_smart_data_t *d, uint8_t id)
{
    for (int i = 0; i < d->num_attrs && i < FW_SMART_NATTR; i++)
        if (d->attr[i].id == id)
            return &d->attr[i];
    return NULL;
}

int main(int argc, char **argv)
{
    int days = 56, onset = 14, quiet = 0;
    uint64_t seed = 7;
    const char *csv_path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--days") && v) {
            days = atoi(v); i++;
        } else if (!strcmp(argv[i], "--onset") && v) {
            onset = atoi(v); i++;
        } else if (!strcmp(argv[i], "--csv") && v) {
            csv_path = v; i++;
        } else if (!strcmp(argv[i], "--seed") && v) {
            seed = strtoull(v, NULL, 0); i++;
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else {
            fprintf(stderr, "usage: fwsim-smart-demo [--days N] [--onset DAY] [--csv FILE] "
                            "[--seed N] [--quiet]\n");
            return 2;
        }
    }

    char media[128];
    snprintf(media, sizeof(media), "/tmp/fwsim-smart-demo-%d.media", (int)getpid());
    fw_config_t cfg;
    fw_config_default(&cfg);
    cfg.media_path = media;
    cfg.time_scale = 0.0;
    cfg.capacity_lbas = 1u << 20;
    cfg.cache_entries = 1024;
    cfg.idle_destage_ms = 0.0;
    cfg.seed = seed;
    cfg.smart_enable = 1;
    fw_t *fw = fw_create(&cfg, NULL);
    if (!fw) {
        fprintf(stderr, "fwsim-smart-demo: cannot create drive\n");
        return 1;
    }
    FILE *csv = csv_path ? fopen(csv_path, "w") : NULL;
    if (csv)
        fprintf(csv, "day,power_on_hours,smart_5,smart_187,smart_197,new_defects,"
                     "logit_q8,risk_bp,threshold_q8,predicted_failure,status\n");
    if (!quiet)
        printf("SMART deployment demo (synthetic counters; accuracy claims come from the "
               "Backblaze test set only)\n"
               "day   POH    5   187  197  new  risk%%   flag\n");

    fw_sim_advance_hours(fw, 20000);           /* a 2.3-year-old drive */
    uint8_t *buf = calloc(8, 512);
    uint64_t rng = seed;
    uint64_t scratch = 700000, next_defect = scratch;
    int first_flag = -1, rc = 0;
    for (int day = 0; day < days; day++) {
        if (day > 0)
            fw_sim_advance_hours(fw, 24);
        fw_sim_set_temperature(fw, 36 + (uint32_t)(day % 4));

        /* the day's normal work: random 4K reads and writes */
        for (int k = 0; k < 64; k++) {
            uint64_t lba = rng_below64(&rng, 500000) & ~UINT64_C(7);
            uint8_t op = (k & 1) ? FW_OP_WRITE : FW_OP_READ;
            if (exec(fw, op, lba, 8, buf, 0) != FW_OK)
                rc = 1;
        }

        int n_new = 0;
        if (day >= onset) {
            int t = day - onset;
            n_new = 1 + t * t / 16;            /* the scratch spreads */
            for (int k = 0; k < n_new; k++) {
                uint64_t lba = next_defect++;
                fw_inject_media_bitflip(fw, lba, 7);
                if (exec(fw, FW_OP_READ, lba, 1, buf, 0) != FW_ERR_MEDIUM)
                    rc = 1;                    /* the defect must be seen */
                if (k % 4 != 3) {
                    memset(buf, 0xA5, 512);
                    if (exec(fw, FW_OP_WRITE, lba, 1, buf, FW_CMD_FUA) != FW_OK)
                        rc = 1;
                }
            }
        }

        uint8_t sec[512];
        fw_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = FW_OP_SMART_READ;
        c.count = 1;
        c.buf = sec;
        c.feature = FW_SMART_PAGE_DATA;
        fw_status_t st = fw_exec(fw, &c);
        if (st != FW_OK && st != FW_INFO_FAILURE_PREDICTED) {
            fprintf(stderr, "fwsim-smart-demo: SMART_READ failed: %s\n", fw_status_str(st));
            rc = 1;
            break;
        }
        fw_smart_data_t d;
        memcpy(&d, sec, sizeof(d));
        if (d.predicted_failure && first_flag < 0)
            first_flag = day;
        uint64_t a5 = attr(&d, 5)->raw, a187 = attr(&d, 187)->raw, a197 = attr(&d, 197)->raw;
        if (csv)
            fprintf(csv, "%d,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%d,%u,%d,%u,%s\n",
                    day, d.power_on_hours, a5, a187, a197, n_new, d.logit_q8, d.risk_bp,
                    d.threshold_q8, d.predicted_failure, fw_status_str(st));
        if (!quiet)
            printf("%3d %6u %4" PRIu64 " %5" PRIu64 " %4" PRIu64 " %4d %6.2f   %s\n",
                   day, d.power_on_hours, a5, a187, a197, n_new, d.risk_bp / 100.0,
                   d.predicted_failure ? "PREDICTED FAILURE" : "-");
    }
    if (!quiet) {
        if (first_flag >= 0)
            printf("flag tripped on day %d (%d days after the first defect); SMART_READ "
                   "then completes with sense 01/5D/00\n", first_flag, first_flag - onset);
        else
            printf("flag never tripped in %d days\n", days);
    }
    if (csv)
        fclose(csv);
    free(buf);
    fw_destroy(fw);
    unlink(media);
    return rc;
}
