# Model card: fwsim SMART failure predictor

A logistic regression that estimates, from a drive's own SMART counters,
the probability that the drive fails within the next 30 days. It is trained
offline in Python on Backblaze Drive Stats and deployed into fwsim's
simulated firmware as integer-only C (`src/smart_model.h`), where
`FW_OP_SMART_READ` reports it.

All numbers below come from [results/smart/results.md](../results/smart/results.md)
(`make bench-smart`). This is a student project on public data, not a
validated product.

## Intended use

- **Use:** a fleet-maintenance signal. Flag a drive for proactive
  replacement or data migration before it fails.
- **Not for:** warranty or RMA decisions, any per-drive guarantee, or drives
  unlike the training population (SSDs, consumer drives, other fleets).

## Data

| | |
|---|---|
| Source | Backblaze Drive Stats, public daily SMART snapshots per drive with a failure flag |
| Quarters | Q1 2026 + Q2 2026 (2026-01-01 .. 2026-06-30), 181 daily files |
| Population | data HDDs of 4 TB and up from Toshiba, Seagate, HGST and WDC. SSDs and boot drives are removed in `ingest.py` |
| Size | 61,856,799 drive-days, 359,507 drives, 2,552 failed drives |
| Processing | each daily CSV is streamed out of the zip and reduced to the needed columns (parquet); DuckDB builds features out of core (spills to disk, ~5 GB RAM) |

Download steps: [tools/smart/README.md](../tools/smart/README.md). Raw data
is not committed.

## Label

`y = 1` on a drive-day if the drive's failure record falls within the next
30 days, today included (`fail_date - date` in 0..29). Rows after a failure
are dropped.

A row is only used if its label is fully observed (date ≤ last day − 30) and
it has 30 days of history before it (date ≥ first day + 30). That leaves the
eligible window **2026-01-31 .. 2026-05-31**.

## Features

All features are integers computed from raw counters with an integer
transform. The firmware computes exactly the same values (`smart_features()`
in `src/smart.c`).

| Feature | Definition |
|---|---|
| `lg_raw_A`, A ∈ {5, 187, 188, 197, 198} | `lg(x)` = integer approximation of 16·log2(1+x): top-bit position ×16 + the next 4 bits. Monotone, shift-and-add only |
| `lg_d7_A`, `lg_d30_A` | `lg` of the counter's growth over the last 7 / 30 days: today minus the oldest snapshot within [today−N, today]. Negative growth (e.g. pending sectors reallocated) counts as 0 |
| `lg_poh` | `lg(SMART 9 raw)`, power-on hours |
| `temp` | SMART 194 raw, clipped to 0..80 °C |

That is 17 **deployable** features. The "all features" comparison models
also get the vendor-normalised values of the seven attributes and two
"attribute reported" flags (187, 188). Those are vendor-specific scalings,
so they are left out of the model that ships in firmware.

Attribute meanings: 5 reallocated sectors, 187 reported uncorrectable
errors, 188 command timeouts, 197 current pending sectors, 198 offline
uncorrectable, 9 power-on hours, 194 temperature.

## Training

- Every positive drive-day plus a deterministic 2% sample of negative
  drive-days (md5 of serial+date+seed).
- Sample weights: negatives ×50 to undo the sampling, then both classes
  rescaled to equal total weight. This is how the extreme imbalance (0.10%
  positive drive-days) is handled.
- `StandardScaler` + `LogisticRegression(C=1)`, seed 0. Deterministic: a
  re-run gives identical coefficients (tested).
- The intercept is shifted by log(π/(1−π)), where π is the true positive
  rate of the training rows. Balanced weights imply a 50/50 prior, and this
  shift turns the output back into an approximate probability.
- Comparisons trained on the same rows: the same LR with all features, and
  a gradient-boosted tree model (`HistGradientBoostingClassifier`, 60 trees
  of depth 3, 896 nodes).

## Splits (leakage rules, tested in `tools/smart/tests/`)

- **Serial split:** 80/20 by drive serial (md5 hash). No drive appears on
  both sides. Rows from the whole eligible window.
- **Time split:** train on rows 2026-01-31 .. 03-31, test on 2026-05-01 .. 05-31.
  The last training label looks 29 days ahead (to 04-29), so no training
  label sees the test window. This split is closer to real deployment
  (train on the past, run on the future).
- **The deployed threshold** is chosen for 1% drive-level false alarms on
  the last 31 days of the *training* window. It is never tuned on the test
  set.

## Metrics (per drive, never accuracy)

Each test drive's score is its highest daily score in the test window. It
is positive if it failed during the window or within 30 days after it.
False-alarm rate = healthy drives flagged / healthy drives. Accuracy would
be 99.8% for "never fails" on the time split, so it is not reported.

