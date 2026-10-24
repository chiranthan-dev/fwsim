"""Tests for the SMART pipeline: leakage-safe splits, the label horizon,
the baseline rule, deterministic training and the integer transforms
shared with the C firmware.

    python -m pytest tools/smart/tests -q
"""
import os
import sys

import numpy as np
import pandas as pd
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import export  # noqa: E402
import smartlib as sl  # noqa: E402
import train  # noqa: E402


# ---- leakage -------------------------------------------------------------------

def _rows(n_drives=400, days=20, seed=1):
    rng = np.random.default_rng(seed)
    serials = [f"SN{i:05d}" for i in range(n_drives)]
    df = pd.DataFrame({
        "serial_number": np.repeat(serials, days),
        "day": np.tile(np.arange(days), n_drives),
    })
    df["x"] = rng.normal(size=len(df))
    return df


def test_serial_split_has_no_shared_drive():
    df = _rows()
    train_s, test_s = sl.split_serials(df["serial_number"], 0.2, seed=0)
    assert not (train_s & test_s), "a serial is in both splits"
    assert train_s | test_s == set(df["serial_number"])
    tr = df[df["serial_number"].isin(train_s)]
    te = df[df["serial_number"].isin(test_s)]
    assert not set(tr["serial_number"]) & set(te["serial_number"])
    assert len(tr) + len(te) == len(df)           # every row on exactly one side
    assert 0.12 < len(test_s) / 400 < 0.28        # roughly the requested fraction


def test_serial_split_is_deterministic_and_seeded():
    s = [f"Z{i}" for i in range(1000)]
    assert sl.split_serials(s, 0.2, 0) == sl.split_serials(list(reversed(s)), 0.2, 0)
    assert sl.split_serials(s, 0.2, 0)[1] != sl.split_serials(s, 0.2, 1)[1]


def test_time_split_labels_never_reach_the_test_window():
    w = sl.time_split_windows("2026-01-01", "2026-06-30")
    tr_a, tr_b = w["train"]
    te_a, te_b = w["test"]
    # rows need 30 days of history and a fully observed 30-day future
    assert w["eligible"][0] == pd.Timestamp("2026-01-31")
    assert te_b + pd.Timedelta(days=sl.HORIZON_DAYS - 1) <= pd.Timestamp("2026-06-30")
    # the last training label looks HORIZON-1 days ahead: still before the test window
    assert tr_b + pd.Timedelta(days=sl.HORIZON_DAYS - 1) < te_a
    assert tr_a >= w["eligible"][0] and tr_b < te_a


# ---- label horizon and deltas (the DuckDB SQL) ---------------------------------

@pytest.fixture()
def tiny_dataset(tmp_path):
    days = pd.date_range("2026-01-01", periods=60, freq="D")
    rows = []
    for sn, fail_day in (("A", 45), ("B", None)):
        for i, d in enumerate(days):
            if fail_day is not None and i > fail_day:
                break
            r = {c: np.nan for c in sl.SMART_COLS}
            r.update(date=d.date(), serial_number=sn, model="TOSHIBA MG08ACA16TA",
                     capacity_bytes=16e12, failure=int(i == fail_day), mfr="Toshiba")
            r["smart_5_raw"] = float(i)            # grows by 1 per day
            r["smart_197_raw"] = float(max(0, 10 - i))   # shrinks
            rows.append(r)
    df = pd.DataFrame(rows)
    daily = tmp_path / "daily"
    daily.mkdir()
    for d, g in df.groupby("date"):
        g.to_parquet(daily / f"{d}.parquet", index=False)
    import features
    con = features.connect(str(tmp_path))
    con.execute(features.feature_sql(str(daily / "*.parquet"), str(tmp_path / "f.parquet")))
    return con.execute(f"SELECT * FROM read_parquet('{tmp_path / 'f.parquet'}') "
                       "ORDER BY serial_number, date").df()


def test_label_is_fail_within_30_days(tiny_dataset):
    a = tiny_dataset[tiny_dataset["serial_number"] == "A"].reset_index(drop=True)
    # drive A fails on day 45: days 16..45 are positive (30 days, today included)
    assert a.loc[15, "y"] == 0
    assert a.loc[16, "y"] == 1
    assert a.loc[45, "y"] == 1
    assert len(a) == 46                            # nothing after the failure
    b = tiny_dataset[tiny_dataset["serial_number"] == "B"]
    assert b["y"].sum() == 0


def test_deltas_match_firmware_definition(tiny_dataset):
    a = tiny_dataset[tiny_dataset["serial_number"] == "A"].reset_index(drop=True)
    assert a.loc[40, "d7_5"] == 7                  # today - (today-7)
    assert a.loc[40, "d30_5"] == 30
    assert a.loc[3, "d7_5"] == 3                   # young drive: since first sample
    assert a.loc[5, "d7_197"] == -5                # went down: the transform clips it
    assert sl.lg_fx(a.loc[[5], "d7_197"])[0] == 0


