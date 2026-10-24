"""train.py - train the baseline rule, logistic regression and a small GBT;
evaluate them PER DRIVE on two leakage-safe splits.

    python tools/smart/train.py DATA_DIR OUT_DIR [--seed 0] [--neg-rate 0.02]

Splits (see smartlib.time_split_windows / split_serials):
  serial : 80/20 by drive serial over the whole eligible period;
  time   : train on Feb-Mar, test on the last 31 eligible days (May), with
           a 30-day gap so no training label sees the test period.

Unit of evaluation = one DRIVE over the test window: its score is the
highest daily score it got in the window, and it is positive if it failed
within the window or the 30 days after it. "False-alarm rate" = fraction
of healthy drives flagged at least once in the window.

Training rows: every positive drive-day plus a deterministic 2% sample of
negative drive-days, re-weighted so that both classes carry equal total
weight (the extreme imbalance is handled by weighting, not by accuracy).

Writes into OUT_DIR: metrics.csv, by_manufacturer.csv, summary.json,
pr_<split>.png, model_deploy.json, vectors.csv; and into DATA_DIR the
deployed model's test-set features (for agree.py).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
from sklearn.ensemble import HistGradientBoostingClassifier  # noqa: E402
from sklearn.linear_model import LogisticRegression  # noqa: E402
from sklearn.metrics import average_precision_score, precision_recall_curve  # noqa: E402
from sklearn.pipeline import make_pipeline  # noqa: E402
from sklearn.preprocessing import StandardScaler  # noqa: E402

import smartlib as sl  # noqa: E402
from features import connect  # noqa: E402

MODELS = ("baseline", "lr_deploy", "lr_full", "gbt")
LABELS = {
    "baseline": "Baseline rule (any of 5/187/188/197/198 raw > 0)",
    "lr_deploy": "Logistic regression, deployable features (exported to fwsim)",
    "lr_full": "Logistic regression, all features",
    "gbt": "Gradient-boosted trees (60 trees, depth 3), all features",
}
RAW_SELECT = ", ".join(
    ["serial_number"]
    + sl.SMART_COLS
    + [f"d7_{a}" for a in sl.COUNTERS]
    + [f"d30_{a}" for a in sl.COUNTERS])


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ---- models -------------------------------------------------------------------

def make_lr(seed: int):
    return make_pipeline(StandardScaler(),
                         LogisticRegression(C=1.0, max_iter=5000, random_state=seed))


def make_gbt(seed: int):
    return HistGradientBoostingClassifier(
        max_iter=60, max_depth=3, learning_rate=0.1, l2_regularization=1.0,
        early_stopping=False, random_state=seed)


def balanced_weights(y: np.ndarray, neg_rate: float) -> np.ndarray:
    """Undo the negative subsampling (x 1/neg_rate), then give both classes
    the same total weight. Mean weight 1 keeps the regulariser's scale."""
    y = np.asarray(y, bool)
    w = np.where(y, 1.0, 1.0 / neg_rate)
    w[y] *= w[~y].sum() / w[y].sum()
    return w * (len(w) / w.sum())


def fit_models(train: pd.DataFrame, neg_rate: float, seed: int):
    X = sl.build_features(train)
    y = train["y"].to_numpy().astype(bool)
    w = balanced_weights(y, neg_rate)
    models = {}
    for name, feats, mk in (("lr_deploy", sl.DEPLOY_FEATURES, make_lr),
                            ("lr_full", sl.FULL_FEATURES, make_lr),
                            ("gbt", sl.FULL_FEATURES, make_gbt)):
        m = mk(seed)
        kw = {"logisticregression__sample_weight": w} if mk is make_lr else {"sample_weight": w}
        m.fit(X[list(feats)].to_numpy(dtype=np.float64), y, **kw)
        models[name] = (m, feats)
    return models


def score_frame(models, df: pd.DataFrame) -> dict:
    X = sl.build_features(df)
    out = {"baseline": sl.baseline_score(df)}
    for name, (m, feats) in models.items():
        # decision_function = log-odds: rank-equivalent to the probability,
        # and it does not saturate to 1.0 for the riskiest drives.
        out[name] = m.decision_function(X[list(feats)].to_numpy(dtype=np.float64))
    out["_X"] = X
    return out


# ---- data access ---------------------------------------------------------------

def sample_train_rows(con, where: str, neg_rate: float, seed: int) -> pd.DataFrame:
    cut = int(round(neg_rate * 1_000_000))
    q = f"""
        SELECT {RAW_SELECT}, y FROM f
        WHERE {where}
          AND (y = 1 OR md5_number(serial_number || strftime(date, '%Y%m%d') || '{seed}')
                        % 1000000 < {cut})
        ORDER BY serial_number, date   -- fixed order: bit-reproducible fits"""
    return con.execute(q).df()


