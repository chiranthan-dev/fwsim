# Model card: fwsim vibration classifier

A four-class classifier that runs inside the simulated drive firmware on
windows of accelerometer samples and tells the write path what kind of
mechanical disturbance is happening, so it can pick a response.

| | |
|---|---|
| Classes | `quiet`, `shock`, `vibration` (sustained rotational vibration), `tap` (harmless periodic tapping) |
| Input | 9 integer features of the last N = 64 samples at 1 kHz (64 ms), computed every 16 samples |
| Deployed model | int8 MLP, 9 → 16 (ReLU) → 4, 368 bytes of parameters (`src/edgeai/model_data.h`) |
| Also exported | decision trees (N = 64/128/256) and MLPs for N = 128/256; the trees are deployed for 128/256 |
| Inference | integer only: int8 weights, int32 accumulators, no malloc, no floating point (`make nofloat` checks it) |
| Training code | `tools/edgeai/train.py` (scikit-learn 1.9.1), export `tools/edgeai/export.py` |
| Data | **synthetic only** (`src/periph/env_sim.h`); no real accelerometer data was used anywhere |

## Intended use and non-use

Intended for the simulator: to compare write-protection policies (none /
fixed threshold / classifier / hybrid) under a documented, frozen physical
model, and as a worked example of an Edge-AI pipeline on firmware (fixed-point
features, int8 export, C/Python agreement checks).

Not suitable for a real drive as is. The data, the sensor (an invented
single-axis part) and every physical constant are illustrative. A real
deployment would need recorded accelerometer data from real chassis, the
real servo's error-sensitivity curve, and re-validation on that hardware.

## Data generation

`env_sim` generates the in-plane acceleration at 4 kHz as a sequence of
labelled segments with randomised, deliberately overlapping parameters:

| class | what is generated | overlaps with |
|---|---|---|
| quiet | fan hum 0-8 mg at 40-120 Hz + 1 mg noise floor | weak vibration |
| shock | 1-3 damped sinusoids per segment, 400-6000 mg (log-uniform), 60-320 Hz, tau 3-25 ms, + a 2.3x mode | hard taps (280-320 Hz, 400-1500 mg, tau 3-4 ms) |
| vibration | 2-4 drifting sinusoids at 30-300 Hz + band noise, total RMS 15-200 mg, 60 ms fades | quiet (low RMS), shock (overlaid shocks) |
| tap | impulses every 150-600 ms, 200-1500 mg, 280-480 Hz, tau 0.5-4 ms | small shocks |

The accelerometer model averages 4 physics samples per output sample, adds
2 mg RMS Gaussian sensor noise and rounds to int16 (1 mg/LSB). Training data
goes through exactly that path (`accel_model_digitize`), and through the C
feature code, via `edgeai_cli dataset`.

**Labels.** Per sample: `shock` while a shock's envelope is above
min(10% of peak, 100 mg); else `vibration` inside a vibration segment; else
`tap` while a tap is ringing; else `quiet`. Per window, by priority: shock if
≥ 4 shock samples, else vibration if ≥ half, else tap if ≥ 2 tap samples,
else quiet. A window that still contains the tail of a shock is "shock".

**Splits (by recording seed, never by window).** 200 recordings of 30 s from
the `train` mix (25% of segments of each class): seeds 1-120 train, 121-160
validation, 161-200 test. Windows overlap (hop 16), so splitting windows at
random would leak near-copies of test windows into training; whole
recordings on one side of the split prevent that. The system benchmark uses
yet other seeds (5001-5030) and four other mixes.

Test-split windows (N = 64): 44,055 quiet, 2,043 shock, 24,969 vibration,
3,813 tap. Shock and tap are rare by nature, so the tree uses
`class_weight="balanced"` and the MLP is trained on a class-balanced
oversample of the training split.

## Features (`src/edgeai/features.c`, all fixed point)

