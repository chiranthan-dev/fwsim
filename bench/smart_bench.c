/*
 * smart_bench.c - `fwsim-smart`: the exported SMART model outside the drive.
 *
 *   fwsim-smart score IN.int16 OUT.int32
 *       IN : N x SMART_MODEL_NFEAT little-endian int16 features (written by
 *            tools/smart/train.py from the Backblaze test set)
 *       OUT: N x 2 int32: logit_q8, risk_bp - what the firmware would report.
 *       tools/smart/agree.py compares OUT with the Python model.
 *
 *   fwsim-smart bench [--csv FILE]
 *       model size in bytes and time per inference call ON THE HOST CPU
 *       (not drive-controller timing: a Cortex-R class core is slower and
 *       has no out-of-order execution).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "smart.h"
#include "util.h"

static int cmd_score(const char *in, const char *out)
{
    FILE *fi = fopen(in, "rb"), *fo = fopen(out, "wb");
    if (!fi || !fo) {
        fprintf(stderr, "fwsim-smart: cannot open %s or %s\n", in, out);
        return 1;
    }
    int16_t f[SMART_MODEL_NFEAT];
    uint64_t n = 0;
    while (fread(f, sizeof(f), 1, fi) == 1) {
        int32_t r[2];
        r[0] = smart_logit_q8(f);
        r[1] = smart_risk_bp(r[0]);
        if (fwrite(r, sizeof(r), 1, fo) != 1) {
            fprintf(stderr, "fwsim-smart: write failed\n");
            return 1;
        }
        n++;
    }
    fclose(fi);
    if (fclose(fo) != 0)
        return 1;
    fprintf(stderr, "fwsim-smart: scored %" PRIu64 " rows\n", n);
    return 0;
}

/* A drive state with a full 32-day history and some growing counters,
 * so smart_features() walks the whole ring like it would in the field. */
static void make_state(smart_t *s, uint64_t seed)
{
    smart_init(s);
    uint64_t c[SMART_NCOUNTERS] = { 0 };
    for (uint64_t d = 0; d < 40; d++) {
        for (int k = 0; k < SMART_NCOUNTERS; k++)
            c[k] += rng_below64(&seed, 3);
        smart_update(s, (833 + d) * SMART_MS_PER_DAY, c, 35);   /* ~20,000 h old */
    }
}

static volatile int64_t sink;

static double time_ns_per_call(int what, uint64_t iters)
{
    enum { NSTATES = 64 };
    static smart_t st[NSTATES];
    static int16_t feats[NSTATES][SMART_MODEL_NFEAT];
    for (int i = 0; i < NSTATES; i++) {
        make_state(&st[i], (uint64_t)i * 7919 + 1);
        smart_features(&st[i], feats[i]);
    }
    double best = 1e30;
    for (int rep = 0; rep < 5; rep++) {     /* best of 5: least disturbed */
        int64_t acc = 0;
        uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < iters; i++) {
            int j = (int)(i % NSTATES);
            if (what == 0) {
                acc += smart_logit_q8(feats[j]);
            } else if (what == 1) {
                acc += smart_risk_bp(smart_logit_q8(feats[j]));
            } else {
                int16_t f[SMART_MODEL_NFEAT];
                smart_features(&st[j], f);
                acc += smart_risk_bp(smart_logit_q8(f));
            }
        }
        uint64_t t1 = now_ns();
        sink += acc;
        double ns = (double)(t1 - t0) / (double)iters;
        if (ns < best)
            best = ns;
    }
    return best;
}

static int cmd_bench(const char *csv_path)
{
    size_t model_bytes = sizeof(SMART_MODEL_W) + sizeof(SMART_MODEL_ZP) + sizeof(SMART_SIGMOID_LUT) +
                         2 * sizeof(int64_t) + sizeof(int32_t);  /* MULT, BIAS, THRESH */
    size_t state_bytes = sizeof(smart_t);
    double t_logit = time_ns_per_call(0, 20000000);
    double t_risk = time_ns_per_call(1, 20000000);
    double t_full = time_ns_per_call(2, 5000000);

    printf("SMART model (exported logistic regression, %d features)\n", SMART_MODEL_NFEAT);
    printf("  model tables     : %zu bytes (int%d weights %zu + zero points %zu + sigmoid LUT %zu"
           " + 20 B constants)\n", model_bytes, SMART_MODEL_WEIGHT_BITS, sizeof(SMART_MODEL_W),
           sizeof(SMART_MODEL_ZP), sizeof(SMART_SIGMOID_LUT));
    printf("  per-drive state  : %zu bytes (32-day history ring + current values)\n", state_bytes);
    printf("  host CPU timing (NOT drive-controller timing), best of 5:\n");
    printf("    logit only               : %7.2f ns/call\n", t_logit);
    printf("    logit + risk (LUT)       : %7.2f ns/call\n", t_risk);
    printf("    features + logit + risk  : %7.2f ns/call\n", t_full);

    if (csv_path) {
        FILE *f = fopen(csv_path, "w");
        if (!f)
            return 1;
        fprintf(f, "metric,value,unit\n");
        fprintf(f, "features,%d,count\n", SMART_MODEL_NFEAT);
        fprintf(f, "model_bytes,%zu,bytes\n", model_bytes);
        fprintf(f, "weight_bits,%d,bits\n", SMART_MODEL_WEIGHT_BITS);
        fprintf(f, "weights_bytes,%zu,bytes\n", sizeof(SMART_MODEL_W));
        fprintf(f, "zp_bytes,%zu,bytes\n", sizeof(SMART_MODEL_ZP));
        fprintf(f, "lut_bytes,%zu,bytes\n", sizeof(SMART_SIGMOID_LUT));
        fprintf(f, "state_bytes,%zu,bytes\n", state_bytes);
        fprintf(f, "host_ns_logit,%.3f,ns/call\n", t_logit);
        fprintf(f, "host_ns_logit_risk,%.3f,ns/call\n", t_risk);
        fprintf(f, "host_ns_features_logit_risk,%.3f,ns/call\n", t_full);
        fclose(f);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "score"))
        return cmd_score(argv[2], argv[3]);
    if (argc >= 2 && !strcmp(argv[1], "bench"))
        return cmd_bench(argc == 4 && !strcmp(argv[2], "--csv") ? argv[3] : NULL);
    fprintf(stderr, "usage: fwsim-smart score IN.int16 OUT.int32\n"
                    "       fwsim-smart bench [--csv FILE]\n");
    return 2;
}