# ---- baseline rule -------------------------------------------------------------

def test_baseline_rule():
    df = pd.DataFrame({f"smart_{a}_raw": [0.0, 0.0, 0.0, 0.0, 0.0, np.nan]
                       for a in sl.COUNTERS})
    df.loc[1, "smart_5_raw"] = 1
    df.loc[2, "smart_187_raw"] = 3
    df.loc[3, "smart_198_raw"] = 8
    df.loc[4, "smart_9_raw"] = 9           # not one of the rule's attributes
    df["smart_9_raw"] = df["smart_9_raw"].fillna(0)
    flag = sl.baseline_flag(df)
    assert flag.tolist() == [False, True, True, True, False, False]   # NaN = not reported
    assert ((sl.baseline_score(df) > 0) == flag).all()


def test_recall_at_far():
    y = np.array([1, 1, 0, 0, 0, 0, 0, 0, 0, 0], bool)
    s = np.array([9, 1, 8, 2, 2, 0, 0, 0, 0, 0], float)
    # 8 healthy drives: one false alarm = 12.5%
    assert sl.recall_at_far(y, s, 0.0) == (0.5, 0.0, 9.0)
    assert sl.recall_at_far(y, s, 0.125) == (0.5, 0.125, 8.0)
    # the two healthy drives tied at score 2 can only be flagged together
    assert sl.recall_at_far(y, s, 0.25) == (0.5, 0.125, 8.0)
    assert sl.recall_at_far(y, s, 0.375) == (1.0, 0.375, 1.0)


# ---- deterministic training ----------------------------------------------------

def _train_frame(seed=0, n=3000):
    rng = np.random.default_rng(seed)
    df = pd.DataFrame({c: np.zeros(n) for c in sl.SMART_COLS})
    y = rng.random(n) < 0.05
    for a in sl.COUNTERS:
        df[f"smart_{a}_raw"] = np.where(y, rng.poisson(20, n), rng.poisson(0.05, n)).astype(float)
        df[f"d7_{a}"] = np.where(y, rng.poisson(5, n), 0).astype(float)
        df[f"d30_{a}"] = df[f"d7_{a}"] * 2
        df[f"smart_{a}_normalized"] = 100.0
    df["smart_9_raw"] = rng.integers(0, 50000, n).astype(float)
    df["smart_194_raw"] = rng.integers(20, 50, n).astype(float)
    df["y"] = y.astype(int)
    return df


def test_training_is_deterministic():
    df = _train_frame()
    m1 = train.fit_models(df, 0.02, seed=0)
    m2 = train.fit_models(df, 0.02, seed=0)
    X = sl.build_features(df)
    for name in ("lr_deploy", "lr_full", "gbt"):
        a, feats = m1[name]
        b, _ = m2[name]
        Xa = X[list(feats)].to_numpy(float)
        assert np.array_equal(a.decision_function(Xa), b.decision_function(Xa)), name


def test_balanced_weights():
    y = np.array([1] * 10 + [0] * 990, bool)
    w = train.balanced_weights(y, 0.02)
    assert np.isclose(w[y].sum(), w[~y].sum())
    assert np.isclose(w.mean(), 1.0)


# ---- integer transforms and export ---------------------------------------------

def test_lg_known_values():
    # The same table is checked in tests/test_smart.c (smart_lg).
    x = np.array([0, 1, 2, 3, 7, 1000, 2 ** 60])
    assert sl.lg_fx(x).tolist() == [0, 16, 24, 32, 48, 159, sl.lg_fx([sl.LG_CAP])[0]]
    v = sl.lg_fx(np.arange(200000))
    assert np.all(np.diff(v) >= 0)
    assert np.all(np.abs(v[1:] - 16 * np.log2(1 + np.arange(1, 200000))) <= 16)


def test_export_quantisation_is_close():
    rng = np.random.default_rng(3)
    n = len(sl.DEPLOY_FEATURES)
    model = dict(features=list(sl.DEPLOY_FEATURES), mean=rng.uniform(0, 50, n).tolist(),
                 scale=rng.uniform(5, 60, n).tolist(), coef=rng.normal(0, 1, n).tolist(),
                 intercept=-2.0, prior_logit_shift=-7.0, threshold_logit=1.0)
    qm = export.quantize(model, bits=8)
    X = rng.integers(0, 300, (5000, n))
    zf = ((X - model["mean"]) / model["scale"]) @ np.array(model["coef"]) + model["intercept"] - 7.0
    zi = export.int_logit_q8(X, qm) / 256.0
    # int8 weights: error bounded by half a step per weight times the
    # distance of each input from its zero point (+ the final floor)
    bound = 0.5 * qm["scale"] * np.abs(X - qm["zp"]).sum(axis=1) + 1 / 256 + 1e-9
    assert np.all(np.abs(zi - zf) <= bound)
    r = export.risk_bp(np.arange(-6000, 6000), qm)
    assert np.all(np.diff(r) >= 0) and r[0] <= 1 and r[-1] == 10000
