/*
 * features.h - fixed-point feature extraction over one accelerometer
 * window (N int16 samples in mg at EDGEAI_FS_HZ), for the vibration
 * classifier. Integer arithmetic only: no float, no malloc, so the same
 * code runs on a microcontroller without an FPU, and Python gets exactly
 * these features too (tools/edgeai/train.py calls them through the
 * edgeai_cli program instead of re-implementing them).
 *
 * The feature vector (EDGEAI_NFEAT int32 values):
 *   0  log2(RMS)      Q8   (RMS after removing the window mean)
 *   1  log2(peak)     Q8   (max |x - mean|)
 *   2  crest factor   Q8   (peak / RMS): impulsive events have a high crest
 *   3  zero crossings Q10  (sign changes per sample): a frequency proxy
 *   4..8 log2(band amplitude) Q8 for 5 bands: 20-60, 60-120, 120-200,
 *      200-300, 300-480 Hz, each the sum of 4 Goertzel bins
 * Logs compress a 1 mg .. 6 g dynamic range into a few thousand Q8
 * units, which keeps both the tree thresholds and the int8 MLP inputs
 * well-conditioned.
 *
 * WHY GOERTZEL AND NOT AN FFT: we need the energy at 20 specific
 * frequencies, not a full spectrum. Goertzel computes one DFT bin with a
 * 2-tap recursion - one multiply and two adds per sample, two words of
 * state - so the bins we want cost 20*N multiplies and 40 words of RAM,
 * with no N-point complex buffer, no twiddle table, no bit reversal, any
 * N (not just powers of two) and bin frequencies placed exactly where the
 * bands need them instead of on the k*fs/N grid. A radix-2 FFT of N=256
 * is a similar number of multiplies but needs 2 KB of complex int32
 * buffer and gives 128 bins we would mostly throw away. (With far more
 * bins than ~log2(N) per band the FFT would win.)
 */
#ifndef FWSIM_EDGEAI_FEATURES_H
#define FWSIM_EDGEAI_FEATURES_H

#include <stdint.h>

#define EDGEAI_FS_HZ      1000u
#define EDGEAI_NFEAT      9
#define EDGEAI_NBANDS     5
#define EDGEAI_BINS_PER_BAND 4
#define EDGEAI_MAX_WINDOW 256u
#define EDGEAI_NCLASSES   4      /* quiet, shock, vibration, tap (= env_class_t) */

typedef struct {
    int32_t  mean;                    /* mg                                 */
    uint32_t rms;                     /* mg (integer sqrt)                  */
    uint32_t peak;                    /* mg                                 */
    uint32_t crest_q8;                /* peak / rms, Q8                      */
    uint32_t zc_q10;                  /* zero crossings per sample, Q10      */
    uint64_t band_pow[EDGEAI_NBANDS]; /* sum of |X(k)|^2 over the band's bins */
    int32_t  vec[EDGEAI_NFEAT];       /* the model input, see above          */
} edgeai_feat_t;

/* Compute everything for x[0..n), 8 <= n <= EDGEAI_MAX_WINDOW. 0 / -1. */
int edgeai_features(const int16_t *x, unsigned n, edgeai_feat_t *f);

/* |X(f)|^2 of x[0..n) at frequency f given as a Q14 Goertzel coefficient
 * 2*cos(2*pi*f/fs). Exposed for tests. */
uint64_t edgeai_goertzel_pow(const int32_t *x, unsigned n, int32_t coeff_q14);
/* Coefficient table: bin b of band k is edgeai_bin_coeff[k][b]; the
 * frequency it was made for is edgeai_bin_hz_x10[k][b] / 10. */
extern const int32_t  edgeai_bin_coeff[EDGEAI_NBANDS][EDGEAI_BINS_PER_BAND];
extern const uint16_t edgeai_bin_hz_x10[EDGEAI_NBANDS][EDGEAI_BINS_PER_BAND];

uint32_t edgeai_isqrt64(uint64_t v);
/* log2(v) in Q8: integer part from the leading bit, the next 8 bits as a
 * linear fraction (max error ~0.086, the same in C and Python). 0 -> 0. */
int32_t  edgeai_log2_q8(uint64_t v);

/* Ground-truth label of a window from its per-sample labels (env_class_t),
 * by priority: SHOCK if >= 4 samples are shock-active; else VIBRATION if
 * at least half are; else TAP if >= 2 are tap-active; else QUIET. */
int edgeai_window_label(const uint8_t *labels, unsigned n);

#endif
