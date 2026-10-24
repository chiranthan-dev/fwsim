/*
 * edgeai_cli.c - the bridge between the C feature/inference code and the
 * Python training tools, plus the classifier benchmark. Python never
 * re-implements a feature: it asks this program, so training and the
 * drive use bit-identical features.
 *
 *   edgeai_cli dataset --window N [--hop H] --seeds A-B [--secs S] [--mix M] [--out F]
 *       one CSV row per window: seed,t_ms,label,f0..f8
 *   edgeai_cli predict --window N --model mlp|tree --in F
 *       C int8 / tree inference on a dataset CSV: one "pred,conf" per row
 *   edgeai_cli eval --window N [--model mlp|tree|deployed] --seeds A-B
 *                   [--secs S] [--mix M] [--hop H] [--threshold MG]
 *       streaming evaluation on whole recordings: confusion matrix, F1,
 *       shock detection latency vs a threshold detector, taps (JSON)
 *   edgeai_cli cost --window N
 *       host-CPU time per window for features and inference, model bytes
 *
 * A "recording" is one seed of env_sim pushed through the accelerometer's
 * digitisation (accel_model_digitize: oversample-average, sensor noise,
 * int16) at 1 kHz - the same path the drive's sensor uses. The SPI / FIFO
 * transport in between is lossless (tests/test_drivers checks FIFO order),
 * so it is skipped here for speed.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "edgeai/features.h"
#include "edgeai/infer.h"
#include "periph/accel_model.h"
#include "periph/env_sim.h"

#define DEFAULT_HOP 16u

typedef struct {
    int16_t *x;
    uint8_t *lab;
    unsigned n;
} rec_t;

static int record(rec_t *r, int mix, uint64_t seed, unsigned secs)
{
    env_sim_t e;
    if (env_init(&e, (env_mix_t)mix, seed) != 0)
        return -1;
    r->n = secs * EDGEAI_FS_HZ;
    r->x = malloc(r->n * sizeof(*r->x));
    r->lab = malloc(r->n);
    if (!r->x || !r->lab) {
        env_free(&e);
        return -1;
    }
    uint64_t rng = seed ^ ACCEL_RNG_SALT;
    for (unsigned k = 0; k < r->n; k++) {
        uint64_t t = (uint64_t)(k + 1) * (1000000u / EDGEAI_FS_HZ);
        r->x[k] = accel_model_digitize(env_accel_source, &e, t, EDGEAI_FS_HZ,
                                       ACCEL_NOISE_MG_DEFAULT, &rng);
        r->lab[k] = (uint8_t)env_label(&e, t);
    }
    env_free(&e);
    return 0;
}

static void rec_free(rec_t *r)
{
    free(r->x);
    free(r->lab);
}

/* ---- options ------------------------------------------------------------------- */

typedef struct {
    unsigned window, hop, secs, thr;
    uint64_t seed_lo, seed_hi;
    int      mix;
    const char *model, *out, *in;
} opts_t;

static int parse(int argc, char **argv, opts_t *o)
{
    memset(o, 0, sizeof(*o));
    o->hop = DEFAULT_HOP;
    o->secs = 30;
    o->thr = 500;
    o->mix = ENV_MIX_TRAIN;
    o->model = "deployed";
    o->seed_lo = 1;
    o->seed_hi = 1;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v)
            return -1;
        i++;
        if (!strcmp(a, "--window"))
            o->window = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--hop"))
            o->hop = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--secs"))
            o->secs = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--threshold"))
            o->thr = (unsigned)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--model"))
            o->model = v;
        else if (!strcmp(a, "--out"))
            o->out = v;
        else if (!strcmp(a, "--in"))
            o->in = v;
        else if (!strcmp(a, "--mix")) {
            if ((o->mix = env_mix_parse(v)) <= 0)
                return -1;
        } else if (!strcmp(a, "--seeds")) {
            char *dash;
            o->seed_lo = strtoull(v, &dash, 0);
            o->seed_hi = *dash == '-' ? strtoull(dash + 1, NULL, 0) : o->seed_lo;
        } else
            return -1;
    }
    if (o->window < 8 || o->window > EDGEAI_MAX_WINDOW || o->hop == 0 || o->secs == 0)
        return -1;
    return 0;
}

