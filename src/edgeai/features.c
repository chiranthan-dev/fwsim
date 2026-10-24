/*
 * features.c - fixed-point window features. Definitions and the reasons
 * for them in features.h. Integer-only: `make nofloat` compiles this file
 * with -mgeneral-regs-only, which rejects any floating-point code.
 *
 * Right shifts of negative int64 values are arithmetic (floor) on every
 * compiler we target (GCC/Clang define it so); Python's >> is floor too,
 * which keeps tools/edgeai/export.py's integer simulation bit-exact.
 */
#include "edgeai/features.h"

/* Q14 coefficients 2*cos(2*pi*f/1000) for the bin centres below (four
 * equal sub-bands per band, bin at each sub-band centre). */
const int32_t edgeai_bin_coeff[EDGEAI_NBANDS][EDGEAI_BINS_PER_BAND] = {
    { 32365, 31979, 31467, 30831 },      /*  25.0   35.0   45.0   55.0 Hz */
    { 29865, 28463, 26809, 24917 },      /*  67.5   82.5   97.5  112.5 Hz */
    { 22431, 19261, 15786, 12063 },      /* 130.0  150.0  170.0  190.0 Hz */
    { 7650, 2571, -2571, -7650 },        /* 212.5  237.5  262.5  287.5 Hz */
    { -14416, -22053, -27939, -31607 },  /* 322.5  367.5  412.5  457.5 Hz */
};

const uint16_t edgeai_bin_hz_x10[EDGEAI_NBANDS][EDGEAI_BINS_PER_BAND] = {
    { 250, 350, 450, 550 },     { 675, 825, 975, 1125 },    { 1300, 1500, 1700, 1900 },
    { 2125, 2375, 2625, 2875 }, { 3225, 3675, 4125, 4575 },
};

uint32_t edgeai_isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

int32_t edgeai_log2_q8(uint64_t v)
{
    if (v == 0)
        return 0;
    int e = 63 - __builtin_clzll(v);
    uint32_t frac = e >= 8 ? (uint32_t)(v >> (e - 8)) & 0xFFu : (uint32_t)(v << (8 - e)) & 0xFFu;
    return e * 256 + (int32_t)frac;
}

/*
 * Goertzel: s[n] = x[n] + c*s[n-1] - s[n-2], c = 2 cos(w), and after the
 * last sample |X(w)|^2 = s1^2 + s2^2 - c*s1*s2. Overflow bound: |s| stays
 * below ~|x|max * N / (2 sin w) ~ 2^25 for our lowest bin (25 Hz), N = 256
 * and full-scale input, so c*s1 < 2^41 and the squares < 2^51: int64 is
 * ample.
 */
uint64_t edgeai_goertzel_pow(const int32_t *x, unsigned n, int32_t c)
{
    int64_t s1 = 0, s2 = 0;
    for (unsigned i = 0; i < n; i++) {
        int64_t s = (int64_t)x[i] + (((int64_t)c * s1) >> 14) - s2;
        s2 = s1;
        s1 = s;
    }
    int64_t p = s1 * s1 + s2 * s2 - (((int64_t)c * s1) >> 14) * s2;
    return p < 0 ? 0 : (uint64_t)p;
}

int edgeai_features(const int16_t *x, unsigned n, edgeai_feat_t *f)
{
    if (n < 8 || n > EDGEAI_MAX_WINDOW)
        return -1;
    int32_t y[EDGEAI_MAX_WINDOW];
    int64_t sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum += x[i];
    f->mean = (int32_t)(sum / (int64_t)n);

    uint64_t sumsq = 0;
    uint32_t peak = 0, zc = 0;
    for (unsigned i = 0; i < n; i++) {
        int32_t v = (int32_t)x[i] - f->mean;
        y[i] = v;
        sumsq += (uint64_t)((int64_t)v * v);
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > peak)
            peak = a;
        if (i && ((y[i - 1] < 0) != (v < 0)))
            zc++;
    }
    uint64_t msq = sumsq / n;
    f->rms = edgeai_isqrt64(msq);
    f->peak = peak;
    f->crest_q8 = (peak << 8) / (f->rms ? f->rms : 1u);
    f->zc_q10 = (zc << 10) / (n - 1);

    /* |X|^2 of a sinusoid of amplitude A at a bin is (A*N/2)^2, so
     * 4*P/N^2 is an amplitude^2 that does not depend on N. */
    int32_t l2_nn = edgeai_log2_q8((uint64_t)n * n);
    for (int b = 0; b < EDGEAI_NBANDS; b++) {
        uint64_t p = 0;
        for (int k = 0; k < EDGEAI_BINS_PER_BAND; k++)
            p += edgeai_goertzel_pow(y, n, edgeai_bin_coeff[b][k]);
        f->band_pow[b] = p;
        f->vec[4 + b] = (edgeai_log2_q8(4 * p) - l2_nn) / 2;
    }
    f->vec[0] = edgeai_log2_q8(msq) / 2;
    f->vec[1] = edgeai_log2_q8(peak);
    f->vec[2] = (int32_t)f->crest_q8;
    f->vec[3] = (int32_t)f->zc_q10;
    return 0;
}

int edgeai_window_label(const uint8_t *labels, unsigned n)
{
    unsigned c[EDGEAI_NCLASSES] = { 0 };
    for (unsigned i = 0; i < n; i++)
        if (labels[i] < EDGEAI_NCLASSES)
            c[labels[i]]++;
    if (c[1] >= 4)
        return 1;                     /* shock */
    if (2 * c[2] >= n)
        return 2;                     /* vibration */
    if (c[3] >= 2)
        return 3;                     /* tap */
    return 0;                         /* quiet */
}
