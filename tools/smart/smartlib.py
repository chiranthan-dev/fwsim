"""smartlib.py - definitions shared by every step of the SMART pipeline.

Everything that must agree between training, evaluation, export and the
C firmware lives here: the attribute list, the feature transforms, the
label horizon, the splits and the baseline rule. The tests in
tools/smart/tests/ pin these down.
"""
from __future__ import annotations

import hashlib

import numpy as np
import pandas as pd

# ---- data ------------------------------------------------------------------

QUARTERS = ("Q1_2026", "Q2_2026")

# The counters the literature (and Backblaze's own blog) single out as
# failure-related, plus power-on hours and temperature.
COUNTERS = (5, 187, 188, 197, 198)   # reallocated, reported uncorrectable,
                                     # command timeout, pending, offline unc.
POH, TEMP = 9, 194
ATTRS = COUNTERS + (POH, TEMP)

SMART_COLS = [f"smart_{a}_{k}" for a in ATTRS for k in ("raw", "normalized")]
BASE_COLS = ["date", "serial_number", "model", "capacity_bytes", "failure"]

HORIZON_DAYS = 30        # label: fails within the next 30 days (today included)
HISTORY_DAYS = 30        # deltas look back this far; rows need this much history

# ---- manufacturers / drive population ----------------------------------------

_SSD_MARKERS = ("SSD", "CT250", "CT500", "MTFD", "DELLBOSS", "SAMSUNG", "MICRON",
                "SSDSC", "KINGSTON", "SEAGATE BARRACUDA 120", "SEAGATE FIRECUDA")


def manufacturer(model: str) -> str:
    """Map a Backblaze model string to a manufacturer ('' = not an HDD we use)."""
    m = model.upper().strip()
    if any(s in m for s in _SSD_MARKERS):
        return ""
    if m.startswith("TOSHIBA"):
        return "Toshiba"
    if m.startswith(("ST", "SEAGATE")):
        return "Seagate"
    if m.startswith(("HGST", "HITACHI")):
        return "HGST"
    if m.startswith(("WDC", "WD", "WUH")):
        return "WDC"
    return ""


MIN_CAPACITY = 4_000_000_000_000   # data drives only: boot drives are a
                                   # different population (and mostly SSDs)

# ---- feature transforms (mirrored bit-for-bit in src/smart.c) ---------------

LG_FRAC_BITS = 4
LG_CAP = (1 << 40) - 1
TEMP_MAX = 80


def lg_fx(x) -> np.ndarray:
    """Integer approximation of 16*log2(1+x), exact replica of smart_lg() in C.

    v = x+1, m = position of v's top bit, frac = the next 4 bits read as a
    linear interpolation: lg = 16*m + frac. Monotone, 0 for x = 0, and only
    shifts and adds, so a drive controller computes it without an FPU.
    Negative inputs (a counter that went down) count as 0.
    """
    v = np.clip(np.nan_to_num(np.asarray(x, dtype=np.float64), nan=0.0), 0, LG_CAP)
    v = v.astype(np.int64) + 1
    m = np.zeros_like(v)
    for k in range(1, 41):
        m += (v >> k) > 0
    frac = ((v - (np.int64(1) << m)) << LG_FRAC_BITS) >> m
    return (m << LG_FRAC_BITS) + frac


def temp_fx(t) -> np.ndarray:
    t = np.nan_to_num(np.asarray(t, dtype=np.float64), nan=0.0)
    return np.clip(t, 0, TEMP_MAX).astype(np.int64)


# Deployable features: computable by fwsim's firmware from its own counters.
# Order matters: it is the weight order in src/smart_model.h.
DEPLOY_FEATURES = (
    [f"lg_raw_{a}" for a in COUNTERS]
    + [f"lg_d7_{a}" for a in COUNTERS]
    + [f"lg_d30_{a}" for a in COUNTERS]
    + ["lg_poh", "temp"]
)
# Full features: + vendor-normalised values and "attribute reported" flags.
FULL_FEATURES = (
    DEPLOY_FEATURES
    + [f"norm_{a}" for a in ATTRS]
    + ["has_187", "has_188"]
)


def build_features(df: pd.DataFrame) -> pd.DataFrame:
    """Raw SMART columns + delta columns (d7_A, d30_A) -> model features."""
    out = {}
    for a in COUNTERS:
        out[f"lg_raw_{a}"] = lg_fx(df[f"smart_{a}_raw"])
        out[f"lg_d7_{a}"] = lg_fx(df[f"d7_{a}"])
        out[f"lg_d30_{a}"] = lg_fx(df[f"d30_{a}"])
    out["lg_poh"] = lg_fx(df[f"smart_{POH}_raw"])
    out["temp"] = temp_fx(df[f"smart_{TEMP}_raw"])
    for a in ATTRS:
        n = df[f"smart_{a}_normalized"].astype("float64")
        out[f"norm_{a}"] = n.fillna(100.0).to_numpy()
    out["has_187"] = df["smart_187_raw"].notna().to_numpy().astype(np.int64)
    out["has_188"] = df["smart_188_raw"].notna().to_numpy().astype(np.int64)
    return pd.DataFrame(out, index=df.index)


