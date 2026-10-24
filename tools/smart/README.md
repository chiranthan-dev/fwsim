# tools/smart: offline SMART failure-prediction pipeline

Trains a "will this drive fail within 30 days?" model on Backblaze Drive
Stats and exports it into fwsim's firmware (`src/smart_model.h`). The README
section "SMART failure prediction" explains the design and the results;
[docs/model-card.md](../../docs/model-card.md) documents the model.

| Step | Script | Input | Output |
|---|---|---|---|
| 1. download | `download.sh` | Backblaze B2 | `DATA/data_Q1_2026.zip`, `data_Q2_2026.zip` (1.3 + 2.1 GB) |
| 2. ingest | `ingest.py` | the zips, one daily CSV at a time | `DATA/daily/*.parquet` (181 days, ~0.6 GB) |
| 3. features | `features.py` | daily parquet (DuckDB, out of core) | `DATA/features.parquet` (62M drive-days) |
| 4. train + evaluate | `train.py` | features | `results/smart/` metrics, PR curves, `model_deploy.json`, `vectors.csv` |
| 5. export | `export.py` | `model_deploy.json` | `src/smart_model.h`, `tests/smart_vectors.h` |
| 6. C vs Python | `agree.py` | test-set features + `fwsim-smart score` | `results/smart/agreement.json` |
| 7. report | `report.py` | all of the above | `results/smart/results.md` |

`smartlib.py` holds everything the steps (and the C code) must agree on:
attribute list, feature transforms, label horizon, splits, baseline rule.

## Reproduce

Everything runs in WSL2 / Linux. Raw data is never committed (`.gitignore`).

```bash
python3 -m venv tools/smart/.venv
tools/smart/.venv/bin/pip install -r tools/smart/requirements.txt
```

```bash
tools/smart/download.sh tools/smart/data
```

```bash
make bench-smart
```

`make bench-smart` runs steps 2-7, rebuilds fwsim with the exported model,
runs `test_smart`, times inference and runs the deployment demo. It picks
up `tools/smart/.venv` automatically; otherwise pass `SMART_PY=/path/to/python`.
Keep the data on the Linux filesystem for speed, e.g.
`make bench-smart SMART_DATA=~/fwsim-smart-data` (the committed results were
produced that way). Peak use: ~5 GB RAM, ~20 GB temporary disk for DuckDB's
spill during step 3, ~10 minutes on a 12-thread laptop.

If Ubuntu's `python3 -m venv` fails because `ensurepip` is missing (no sudo
for `apt install python3-venv`): `python3 -m venv --without-pip VENV` and
then bootstrap pip with `https://bootstrap.pypa.io/get-pip.py`.

Tests (no data needed, they build tiny synthetic datasets):

```bash
tools/smart/.venv/bin/python -m pytest tools/smart/tests -q
```

## Data used

- Quarters: **Q1 2026 and Q2 2026** (2026-01-01 .. 2026-06-30), from
  `https://f001.backblazeb2.com/file/Backblaze-Hard-Drive-Data/data_<Q>_<YYYY>.zip`
  as listed on https://www.backblaze.com/cloud-storage/resources/hard-drive-test-data
- Population: data HDDs of 4 TB and up from Toshiba, Seagate, HGST and WDC.
  SSDs and boot drives are dropped in `ingest.py`.
