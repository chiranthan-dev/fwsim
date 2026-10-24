"""ingest.py - Backblaze quarterly zips -> one small parquet file per day.

    python tools/smart/ingest.py DATA_DIR [QUARTER...]

Reads DATA_DIR/data_<quarter>.zip for each quarter in smartlib.QUARTERS,
streaming ONE daily CSV at a time out of the zip (never unpacked, never
the whole quarter in memory), keeps only the columns the model uses and
only data HDDs, and writes DATA_DIR/daily/YYYY-MM-DD.parquet. A day that
is already converted is skipped, so the step is restartable.
"""
from __future__ import annotations

import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pandas as pd  # noqa: E402

import smartlib as sl  # noqa: E402


def convert_day(z: zipfile.ZipFile, name: str, out_path: str) -> int:
    wanted = set(sl.BASE_COLS + sl.SMART_COLS)
    with z.open(name) as f:
        df = pd.read_csv(f, usecols=lambda c: c in wanted, low_memory=False)
    for c in sl.SMART_COLS:            # a day may lack a column entirely
        if c not in df.columns:
            df[c] = float("nan")
    df = df[sl.BASE_COLS + sl.SMART_COLS]
    df["model"] = df["model"].astype(str).str.strip()
    df["mfr"] = df["model"].map(sl.manufacturer)
    df = df[(df["mfr"] != "") & (df["capacity_bytes"] >= sl.MIN_CAPACITY)]
    df["date"] = pd.to_datetime(df["date"]).dt.date
    df["failure"] = df["failure"].astype("int8")
    for c in sl.SMART_COLS:
        df[c] = df[c].astype("float64")
    tmp = out_path + ".tmp"
    df.to_parquet(tmp, index=False)
    os.replace(tmp, out_path)          # atomic: a crash never leaves half a day
    return len(df)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    data = sys.argv[1]
    daily = os.path.join(data, "daily")
    os.makedirs(daily, exist_ok=True)
    total = 0
    for q in sys.argv[2:] or sl.QUARTERS:
        zpath = os.path.join(data, f"data_{q}.zip")
        with zipfile.ZipFile(zpath) as z:
            names = sorted(n for n in z.namelist()
                           if n.endswith(".csv") and not os.path.basename(n).startswith("."))
            for n in names:
                day = os.path.basename(n)[:-4]
                out = os.path.join(daily, f"{day}.parquet")
                if os.path.exists(out):
                    continue
                rows = convert_day(z, n, out)
                total += rows
                print(f"{q} {day}: {rows} rows", flush=True)
    print(f"ingest: {total} new rows, {len(os.listdir(daily))} days in {daily}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