static int run_model(const opts_t *o, const edgeai_model_t *m, const int32_t *vec, edgeai_out_t *out)
{
    if (!strcmp(o->model, "mlp"))
        return edgeai_infer_mlp(m->mlp, vec, out);
    if (!strcmp(o->model, "tree"))
        return edgeai_infer_tree(&m->tree, vec, out);
    return edgeai_classify(m, vec, out);
}

/* ---- dataset ---------------------------------------------------------------------- */

static int cmd_dataset(const opts_t *o)
{
    FILE *f = o->out ? fopen(o->out, "w") : stdout;
    if (!f)
        return 1;
    fprintf(f, "seed,t_ms,label");
    for (int j = 0; j < EDGEAI_NFEAT; j++)
        fprintf(f, ",f%d", j);
    fprintf(f, "\n");
    for (uint64_t s = o->seed_lo; s <= o->seed_hi; s++) {
        rec_t r;
        if (record(&r, o->mix, s, o->secs) != 0)
            return 1;
        for (unsigned e = o->window; e <= r.n; e += o->hop) {
            edgeai_feat_t ft;
            edgeai_features(r.x + e - o->window, o->window, &ft);
            fprintf(f, "%" PRIu64 ",%u,%d", s, e, edgeai_window_label(r.lab + e - o->window, o->window));
            for (int j = 0; j < EDGEAI_NFEAT; j++)
                fprintf(f, ",%d", ft.vec[j]);
            fprintf(f, "\n");
        }
        rec_free(&r);
    }
    if (f != stdout)
        fclose(f);
    return 0;
}

/* ---- predict ------------------------------------------------------------------------- */

static int cmd_predict(const opts_t *o)
{
    const edgeai_model_t *m = edgeai_model_for(o->window);
    FILE *f = o->in ? fopen(o->in, "r") : stdin;
    if (!m || !f) {
        fprintf(stderr, "edgeai_cli: no model for window %u or no input\n", o->window);
        return 1;
    }
    char line[1024];
    if (!fgets(line, sizeof(line), f))          /* header */
        return 1;
    printf("pred,conf\n");
    while (fgets(line, sizeof(line), f)) {
        int32_t v[EDGEAI_NFEAT];
        char *p = line;
        for (int skip = 0; skip < 3; skip++)    /* seed, t_ms, label */
            p = strchr(p, ',') + 1;
        for (int j = 0; j < EDGEAI_NFEAT; j++) {
            v[j] = (int32_t)strtol(p, &p, 10);
            if (*p == ',')
                p++;
        }
        edgeai_out_t out;
        if (run_model(o, m, v, &out) != 0)
            return 1;
        printf("%u,%u\n", out.cls, out.conf_pct);
    }
    if (f != stdin)
        fclose(f);
    return 0;
}