# ---- baseline (no ML) --------------------------------------------------------

def baseline_flag(df: pd.DataFrame) -> np.ndarray:
    """The classic rule: flag if any of SMART 5/187/188/197/198 raw > 0.

    A missing attribute (the vendor does not report it) is not a trip.
    """
    flag = np.zeros(len(df), dtype=bool)
    for a in COUNTERS:
        flag |= df[f"smart_{a}_raw"].fillna(0).to_numpy() > 0
    return flag


def baseline_score(df: pd.DataFrame) -> np.ndarray:
    """Graded version of the rule: sum of the five raw counters.

    score > 0 is exactly baseline_flag, so the rule is one point on this
    score's curve; the graded form lets us read the rule's recall at any
    false-alarm rate, the same way as the models'.
    """
    s = np.zeros(len(df), dtype=np.float64)
    for a in COUNTERS:
        s += np.clip(df[f"smart_{a}_raw"].fillna(0).to_numpy(dtype=np.float64), 0, 1e12)
    return s


# ---- splits ------------------------------------------------------------------

def serial_bucket(serial: str, seed: int) -> float:
    """Deterministic uniform [0,1) per serial (md5: stable across machines)."""
    h = hashlib.md5(f"{seed}:{serial}".encode()).hexdigest()
    return int(h[:12], 16) / float(1 << 48)


def split_serials(serials, test_frac: float = 0.2, seed: int = 0):
    """Split by DRIVE: every row of a serial lands on the same side."""
    serials = sorted(set(serials))
    test = {s for s in serials if serial_bucket(s, seed) < test_frac}
    train = set(serials) - test
    return train, test


def time_split_windows(first_day, last_day):
    """Train / test row windows for the time-based split.

    Rows need HISTORY_DAYS of history (for the 30-day deltas) and a
    HORIZON_DAYS future (to know the label). The training window ends
    HORIZON_DAYS before the test window starts, so no training label can
    look into the test period.
    """
    first_day, last_day = pd.Timestamp(first_day), pd.Timestamp(last_day)
    eligible_start = first_day + pd.Timedelta(days=HISTORY_DAYS)
    eligible_end = last_day - pd.Timedelta(days=HORIZON_DAYS)
    test_end = eligible_end
    test_start = test_end - pd.Timedelta(days=30)
    train_end = test_start - pd.Timedelta(days=HORIZON_DAYS + 1)
    return {
        "eligible": (eligible_start, eligible_end),
        "train": (eligible_start, train_end),
        "test": (test_start, test_end),
    }


# ---- drive-level evaluation ----------------------------------------------------

def recall_at_far(y: np.ndarray, score: np.ndarray, far: float = 0.01):
    """Highest recall with false-alarm rate (FPR over healthy drives) <= far.

    Returns (recall, achieved_far, threshold). Ties are handled as a real
    threshold would: all drives with score >= t are flagged.
    """
    y = np.asarray(y, dtype=bool)
    score = np.asarray(score, dtype=np.float64)
    npos, nneg = int(y.sum()), int((~y).sum())
    order = np.argsort(-score, kind="mergesort")
    s, yy = score[order], y[order]
    tp = np.cumsum(yy)
    fp = np.cumsum(~yy)
    # only cut between distinct scores
    last = np.r_[s[1:] != s[:-1], True]
    tp, fp, thr = tp[last], fp[last], s[last]
    ok = fp / max(nneg, 1) <= far
    if not ok.any():
        return 0.0, 0.0, float("inf")
    i = np.nonzero(ok)[0][-1]
    return tp[i] / max(npos, 1), fp[i] / max(nneg, 1), float(thr[i])


def point_metrics(y: np.ndarray, flag: np.ndarray):
    y, flag = np.asarray(y, bool), np.asarray(flag, bool)
    tp = int((y & flag).sum())
    fp = int((~y & flag).sum())
    npos, nneg = int(y.sum()), int((~y).sum())
    return {
        "recall": tp / max(npos, 1),
        "far": fp / max(nneg, 1),
        "precision": tp / max(tp + fp, 1),
        "tp": tp, "fp": fp, "pos": npos, "neg": nneg,
    }