def drive_labels(con, where: str, a, b) -> pd.DataFrame:
    hi = (pd.Timestamp(b) + pd.Timedelta(days=sl.HORIZON_DAYS - 1)).date()
    q = f"""
        SELECT serial_number, any_value(model) AS model, any_value(mfr) AS mfr,
               max(CASE WHEN fail_date BETWEEN DATE '{a.date()}' AND DATE '{hi}'
                        THEN 1 ELSE 0 END)::TINYINT AS y,
               count(*) AS days
        FROM f WHERE {where} GROUP BY serial_number ORDER BY serial_number"""
    return con.execute(q).df().set_index("serial_number")


def drive_scores(con, where: str, models, dump=None) -> pd.DataFrame:
    """Max daily score per drive over the rows matching `where` (streamed)."""
    reader = con.sql(f"SELECT {RAW_SELECT}, y FROM f WHERE {where} ORDER BY serial_number, date").to_arrow_reader(1_000_000)
    parts = []
    for batch in reader:
        df = batch.to_pandas()
        s = score_frame(models, df)
        X = s.pop("_X")
        if dump is not None:
            dump.append((X[list(sl.DEPLOY_FEATURES)].to_numpy(dtype=np.int16),
                         s["lr_deploy"].astype(np.float64), df["y"].to_numpy(np.int8)))
        part = pd.DataFrame(s)
        part["serial_number"] = df["serial_number"].to_numpy()
        parts.append(part.groupby("serial_number").max())
    return pd.concat(parts).groupby(level=0).max()


# ---- metrics ------------------------------------------------------------------

def evaluate(y: np.ndarray, scores: pd.DataFrame, flag_rule: np.ndarray, far=0.01,
             n_boot=0, seed=0) -> dict:
    res = {}
    rule = sl.point_metrics(y, flag_rule)
    for name in MODELS:
        s = scores[name].to_numpy()
        r = {"pr_auc": average_precision_score(y, s) if y.any() else float("nan")}
        r["recall_at_far"], r["far_achieved"], r["threshold"] = sl.recall_at_far(y, s, far)
        r["recall_at_rule_far"] = sl.recall_at_far(y, s, rule["far"])[0]
        if n_boot and y.any():
            rng = np.random.default_rng(seed)
            ap, rc = [], []
            for _ in range(n_boot):
                i = rng.integers(0, len(y), len(y))
                if y[i].any():
                    ap.append(average_precision_score(y[i], s[i]))
                    rc.append(sl.recall_at_far(y[i], s[i], far)[0])
            r["pr_auc_ci"] = [float(np.percentile(ap, 2.5)), float(np.percentile(ap, 97.5))]
            r["recall_at_far_ci"] = [float(np.percentile(rc, 2.5)), float(np.percentile(rc, 97.5))]
        res[name] = r
    res["rule"] = rule
    return res


def plot_pr(path: str, title: str, y, scores: pd.DataFrame, rule: dict) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(7, 5), dpi=120)
    colors = {"baseline": "#888888", "lr_deploy": "#1f77b4", "lr_full": "#2ca02c", "gbt": "#d62728"}
    for name in MODELS:
        p, r, _ = precision_recall_curve(y, scores[name].to_numpy())
        ap = average_precision_score(y, scores[name].to_numpy())
        ls = "--" if name == "baseline" else "-"
        lab = "Baseline, graded (sum of raw counters)" if name == "baseline" else LABELS[name]
        ax.plot(r, p, ls, color=colors[name], lw=1.6, label=f"{lab}  PR-AUC {ap:.3f}")
    ax.plot([rule["recall"]], [rule["precision"]], "ko", ms=7,
            label=f"Baseline rule (raw > 0): recall {rule['recall']:.2f}, "
                  f"false alarms {100 * rule['far']:.1f}%")
    ax.set_xlabel("Recall (failed drives caught)")
    ax.set_ylabel("Precision (flagged drives that fail)")
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.grid(alpha=0.3)
    ax.set_title(title, fontsize=10)
    ax.legend(fontsize=7, loc="upper right")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


