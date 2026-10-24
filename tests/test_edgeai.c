/*
 * test_edgeai.c - the classifier's C side: fixed-point features against
 * hand-computed values, the window labelling rule, and the exported
 * models against the golden vectors written by tools/edgeai/export.py
 * (Python float + bit-exact int8 simulation on held-out test seeds).
 */
#include <math.h>

#include "check.h"
#include "edgeai/features.h"
#include "edgeai/infer.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846   /* not in strict C11 / POSIX */
#endif

static void sine(int16_t *x, unsigned n, double amp, double f_hz)
{
    for (unsigned i = 0; i < n; i++)
        x[i] = (int16_t)lrint(amp * sin(2.0 * M_PI * f_hz * i / EDGEAI_FS_HZ));
}

/* |X(f)|^2 of A sin(w n) over N samples at its own frequency is (A N / 2)^2
 * (plus a small leakage term from the negative-frequency image). */
static void test_goertzel_matches_hand_computed_sine(void)
{
    const unsigned n = 256;
    int16_t x16[256];
    int32_t x[256];
    sine(x16, n, 1000.0, 150.0);
    for (unsigned i = 0; i < n; i++)
        x[i] = x16[i];
    /* band 2, bin 1 is 150 Hz: 2 cos(2 pi 0.15) * 2^14 = 19261.4 */
    CHECK_EQ(edgeai_bin_hz_x10[2][1], 1500);
    CHECK_EQ(edgeai_bin_coeff[2][1], 19261);
    double expect = (1000.0 * n / 2) * (1000.0 * n / 2);
    double got = (double)edgeai_goertzel_pow(x, n, edgeai_bin_coeff[2][1]);
    printf("    150 Hz, A = 1000, N = 256: |X|^2 = %.4g (hand: %.4g, ratio %.4f)\n", got, expect,
           got / expect);
    CHECK(fabs(got / expect - 1.0) < 0.03);
    /* Far from the tone (412.5 Hz) almost nothing. */
    double off = (double)edgeai_goertzel_pow(x, n, edgeai_bin_coeff[4][2]);
    CHECK(off < 0.001 * expect);

    /* Through the feature vector: the 120-200 Hz band dominates, and its
     * log2 amplitude is log2(1000) ~ 9.97 (Q8 ~ 2552); the four bins of a
     * band are summed, the tone sits on one of them. */
    edgeai_feat_t f;
    CHECK_EQ(edgeai_features(x16, n, &f), 0);
    for (int b = 0; b < EDGEAI_NBANDS; b++)
        if (b != 2)
            CHECK(f.band_pow[b] < f.band_pow[2] / 10);
    CHECK(abs(f.vec[6] - 2552) < 40);
}

static void test_rms_peak_crest_on_known_signals(void)
{
    edgeai_feat_t f;
    int16_t x[128];

    /* Sine, A = 2000 at 125 Hz (8 samples per period, 16 whole periods):
     * RMS = A / sqrt 2 = 1414.2, peak = A, crest = sqrt 2 (Q8 362). */
    sine(x, 128, 2000.0, 125.0);
    CHECK_EQ(edgeai_features(x, 128, &f), 0);
    printf("    sine A=2000: rms %u, peak %u, crest %.3f, zc %.3f/sample\n", f.rms, f.peak,
           f.crest_q8 / 256.0, f.zc_q10 / 1024.0);
    CHECK(abs((int)f.rms - 1414) <= 2);
    CHECK(abs((int)f.peak - 2000) <= 1);
    CHECK(abs((int)f.crest_q8 - 362) <= 2);
    /* 125 Hz crosses zero 2 * 125 / 1000 = 0.25 times per sample. */
    CHECK(abs((int)f.zc_q10 - 256) <= 12);

    /* Square wave +-500: RMS = peak = 500, crest exactly 1. */
    for (int i = 0; i < 128; i++)
        x[i] = (i / 4) % 2 ? -500 : 500;
    CHECK_EQ(edgeai_features(x, 128, &f), 0);
    CHECK_EQ(f.rms, 500);
    CHECK_EQ(f.peak, 500);
    CHECK_EQ(f.crest_q8, 256);

    /* One impulse in silence: crest = peak / rms = sqrt(N) for a single
     * non-zero sample (after mean removal, approximately). */
    memset(x, 0, sizeof(x));
    x[60] = 3000;
    CHECK_EQ(edgeai_features(x, 128, &f), 0);
    CHECK(f.crest_q8 > 10 * 256);                 /* sqrt(128) = 11.3 */

    /* A constant (DC) is removed: no RMS, no peak, no crossings. */
    for (int i = 0; i < 128; i++)
        x[i] = 777;
    CHECK_EQ(edgeai_features(x, 128, &f), 0);
    CHECK_EQ(f.rms, 0);
    CHECK_EQ(f.peak, 0);
    CHECK_EQ(f.zc_q10, 0);
    CHECK(edgeai_features(x, 4, &f) != 0);        /* too short */
}

static void test_fixed_point_helpers(void)
{
    CHECK_EQ(edgeai_log2_q8(1), 0);
    CHECK_EQ(edgeai_log2_q8(2), 256);
    CHECK_EQ(edgeai_log2_q8(1024), 2560);
    CHECK_EQ(edgeai_log2_q8(3), 256 + 128);       /* linear fraction: 1.5 -> .5 */
    CHECK_EQ(edgeai_log2_q8(0), 0);
    CHECK_EQ(edgeai_isqrt64(0), 0);
    CHECK_EQ(edgeai_isqrt64(99), 9);
    CHECK_EQ(edgeai_isqrt64(100), 10);
    CHECK_EQ(edgeai_isqrt64(1ull << 62), 1u << 31);
}

