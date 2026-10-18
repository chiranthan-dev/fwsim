/*
 * main.c - `fwsim` command-line demo: power on a drive, IDENTIFY it, run
 * one workload, print latency / cache / media / integrity statistics.
 *
 *   fwsim [--workload randwrite|randread|seqread|seqwrite|mixed]
 *         [--ops N] [--qd N] [--xfer SECTORS] [--region LBAS]
 *         [--write-through] [--fua] [--no-readahead] [--unsorted-destage]
 *         [--cache ENTRIES] [--time-scale X] [--flip-prob P]
 *         [--media PATH] [--seed N] [--quiet]
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fwsim.h"
#include "workload.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: fwsim [--workload randwrite|randread|seqread|seqwrite|mixed]\n"
            "             [--ops N] [--qd N] [--xfer SECTORS] [--region LBAS]\n"
            "             [--write-through] [--fua] [--no-readahead] [--unsorted-destage]\n"
            "             [--cache ENTRIES] [--time-scale X] [--flip-prob P]\n"
            "             [--media PATH] [--seed N] [--quiet]\n");
}

static int parse_kind(const char *s, wl_kind_t *k)
{
    static const wl_kind_t all[] = { WL_RAND_WRITE, WL_RAND_READ, WL_SEQ_READ,
                                     WL_SEQ_WRITE, WL_MIXED };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (strcmp(s, wl_kind_str(all[i])) == 0) {
            *k = all[i];
            return 0;
        }
    return -1;
}

int main(int argc, char **argv)
{
    fw_config_t cfg;
    fw_config_default(&cfg);
    cfg.time_scale = 0.02;
    wl_params_t wl;
    wl_params_default(&wl);
    wl.final_flush = 1;
    int quiet = 0, own_media = 1;
    char media[256];
    snprintf(media, sizeof(media), "/tmp/fwsim-cli-%d.media", (int)getpid());
    cfg.media_path = media;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
#define NEEDV() do { if (!v) { usage(); return 2; } i++; } while (0)
        if (!strcmp(a, "--workload")) {
            NEEDV();
            if (parse_kind(v, &wl.kind) != 0) {
                usage();
                return 2;
            }
        } else if (!strcmp(a, "--ops")) {
            NEEDV(); wl.ops = strtoull(v, NULL, 0);
        } else if (!strcmp(a, "--qd")) {
            NEEDV(); wl.qd = (uint32_t)strtoul(v, NULL, 0);
        } else if (!strcmp(a, "--xfer")) {
            NEEDV(); wl.xfer_sectors = (uint32_t)strtoul(v, NULL, 0);
        } else if (!strcmp(a, "--region")) {
            NEEDV(); wl.region_lbas = strtoull(v, NULL, 0);
        } else if (!strcmp(a, "--cache")) {
            NEEDV(); cfg.cache_entries = strtoul(v, NULL, 0);
        } else if (!strcmp(a, "--time-scale")) {
            NEEDV(); cfg.time_scale = strtod(v, NULL);
        } else if (!strcmp(a, "--flip-prob")) {
            NEEDV(); cfg.fault_read_flip_prob = strtod(v, NULL);
        } else if (!strcmp(a, "--media")) {
            NEEDV(); snprintf(media, sizeof(media), "%s", v); own_media = 0;
        } else if (!strcmp(a, "--seed")) {
            NEEDV(); cfg.seed = wl.seed = strtoull(v, NULL, 0);
        } else if (!strcmp(a, "--write-through")) {
            cfg.write_cache = 0;
        } else if (!strcmp(a, "--fua")) {
            wl.fua = 1;
        } else if (!strcmp(a, "--no-readahead")) {
            cfg.read_ahead_sectors = 0;
        } else if (!strcmp(a, "--unsorted-destage")) {
            cfg.destage_sorted = 0;
        } else if (!strcmp(a, "--quiet")) {
            quiet = 1;
        } else {
            usage();
            return 2;
        }
#undef NEEDV
    }

    int err = 0;
    fw_t *fw = fw_create(&cfg, &err);
    if (!fw) {
        fprintf(stderr, "fwsim: cannot create drive (%d)\n", err);
        return 1;
    }

    uint8_t idbuf[4096];
    fw_cmd_t id;
    memset(&id, 0, sizeof(id));
    id.opcode = FW_OP_IDENTIFY;
    id.buf = idbuf;
    if (fw_exec(fw, &id) != FW_OK) {
        fprintf(stderr, "fwsim: IDENTIFY failed\n");
        fw_destroy(fw);
        return 1;
    }
    fw_identify_t ident;
    memcpy(&ident, idbuf, sizeof(ident));

    wl_result_t res;
    int rc = wl_run(fw, &wl, &res);
    fw_shutdown(fw);
    fw_stats_t st;
    fw_get_stats(fw, &st);

    if (!quiet) {
        double scale = cfg.time_scale > 0 ? cfg.time_scale : 1.0;
        lat_summary_t s;
        lat_summarize(&res.lat_all, &s);
        printf("drive     : %s, fw %s, %" PRIu64 " LBAs x %u B, QD %u, queue=%s\n",
               ident.model, ident.firmware_rev, ident.capacity_lbas,
               ident.sector_size, ident.queue_depth, ident.queue_impl);
        printf("cache     : %u entries, %s, read-ahead %s, destage %s\n",
               ident.cache_entries, ident.write_cache_enabled ? "write-back" : "write-through",
               ident.read_ahead_enabled ? "on" : "off",
               cfg.destage_sorted ? "LBA-sorted" : "age order");
        printf("workload  : %s, %" PRIu64 " ops x %u sectors, QD %u%s\n",
               wl_kind_str(wl.kind), wl.ops, wl.xfer_sectors, wl.qd, wl.fua ? ", FUA" : "");
        printf("time      : drive-time = wall / %.3g\n", scale);
        printf("throughput: %.1f IOPS acked, %.1f IOPS incl. final FLUSH\n",
               (double)res.ops / ((double)res.wall_ns / 1e9 / scale),
               (double)res.ops / ((double)res.wall_ns_durable / 1e9 / scale));
        printf("latency   : avg %.3f ms  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f\n",
               s.avg_ns / 1e6 * scale, s.p50_ns / 1e6 * scale, s.p95_ns / 1e6 * scale,
               s.p99_ns / 1e6 * scale, s.max_ns / 1e6 * scale);
        uint64_t rd = st.read_hit_sectors + st.read_miss_sectors;
        printf("cache     : read hit rate %.1f%% (%" PRIu64 "/%" PRIu64 " sectors), "
               "write hits %" PRIu64 ", read-ahead %" PRIu64 " ops\n",
               rd ? 100.0 * (double)st.read_hit_sectors / (double)rd : 0.0,
               st.read_hit_sectors, rd, st.write_hit_sectors, st.readahead_ops);
        printf("destage   : %" PRIu64 " runs, %" PRIu64 " sectors (threshold %" PRIu64
               ", forced %" PRIu64 ", idle %" PRIu64 ", flush %" PRIu64 ")\n",
               st.destage_runs, st.destaged_sectors, st.threshold_destages,
               st.forced_destages, st.idle_destages, st.flushes);
        printf("media     : %" PRIu64 " reads, %" PRIu64 " writes, busy %.1f ms (simulated)\n",
               st.media_reads, st.media_writes, st.media_busy_ms);
        printf("integrity : %" PRIu64 " CRC errors, %" PRIu64 " recovered by retry, %" PRIu64
               " unrecovered, %" PRIu64 " injected flips\n",
               st.crc_errors, st.crc_recovered, st.crc_unrecovered, st.transient_flips);
        printf("errors    : %" PRIu64 " commands failed\n", res.errors);
    }
    wl_result_free(&res);
    fw_destroy(fw);
    if (own_media)
        unlink(media);
    return (rc == 0 && res.errors == 0) ? 0 : 1;
}