# ---- main ---------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--neg-rate", type=float, default=0.02)
    ap.add_argument("--boot", type=int, default=100)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    con = connect(args.data)
    con.execute(f"CREATE VIEW f AS SELECT * FROM read_parquet('{os.path.join(args.data, 'features.parquet')}')")
    d0, d1 = con.execute("SELECT min(date), max(date) FROM f").fetchone()
    win = sl.time_split_windows(d0, d1)
    ea, eb = win["eligible"]
    log(f"data {d0} .. {d1}; eligible rows {ea.date()} .. {eb.date()}")

    def between(a, b):
        return f"date BETWEEN DATE '{a.date()}' AND DATE '{b.date()}'"

    # serial split: assign every drive seen in the eligible window
    serials = con.execute(f"SELECT DISTINCT serial_number FROM f WHERE {between(ea, eb)}").df()
    tr, te = sl.split_serials(serials["serial_number"], 0.2, args.seed)
    con.register("split_df", pd.DataFrame({"serial_number": sorted(te)}))
    con.execute("CREATE TABLE test_serials AS SELECT * FROM split_df")
    in_test = "serial_number IN (SELECT serial_number FROM test_serials)"

    tra, trb = win["train"]
    tea, teb = win["test"]
    splits = {
        "serial": dict(train=f"{between(ea, eb)} AND NOT {in_test}",
                       test=f"{between(ea, eb)} AND {in_test}", test_window=(ea, eb),
                       desc=f"80/20 by serial, rows {ea.date()}..{eb.date()}"),
        "time": dict(train=between(tra, trb), test=between(tea, teb), test_window=(tea, teb),
                     calib=between(trb - pd.Timedelta(days=30), trb),
                     desc=f"train rows {tra.date()}..{trb.date()}, test rows {tea.date()}..{teb.date()}"),
    }

    summary = {"data": {"quarters": list(sl.QUARTERS), "first_day": str(d0), "last_day": str(d1),
                        "horizon_days": sl.HORIZON_DAYS, "seed": args.seed, "neg_rate": args.neg_rate},
               "splits": {}}
    metric_rows, mfr_rows = [], []
    for sname, sp in splits.items():
        log(f"[{sname}] sampling training rows")
        train = sample_train_rows(con, sp["train"], args.neg_rate, args.seed)
        npos = int(train["y"].sum())
        log(f"[{sname}] {len(train)} training rows ({npos} positive); fitting")
        models = fit_models(train, args.neg_rate, args.seed)
        pos_rows, all_rows = con.execute(
            f"SELECT sum(y), count(*) FROM f WHERE {sp['train']}").fetchone()
        prior = float(pos_rows) / float(all_rows)

        log(f"[{sname}] scoring test drives")
        dump = [] if sname == "time" else None
        a, b = sp["test_window"]
        lab = drive_labels(con, sp["test"], a, b)
        sc = drive_scores(con, sp["test"], models, dump).reindex(lab.index)
        y = lab["y"].to_numpy().astype(bool)
        rule_flag = sc["baseline"].to_numpy() > 0
        res = evaluate(y, sc, rule_flag, n_boot=args.boot, seed=args.seed)
        summary["splits"][sname] = {
            "desc": sp["desc"], "train_rows": len(train), "train_pos_rows": npos,
            "train_prior": prior, "test_drives": int(len(y)), "test_failed_drives": int(y.sum()),
            "metrics": res}
        plot_pr(os.path.join(args.out, f"pr_{sname}.png"),
                f"Per-drive precision-recall, {sname} split\n{sp['desc']}; "
                f"{len(y)} test drives, {int(y.sum())} failed within 30 days",
                y, sc, res["rule"])
        for name in MODELS:
            r = res[name]
            metric_rows.append(dict(split=sname, model=name, pr_auc=r["pr_auc"],
                                    recall_at_1pct_far=r["recall_at_far"],
                                    far_achieved=r["far_achieved"],
                                    recall_at_rule_far=r["recall_at_rule_far"],
                                    pr_auc_lo=r.get("pr_auc_ci", [np.nan])[0],
                                    pr_auc_hi=r.get("pr_auc_ci", [np.nan, np.nan])[1],
                                    recall_lo=r.get("recall_at_far_ci", [np.nan])[0],
                                    recall_hi=r.get("recall_at_far_ci", [np.nan, np.nan])[1]))
        rr = res["rule"]
        metric_rows.append(dict(split=sname, model="rule_point", pr_auc=np.nan,
                                recall_at_1pct_far=np.nan, far_achieved=rr["far"],
                                recall_at_rule_far=rr["recall"]))

        # per manufacturer (and Toshiba per model)
        groups = [("mfr", m, lab["mfr"] == m) for m in sorted(lab["mfr"].unique())]
        tosh = lab[lab["mfr"] == "Toshiba"]["model"].value_counts()
        groups += [("toshiba_model", m, lab["model"] == m) for m in tosh.index[:6]]
        for kind, g, mask in groups:
            mask = mask.to_numpy()
            yy = y[mask]
            gres = evaluate(yy, sc[mask], rule_flag[mask])
            row = dict(split=sname, kind=kind, group=g, drives=int(mask.sum()), failed=int(yy.sum()),
                       rule_recall=gres["rule"]["recall"], rule_far=gres["rule"]["far"])
            for name in MODELS:
                row[f"{name}_pr_auc"] = gres[name]["pr_auc"]
                row[f"{name}_recall_at_1pct"] = gres[name]["recall_at_far"]
                row[f"{name}_recall_at_rule_far"] = gres[name]["recall_at_rule_far"]
            mfr_rows.append(row)

        if sname == "time":
            # Deployment threshold: 1% drive-level false alarms over a
            # 31-day window of the TRAINING period (never the test set).
            log("[time] choosing the deployment threshold on the training period")
            ca = trb - pd.Timedelta(days=30)
            clab = drive_labels(con, sp["calib"], ca, trb)
            csc = drive_scores(con, sp["calib"], {"lr_deploy": models["lr_deploy"]}).reindex(clab.index)
            cy = clab["y"].to_numpy().astype(bool)
            _, cfar, thr = sl.recall_at_far(cy, csc["lr_deploy"].to_numpy(), 0.01)
            flag = sc["lr_deploy"].to_numpy() >= thr
            deployed = sl.point_metrics(y, flag)
            summary["splits"]["time"]["deployed_threshold"] = {
                "logit": thr, "calib_far": cfar, "test": deployed}
            log(f"[time] threshold {thr:.3f}: test recall {deployed['recall']:.3f}, "
                f"false alarms {deployed['far']:.4f}")
            save_deploy(args, models["lr_deploy"][0], prior, thr, summary, dump)
            gb = models["gbt"][0]
            nodes = sum(len(p[0].nodes) for p in gb._predictors)
            summary["gbt_nodes"] = int(nodes)

    pd.DataFrame(metric_rows).to_csv(os.path.join(args.out, "metrics.csv"), index=False)
    pd.DataFrame(mfr_rows).to_csv(os.path.join(args.out, "by_manufacturer.csv"), index=False)
    with open(os.path.join(args.out, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=2, default=float)
    log("done")
    return 0


def save_deploy(args, pipe, prior: float, thr: float, summary, dump) -> None:
    """Persist what export.py needs: float weights, the prior shift, the
    threshold, test vectors; and the full test-set features for agree.py."""
    scaler, lr = pipe.named_steps["standardscaler"], pipe.named_steps["logisticregression"]
    # Training used balanced class weights, i.e. a 50/50 prior; shifting the
    # intercept by the true log prior-odds turns the score back into an
    # (approximate) probability of failing within 30 days.
    shift = float(np.log(prior / (1 - prior)))
    model = {
        "features": list(sl.DEPLOY_FEATURES),
        "mean": scaler.mean_.tolist(), "scale": scaler.scale_.tolist(),
        "coef": lr.coef_[0].tolist(), "intercept": float(lr.intercept_[0]),
        "prior": prior, "prior_logit_shift": shift,
        "threshold_logit": thr,
        "trained_on": summary["splits"]["time"]["desc"],
        "quarters": list(sl.QUARTERS),
    }
    with open(os.path.join(args.out, "model_deploy.json"), "w") as fh:
        json.dump(model, fh, indent=2)

    X = np.concatenate([d[0] for d in dump])
    z = np.concatenate([d[1] for d in dump])
    yy = np.concatenate([d[2] for d in dump])
    X.tofile(os.path.join(args.data, "deploy_test_X.int16"))
    np.save(os.path.join(args.data, "deploy_test_logit.npy"), z)
    # 512 committed test vectors: positives and the highest-risk healthy rows
    # exercise the interesting (non-zero) part of the model, random rows the rest.
    rng = np.random.default_rng(args.seed)
    pos = np.nonzero(yy == 1)[0]
    hi = np.argsort(-z)[:2000]
    pick = np.unique(np.concatenate([
        rng.choice(pos, min(192, len(pos)), replace=False),
        rng.choice(hi, 128, replace=False),
        rng.choice(len(z), 192, replace=False)]))[:512]
    v = pd.DataFrame(X[pick], columns=sl.DEPLOY_FEATURES)
    v["float_logit"] = z[pick]
    v.to_csv(os.path.join(args.out, "vectors.csv"), index=False)


if __name__ == "__main__":
    sys.exit(main())