/* ---- eval ---------------------------------------------------------------------------- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static double pct(uint32_t *v, size_t n, double q)
{
    if (!n)
        return -1;
    qsort(v, n, sizeof(*v), cmp_u32);
    size_t k = (size_t)(q * (double)(n - 1) + 0.5);
    return v[k];
}

#define MAX_EVENTS 200000

static int cmd_eval(const opts_t *o)
{
    const edgeai_model_t *m = edgeai_model_for(o->window);
    if (!m) {
        fprintf(stderr, "edgeai_cli: no model for window %u\n", o->window);
        return 1;
    }
    uint64_t cm[EDGEAI_NCLASSES][EDGEAI_NCLASSES] = { { 0 } };
    uint32_t *lat_ai = malloc(MAX_EVENTS * 4), *lat_thr = malloc(MAX_EVENTS * 4);
    size_t n_ai = 0, n_thr = 0;
    uint64_t shocks = 0, taps = 0, tap_ai_shock = 0, tap_thr = 0, vib_segs = 0, vib_seg_thr = 0;
    uint64_t vib_samples = 0, vib_over = 0, windows = 0;
    for (uint64_t s = o->seed_lo; s <= o->seed_hi; s++) {
        rec_t r;
        if (record(&r, o->mix, s, o->secs) != 0)
            return 1;
        /* pred[e] for each window ending at sample e (1-based count), 255 = none */
        uint8_t *pred = malloc(r.n + 1);
        memset(pred, 255, r.n + 1);
        for (unsigned e = o->window; e <= r.n; e += o->hop) {
            edgeai_feat_t ft;
            edgeai_out_t out;
            edgeai_features(r.x + e - o->window, o->window, &ft);
            if (run_model(o, m, ft.vec, &out) != 0)
                return 1;
            pred[e] = out.cls;
            cm[edgeai_window_label(r.lab + e - o->window, o->window)][out.cls]++;
            windows++;
        }
        for (unsigned k = 0; k < r.n; k++) {
            int onset = k == 0 || r.lab[k - 1] != r.lab[k];
            unsigned a = (unsigned)abs(r.x[k]);
            if (r.lab[k] == ENV_VIBRATION) {
                vib_samples++;
                vib_over += a >= o->thr;
            }
            if (!onset)
                continue;
            if (r.lab[k] == ENV_SHOCK && k >= o->window) {
                shocks++;
                for (unsigned e = k + 1; e <= r.n && e <= k + 500; e++)
                    if (pred[e] == ENV_SHOCK) {
                        if (n_ai < MAX_EVENTS)
                            lat_ai[n_ai++] = e - (k + 1);
                        break;
                    }
                for (unsigned j = k; j < r.n && j < k + 500; j++)
                    if ((unsigned)abs(r.x[j]) >= o->thr) {
                        if (n_thr < MAX_EVENTS)
                            lat_thr[n_thr++] = j - k;
                        break;
                    }
            } else if (r.lab[k] == ENV_TAP && k >= o->window) {
                taps++;
                int ai = 0, th = 0;
                for (unsigned e = k + 1; e <= r.n && e <= k + 1 + o->window; e++)
                    ai |= pred[e] == ENV_SHOCK;
                for (unsigned j = k; j < r.n && j < k + 20; j++)
                    th |= (unsigned)abs(r.x[j]) >= o->thr;
                tap_ai_shock += ai;
                tap_thr += th;
            } else if (r.lab[k] == ENV_VIBRATION) {
                vib_segs++;
                int th = 0;
                for (unsigned j = k; j < r.n && r.lab[j] == ENV_VIBRATION; j++)
                    th |= (unsigned)abs(r.x[j]) >= o->thr;
                vib_seg_thr += th;
            }
        }
        free(pred);
        rec_free(&r);
    }
    double f1sum = 0, rec[EDGEAI_NCLASSES], prec[EDGEAI_NCLASSES], f1[EDGEAI_NCLASSES];
    uint64_t correct = 0;
    for (int c = 0; c < EDGEAI_NCLASSES; c++) {
        uint64_t tp = cm[c][c], row = 0, col = 0;
        for (int k = 0; k < EDGEAI_NCLASSES; k++) {
            row += cm[c][k];
            col += cm[k][c];
        }
        correct += tp;
        rec[c] = row ? (double)tp / (double)row : 0;
        prec[c] = col ? (double)tp / (double)col : 0;
        f1[c] = rec[c] + prec[c] > 0 ? 2 * rec[c] * prec[c] / (rec[c] + prec[c]) : 0;
        f1sum += f1[c];
    }
    printf("{\"window\":%u,\"hop\":%u,\"model\":\"%s\",\"mix\":\"%s\",\"seeds\":\"%" PRIu64 "-%" PRIu64
           "\",\"windows\":%" PRIu64 ",\n \"confusion\":[", o->window, o->hop,
           !strcmp(o->model, "deployed") ? (m->selected == EDGEAI_MODEL_TREE ? "tree" : "mlp") : o->model,
           env_mix_str(o->mix), o->seed_lo, o->seed_hi, windows);
    for (int c = 0; c < EDGEAI_NCLASSES; c++)
        printf("%s[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]", c ? "," : "", cm[c][0], cm[c][1],
               cm[c][2], cm[c][3]);
    printf("],\n \"recall\":[%.4f,%.4f,%.4f,%.4f],\"precision\":[%.4f,%.4f,%.4f,%.4f],"
           "\"f1\":[%.4f,%.4f,%.4f,%.4f],\"macro_f1\":%.4f,\"accuracy\":%.4f,\n",
           rec[0], rec[1], rec[2], rec[3], prec[0], prec[1], prec[2], prec[3], f1[0], f1[1], f1[2],
           f1[3], f1sum / EDGEAI_NCLASSES, windows ? (double)correct / (double)windows : 0);
    size_t nai = n_ai, nthr = n_thr;
    double ai50 = pct(lat_ai, nai, 0.5), ai90 = pct(lat_ai, nai, 0.9);
    double th50 = pct(lat_thr, nthr, 0.5), th90 = pct(lat_thr, nthr, 0.9);
    printf(" \"threshold_mg\":%u,\"shock_events\":%" PRIu64 ",\"shock_detected_ai\":%zu,"
           "\"shock_latency_ai_ms_p50\":%.0f,\"shock_latency_ai_ms_p90\":%.0f,"
           "\"shock_detected_thr\":%zu,\"shock_latency_thr_ms_p50\":%.0f,"
           "\"shock_latency_thr_ms_p90\":%.0f,\n \"tap_events\":%" PRIu64
           ",\"tap_classified_shock\":%" PRIu64 ",\"tap_tripped_threshold\":%" PRIu64
           ",\"vibration_segments\":%" PRIu64 ",\"vibration_segments_tripping_threshold\":%" PRIu64
           ",\"vibration_samples_over_threshold\":%.4f}\n",
           o->thr, shocks, nai, ai50, ai90, nthr, th50, th90, taps, tap_ai_shock, tap_thr, vib_segs,
           vib_seg_thr, vib_samples ? (double)vib_over / (double)vib_samples : 0);
    free(lat_ai);
    free(lat_thr);
    return 0;
}