static void test_window_label_rule(void)
{
    uint8_t l[64];
    memset(l, 0, sizeof(l));
    CHECK_EQ(edgeai_window_label(l, 64), 0);
    l[3] = l[9] = 3;
    CHECK_EQ(edgeai_window_label(l, 64), 3);                /* tap */
    memset(l, 2, 32);
    CHECK_EQ(edgeai_window_label(l, 64), 2);                /* half vibration */
    l[40] = l[41] = l[42] = 1;
    CHECK_EQ(edgeai_window_label(l, 64), 2);                /* 3 shock samples: not yet */
    l[43] = 1;
    CHECK_EQ(edgeai_window_label(l, 64), 1);                /* shock wins */
}

/* ---- models vs golden vectors ----------------------------------------------------------- */

static void check_golden(unsigned window)
{
    char p[64];
    snprintf(p, sizeof(p), "tests/data/edgeai_golden_%u.csv", window);
    FILE *f = fopen(p, "r");
    CHECK(f != NULL);
    const edgeai_model_t *m = edgeai_model_for(window);
    CHECK(m != NULL);
    if (!f || !m) {
        if (f)
            fclose(f);
        return;
    }
    char line[512];
    CHECK(fgets(line, sizeof(line), f) != NULL);
    unsigned n = 0, mlp_vs_int8 = 0, mlp_vs_float = 0, tree_ok = 0, correct = 0;
    while (fgets(line, sizeof(line), f)) {
        int lab, pf, pq, pt;
        int32_t x[EDGEAI_NFEAT];
        char *s = line;
        lab = (int)strtol(s, &s, 10);
        pf = (int)strtol(s + 1, &s, 10);
        pq = (int)strtol(s + 1, &s, 10);
        pt = (int)strtol(s + 1, &s, 10);
        for (int j = 0; j < EDGEAI_NFEAT; j++)
            x[j] = (int32_t)strtol(s + 1, &s, 10);
        edgeai_out_t a, b;
        CHECK_EQ(edgeai_infer_mlp(m->mlp, x, &a), 0);
        CHECK_EQ(edgeai_infer_tree(&m->tree, x, &b), 0);
        n++;
        mlp_vs_int8 += a.cls == pq;
        mlp_vs_float += a.cls == pf;
        tree_ok += b.cls == pt;
        edgeai_out_t d;
        edgeai_classify(m, x, &d);
        correct += d.cls == lab;
        CHECK(a.conf_pct <= 100 && a.conf_pct >= 25);    /* >= 1/4 for 4 classes */
    }
    fclose(f);
    printf("    window %u: %u golden windows; C int8 == numpy int8 %u, == Python float %u "
           "(%.2f%%), tree %u; deployed %s correct on %.1f%%\n", window, n, mlp_vs_int8,
           mlp_vs_float, 100.0 * mlp_vs_float / n, tree_ok,
           m->selected == EDGEAI_MODEL_TREE ? "tree" : "mlp", 100.0 * correct / n);
    CHECK(n >= 400);
    CHECK_EQ(mlp_vs_int8, n);                /* bit-exact with export.py's simulation */
    CHECK_EQ(tree_ok, n);                    /* the integer tree is the sklearn tree  */
    CHECK(mlp_vs_float * 100 >= n * 98);     /* quantisation: >= 98% agreement        */
}

static void test_models_match_python(void)
{
    CHECK(edgeai_default_window() != 0);
    CHECK(edgeai_model_id() != 0);
    check_golden(64);
    check_golden(128);
    check_golden(256);
    CHECK(edgeai_model_for(100) == NULL);
}

static void test_inference_is_deterministic(void)
{
    const edgeai_model_t *m = edgeai_model_for(edgeai_default_window());
    CHECK(m != NULL);
    if (!m)
        return;
    int16_t x[256];
    sine(x, 256, 150.0, 110.0);                    /* vibration-like */
    edgeai_feat_t f1, f2;
    edgeai_features(x, m->window, &f1);
    edgeai_features(x, m->window, &f2);
    CHECK(memcmp(f1.vec, f2.vec, sizeof(f1.vec)) == 0);
    edgeai_out_t a, b;
    memset(&a, 0, sizeof(a));          /* memcmp below also sees the padding */
    memset(&b, 0, sizeof(b));
    CHECK_EQ(edgeai_infer_mlp(m->mlp, f1.vec, &a), 0);
    for (int r = 0; r < 100; r++) {
        CHECK_EQ(edgeai_infer_mlp(m->mlp, f1.vec, &b), 0);
        CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    }
    /* Pure sensor-noise-level input should read as quiet. */
    for (int i = 0; i < 256; i++)
        x[i] = (int16_t)((i * 7919) % 5 - 2);
    edgeai_features(x, m->window, &f1);
    CHECK_EQ(edgeai_classify(m, f1.vec, &a), 0);
    CHECK_EQ(a.cls, 0);
}

int main(void)
{
    printf("test_edgeai\n");
    RUN_TEST(test_goertzel_matches_hand_computed_sine);
    RUN_TEST(test_rms_peak_crest_on_known_signals);
    RUN_TEST(test_fixed_point_helpers);
    RUN_TEST(test_window_label_rule);
    RUN_TEST(test_models_match_python);
    RUN_TEST(test_inference_is_deterministic);
    return test_summary("test_edgeai");
}
