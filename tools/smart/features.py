"""features.py - daily parquet -> one feature/label table (DuckDB, out of core).

    python tools/smart/features.py DATA_DIR

Writes DATA_DIR/features.parquet with, per drive-day:
  - the raw/normalised SMART columns,
  - d7_A / d30_A: how much counter A grew over the last 7 / 30 days
    (today minus the EARLIEST snapshot inside [day-7, day]; a drive with a
    shorter history uses what it has, exactly like the firmware's ring),
  - fail_date (NULL if the drive never failed in the data) and
  - y = 1 if the drive fails within HORIZON_DAYS (today included).

DuckDB streams the parquet files and spills to disk, so the ~60M rows of
two quarters never have to fit in RAM.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import duckdb  # noqa: E402

import smartlib as sl  # noqa: E402


def feature_sql(src: str, dst: str) -> str:
    deltas = ",\n  ".join(
        f"smart_{a}_raw - first_value(smart_{a}_raw IGNORE NULLS) OVER w7  AS d7_{a},\n  "
        f"smart_{a}_raw - first_value(smart_{a}_raw IGNORE NULLS) OVER w30 AS d30_{a}"
        for a in sl.COUNTERS)
    smart = ", ".join(sl.SMART_COLS)
    h = sl.HORIZON_DAYS
    return f"""
COPY (
WITH d AS (
  SELECT * FROM read_parquet('{src}')
),
fails AS (
  SELECT serial_number, min(date) AS fail_date
  FROM d WHERE failure = 1 GROUP BY serial_number
),
f AS (
  SELECT date, serial_number, model, mfr, failure, {smart},
  {deltas}
  FROM d
  WINDOW w7  AS (PARTITION BY serial_number ORDER BY date
                 RANGE BETWEEN INTERVAL 7 DAYS PRECEDING AND CURRENT ROW),
         w30 AS (PARTITION BY serial_number ORDER BY date
                 RANGE BETWEEN INTERVAL 30 DAYS PRECEDING AND CURRENT ROW)
)
SELECT f.*, fails.fail_date,
       (fails.fail_date IS NOT NULL
        AND fails.fail_date >= f.date
        AND fails.fail_date <= f.date + INTERVAL {h - 1} DAYS)::TINYINT AS y
FROM f LEFT JOIN fails USING (serial_number)
-- a few drives keep reporting after their failure record: drop those days
WHERE fails.fail_date IS NULL OR f.date <= fails.fail_date
ORDER BY f.date, f.serial_number
) TO '{dst}' (FORMAT PARQUET, ROW_GROUP_SIZE 500000)
"""


def connect(data: str) -> duckdb.DuckDBPyConnection:
    con = duckdb.connect()
    tmp = os.path.join(data, "duckdb_tmp")
    os.makedirs(tmp, exist_ok=True)
    con.execute(f"SET temp_directory = '{tmp}'")
    con.execute("SET memory_limit = '4GB'")
    con.execute("SET preserve_insertion_order = false")
    return con


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    data = sys.argv[1]
    src = os.path.join(data, "daily", "*.parquet")
    dst = os.path.join(data, "features.parquet")
    con = connect(data)
    con.execute(feature_sql(src, dst))
    n, ndrives, npos, nfail, d0, d1 = con.execute(
        f"SELECT count(*), count(DISTINCT serial_number), sum(y),"
        f" count(DISTINCT CASE WHEN failure = 1 THEN serial_number END),"
        f" min(date), max(date) FROM read_parquet('{dst}')").fetchone()
    print(f"features: {n} rows, {ndrives} drives, {nfail} failed drives, "
          f"{npos} positive rows, {d0} .. {d1} -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