/* ---- cost ----------------------------------------------------------------------------- */

static int cmd_cost(const opts_t *o)
{
    const edgeai_model_t *m = edgeai_model_for(o->window);
    rec_t r;
    if (!m || record(&r, ENV_MIX_MIXED, 9999, 20) != 0)
        return 1;
    unsigned nw = 0;
    edgeai_feat_t ft;
    int32_t (*vec)[EDGEAI_NFEAT] = malloc(sizeof(*vec) * (r.n / o->hop + 1));
    volatile unsigned sink = 0;
    const int reps = 5;
    uint64_t t0 = now_ns();
    for (int rep = 0; rep < reps; rep++) {
        nw = 0;
        for (unsigned e = o->window; e <= r.n; e += o->hop) {
            edgeai_features(r.x + e - o->window, o->window, &ft);
            memcpy(vec[nw++], ft.vec, sizeof(ft.vec));
        }
    }
    double feat_ns = (double)(now_ns() - t0) / reps / nw;
    edgeai_out_t out;
    t0 = now_ns();
    for (int rep = 0; rep < reps * 4; rep++)
        for (unsigned i = 0; i < nw; i++) {
            edgeai_infer_mlp(m->mlp, vec[i], &out);
            sink += out.cls;
        }
    double mlp_ns = (double)(now_ns() - t0) / (reps * 4) / nw;
    t0 = now_ns();
    for (int rep = 0; rep < reps * 4; rep++)
        for (unsigned i = 0; i < nw; i++) {
            edgeai_infer_tree(&m->tree, vec[i], &out);
            sink += out.cls;
        }
    double tree_ns = (double)(now_ns() - t0) / (reps * 4) / nw;
    printf("{\"window\":%u,\"hop\":%u,\"window_ms\":%u,\"windows_timed\":%u,"
           "\"features_ns\":%.0f,\"mlp_ns\":%.0f,\"tree_ns\":%.0f,\"mlp_bytes\":%u,"
           "\"tree_bytes\":%u,\"tree_nodes\":%u,\"goertzel_mults\":%u,\"selected\":\"%s\","
           "\"timing\":\"host CPU wall clock, not an MCU\"}\n",
           o->window, o->hop, o->window * 1000u / EDGEAI_FS_HZ, nw, feat_ns, mlp_ns, tree_ns,
           edgeai_mlp_bytes(), edgeai_tree_bytes(&m->tree), m->tree.nnodes,
           EDGEAI_NBANDS * EDGEAI_BINS_PER_BAND * o->window,
           m->selected == EDGEAI_MODEL_TREE ? "tree" : "mlp");
    (void)sink;
    free(vec);
    rec_free(&r);
    return 0;
}

int main(int argc, char **argv)
{
    opts_t o;
    if (argc < 2 || parse(argc, argv, &o) != 0) {
        fprintf(stderr, "usage: edgeai_cli dataset|predict|eval|cost --window N [options]\n"
                        "       (see the comment at the top of tools/edgeai/edgeai_cli.c)\n");
        return 2;
    }
    if (!strcmp(argv[1], "dataset"))
        return cmd_dataset(&o);
    if (!strcmp(argv[1], "predict"))
        return cmd_predict(&o);
    if (!strcmp(argv[1], "eval"))
        return cmd_eval(&o);
    if (!strcmp(argv[1], "cost"))
        return cmd_cost(&o);
    fprintf(stderr, "edgeai_cli: unknown command %s\n", argv[1]);
    return 2;
}