After removing the window mean: log2 RMS (Q8), log2 peak (Q8), crest factor
peak/RMS (Q8), zero-crossing rate (Q10), and log2 band amplitude (Q8) in five
bands (20-60, 60-120, 120-200, 200-300, 300-480 Hz), each the sum of four
Goertzel bins. Goertzel instead of an FFT because only 20 specific
frequencies are needed: one multiply-accumulate per sample per bin, two
words of state per bin, any window length, bins placed exactly on the band
centres, and no N-point complex buffer or twiddle table.

## Model selection (rule fixed before the first training run)

Target: validation macro-F1 ≥ 0.85. For each window size, the tree depth
is the smallest in {3, 4, 5, 6, 8, 10, 12} that meets the target. Then the
smaller of {tree, MLP} (parameter bytes) that meets it is deployed. The
firmware default is the smallest window whose deployed model meets it.

| window | tree (depth / nodes / bytes) | tree test macro-F1 | MLP test macro-F1 | deployed |
|---|---|---|---|---|
| 64 | 4 / 31 / 372 | 0.876 | **0.952** | MLP (368 B < 372 B) |
| 128 | 3 / 15 / 180 | 0.917 | 0.966 | tree |
| 256 | 3 / 15 / 180 | 0.929 | 0.969 | tree |

Per-class test recall of the deployed N = 64 MLP: quiet 0.984, shock 0.961,
vibration 0.986, tap 0.990. Benchmark tables with confusion matrices for
all three window sizes: `results/edgeai/results.md`.

Note: `train.py` first assumed 304 bytes for the MLP; the real
`sizeof(edgeai_mlp_t)` is 368 (`edgeai_cli cost`). The choice for N = 64
(368 < 372) is the same either way. The committed `train_report.json`
still records the 304 from the training run.

## Quantisation

Symmetric int8 with one scale per layer (s_w = max|W|/127); the input
standardisation and input scale fold into one integer multiplier per
feature; biases are int32 at the accumulator scale; the hidden layer is
requantised with a 24-bit fixed-point multiplier; activation scales are the
99.9th percentile on the training split. Confidence is the softmax of the
top class, computed with a 17-entry 2^-x table.

| window | int8 vs float agreement (test) | float accuracy | int8 accuracy |
|---|---|---|---|
| 64 | 99.86% | 98.42% | 98.37% |
| 128 | 99.92% | 98.29% | 98.28% |
| 256 | 99.90% | 97.63% | 97.62% |

C inference was checked against Python on the whole test split: C int8 equals
export.py's bit-exact numpy simulation on 100% of windows, and the C
integer tree equals sklearn's tree on 100% (sklearn's float thresholds lie
between integer feature values, so `floor()` is exact).
`tests/test_edgeai.c` re-checks this on 1,000 golden windows per size.

## Known failure modes

- **Too slow for shocks by construction.** A decision every 16 ms over a 64 ms
  window can only react after a shock has started ringing; the threshold
  interrupt reacts at the next sample (1 ms). The shock damage happens in the
  first ~10-30 ms. This is why policy P3 keeps the threshold for the fast path.
- **Shock "afterglow".** A window keeps containing a shock for up to 64 ms after
  it ended, so P2 keeps writes inhibited after the danger has passed (counted
  as needless stall in the benchmark).
- **Tap vs small shock.** Classes overlap at 280-320 Hz, 400-1500 mg; a hard tap
  can be called a shock (needless inhibit) and a soft high-frequency shock a
  tap (no protection; in this physics, such shocks mostly do little harm).
- **Vibration is a situation, not a harm level.** Low-frequency (< 60 Hz) or weak
  vibration is classified as vibration although the servo rejects it; the
  policy then pays for verify-after-write it did not need.
- **Distribution.** Trained on one synthetic mix; the benchmark mixes only
  re-weight the same generators. Real chassis vibration (multi-axis, drive
  resonances, non-stationary fans) would need new data.