| Time split (350,500 drives, 843 failed) | PR-AUC | Recall at 1% false alarms |
|---|---:|---:|
| Baseline rule, graded (sum of raw 5/187/188/197/198) | 0.033 | 12.2% |
| **LR, deployable (this model)** | **0.189** (0.164–0.218) | **52.9%** (50–56%) |
| LR, all features | 0.196 | 52.6% |
| GBT, all features | 0.208 | 60.1% |

- **Baseline rule "any raw > 0"** (a single operating point): recall 76.5%
  at **8.5%** false alarms. At that same false-alarm rate this model reaches
  77.8%, so it **barely beats the rule there**. Its value is at low
  false-alarm rates.
- **Deployed threshold on the test window:** recall 53.9% at 1.08% false
  alarms, precision 10.7%.
- **Serial split** (71,034 drives, 474 failed): PR-AUC 0.358 vs 0.103 for
  the graded baseline; recall at 1% false alarms 61.4% vs 11.0%. PR-AUC
  depends on the positive rate (0.67% here vs 0.24% on the time split), so
  compare recall at fixed false alarms across splits, not PR-AUC.

## Per manufacturer (time split; recall at each group's own 1% false-alarm threshold)

| | Drives | Failed | Graded rule | This model | GBT |
|---|---:|---:|---:|---:|---:|
| Seagate | 111,538 | 316 | 19.9% | 64.6% | 65.5% |
| Toshiba | 124,581 | 253 | 42.3% | 51.8% | 58.1% |
| WDC | 88,836 | 134 | 50.7% | 54.5% | 52.2% |
| HGST | 25,545 | 140 | 37.1% | 40.0% | 35.0% |

The model helps most on Seagate drives, whose 187/188 counters the simple
rule handles badly (the rule flags 15.3% of healthy Seagates). On HGST and
WDC it is roughly level with the graded baseline: on the serial split it is
*below* it (HGST 35.0% vs 39.0%, WDC 56.9% vs 58.5%). Toshiba MG07ACA14TA
(106 failures) improves from 63.2% to 73.6%. MG10ACA20TE (39 failures) does
not improve (48.7% vs 51.3%). The full table is in `results/smart/results.md`.

## Quantization (Python float → firmware integer)

- Scaler folded into the weights. Weights quantised to **int16** with one
  scale. Integer zero points equal to the training means. Logit in 1/256
  units via a 64-bit multiply and shift. Probability from a 257-entry
  sigmoid LUT with linear interpolation. No FPU, no malloc.
- On all 10,779,155 drive-days of the time-split test window:
  - the C result equals the Python integer reference in 100% of rows;
  - the flag agrees with the float scikit-learn model in 100% of rows
    (0 differ);
  - max |logit error| 0.0042, max |probability error| 0.0011.
- **int8 weights were tried first.** They gave up to 0.215 logit and 3.8
  percentage points of probability error, with 99.996% flag agreement, for
  failing drives whose large counters sit where the sigmoid is steep. int16
  costs 17 more bytes.
- Size: 602 bytes of tables (34 B weights, 34 B zero points, 514 B LUT,
  20 B constants), plus 1,608 bytes of per-drive state (32-day history
  ring). Host CPU (x86-64 under WSL2, *not* a drive controller): 10-18 ns
  per logit, 150-275 ns including feature computation (varies between runs).

## Known failure modes and caveats

- **Silent failures.** On the time split, 23.5% of the drives that failed
  never showed a non-zero 5/187/188/197/198 in the window. No counter-based
  model can catch those (only temperature and age remain), which caps recall.
- **One fleet, one half-year.** Backblaze's workload, enclosures, cooling and
  replacement policy are baked in. The time split already shows drift:
  recall at 1% false alarms falls from 61% (serial split) to 53% (time
  split, two months later).
- **Coefficients are not explanations.** The counter features are strongly
  correlated (raw vs 7-day vs 30-day growth), so some weights come out
  negative (e.g. `lg_raw_187`, `lg_d7_5`). Temperature and power-on hours
  also get negative weights, probably because of fleet composition (which
  models sit where, survivor effects). Read the model as a whole, not
  weight by weight.
- **Calibration is approximate.** The prior shift is a global correction;
  per-manufacturer base rates differ. Use the risk score for ranking and
  thresholding, and the probability only as a rough guide.
- **Snapshot gaps.** Backblaze occasionally misses a day for a drive. The
  SQL deltas then use the earliest snapshot inside the window, while the
  firmware fills idle days with the previous values. Both agree whenever
  there is a snapshot every day.
- **In fwsim:**
  - 188 (command timeouts) and 198 (offline uncorrectable) are always 0,
    because the simulator has no timeouts and no offline scan;
  - temperature is an input, not a thermal model;
  - the counters in the deployment demo are scripted.
  The demo shows the plumbing, not accuracy.
