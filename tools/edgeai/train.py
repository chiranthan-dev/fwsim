#!/usr/bin/env python3
"""
train.py - train the vibration classifier on SYNTHETIC data from env_sim.

Features come from the C code itself: this script runs `edgeai_cli
dataset`, which pushes env_sim recordings through the accelerometer's
digitisation and src/edgeai/features.c, so Python and the drive see
bit-identical feature vectors.

Data leakage control - the split is by RECORDING SEED, never by window:
    train       seeds   1..120
    validation  seeds 121..160   (model / depth / window selection)
    test        seeds 161..200   (reported once, never used to choose)
Windows overlap heavily (hop 16 samples), so a random split of windows
would put near-copies of a test window into training and inflate every
metric; whole recordings on one side or the other prevents that.

Pre-registered selection rule (fixed before the first training run):
  * target: macro-F1 >= 0.85 on the validation seeds;
  * per window size, decision tree depth = the smallest depth meeting the
    target (else the best);
  * per window size, deploy the SMALLER model (bytes) of {tree, MLP} that
    meets the target (else the better one);
  * the firmware's default window = the smallest window whose deployed
    model meets the target (else the best).

Outputs:
  results/edgeai/float_models.json   float parameters for export.py
  results/edgeai/train_report.json   metrics (validation + test)
  build/edgeai/test_<N>.csv          test-split features (for C agreement)
  build/edgeai/pred_float_<N>.npz    Python float predictions on them

Usage:
  python3 tools/edgeai/train.py --cli ./build/lockfree/edgeai_cli
  (from Windows with a WSL build: --cli "wsl -d Ubuntu -e ./build/lockfree/edgeai_cli")
"""
import argparse
import io
import json
import os
import shlex
import subprocess
import sys
import time

import numpy as np
import sklearn
from sklearn.metrics import confusion_matrix, f1_score
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler
from sklearn.tree import DecisionTreeClassifier

CLASSES = ["quiet", "shock", "vibration", "tap"]
SPLITS = {"train": (1, 120), "val": (121, 160), "test": (161, 200)}
TARGET_F1 = 0.85
TREE_DEPTHS = [3, 4, 5, 6, 8, 10, 12]
HIDDEN = 16
NFEAT = 9
MLP_BYTES = 368            # sizeof(edgeai_mlp_t), as reported by `edgeai_cli cost`
NODE_BYTES = 12            # sizeof(edgeai_node_t)


def run_cli(cli, args):
    cmd = shlex.split(cli) + args
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    return out


def load_dataset(cli, window, secs):
    lo, hi = SPLITS["train"][0], SPLITS["test"][1]
    text = run_cli(cli, ["dataset", "--window", str(window), "--seeds", f"{lo}-{hi}",
                         "--secs", str(secs), "--mix", "train"])
    data = np.loadtxt(io.StringIO(text), delimiter=",", skiprows=1, dtype=np.int64)
    return data, text


def split(data, name):
    lo, hi = SPLITS[name]
    m = (data[:, 0] >= lo) & (data[:, 0] <= hi)
    return data[m, 3:3 + NFEAT], data[m, 2]


def metrics(y, p):
    cm = confusion_matrix(y, p, labels=range(len(CLASSES)))
    rec = (cm.diagonal() / np.maximum(cm.sum(axis=1), 1)).tolist()
    return {"macro_f1": float(f1_score(y, p, average="macro", labels=range(len(CLASSES)))),
            "accuracy": float((y == p).mean()), "recall": rec, "confusion": cm.tolist()}


def balance(X, y, seed=0):
    """Oversample every class to the size of the largest (MLPClassifier has
    no class_weight). Training split only."""
    rng = np.random.default_rng(seed)
    n = np.bincount(y, minlength=len(CLASSES)).max()
    idx = np.concatenate([rng.choice(np.flatnonzero(y == c), n, replace=True)
                          for c in range(len(CLASSES)) if (y == c).any()])
    rng.shuffle(idx)
    return X[idx], y[idx]


def tree_to_json(t):
    tr = t.tree_
    value = tr.value[:, 0, :]
    frac = value / np.maximum(value.sum(axis=1, keepdims=True), 1e-12)
    return {"feature": tr.feature.tolist(), "threshold": tr.threshold.tolist(),
            "left": tr.children_left.tolist(), "right": tr.children_right.tolist(),
            "cls": [int(t.classes_[k]) for k in frac.argmax(axis=1)],
            "purity": frac.max(axis=1).tolist(), "nodes": int(tr.node_count),
            "depth": int(t.get_depth())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default="./build/lockfree/edgeai_cli")
    ap.add_argument("--windows", default="64,128,256")
    ap.add_argument("--secs", type=int, default=30)
    ap.add_argument("--out", default="results/edgeai")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    os.makedirs("build/edgeai", exist_ok=True)

    report = {"target_macro_f1_val": TARGET_F1, "splits": SPLITS, "secs_per_recording": a.secs,
              "hop": 16, "classes": CLASSES, "sklearn": sklearn.__version__,
              "data": "synthetic (env_sim mix 'train'), see src/periph/env_sim.h", "windows": {}}
    models = {"classes": CLASSES, "windows": {}}

    for window in [int(w) for w in a.windows.split(",")]:
        t0 = time.time()
        data, text = load_dataset(a.cli, window, a.secs)
        Xtr, ytr = split(data, "train")
        Xva, yva = split(data, "val")
        Xte, yte = split(data, "test")
        # The test split's rows (all columns) for the C agreement check.
        lines = text.splitlines()
        lo, hi = SPLITS["test"]
        with open(f"build/edgeai/test_{window}.csv", "w", newline="\n") as f:
            f.write(lines[0] + "\n")
            for ln in lines[1:]:
                if lo <= int(ln.split(",", 1)[0]) <= hi:
                    f.write(ln + "\n")

        counts = {n: np.bincount(y, minlength=4).tolist()
                  for n, y in (("train", ytr), ("val", yva), ("test", yte))}
        print(f"window {window}: {len(ytr)} train / {len(yva)} val / {len(yte)} test windows; "
              f"class counts {counts}", flush=True)

        # --- decision tree: depth by the pre-registered rule
        trees = []
        for d in TREE_DEPTHS:
            t = DecisionTreeClassifier(max_depth=d, class_weight="balanced",
                                       min_samples_leaf=20, random_state=0).fit(Xtr, ytr)
            f1v = f1_score(yva, t.predict(Xva), average="macro")
            trees.append((d, t, f1v))
            print(f"  tree depth {d:2d}: {t.tree_.node_count:4d} nodes, val macro-F1 {f1v:.4f}")
        ok = [x for x in trees if x[2] >= TARGET_F1]
        d, tree, tree_f1v = ok[0] if ok else max(trees, key=lambda x: x[2])

        # --- MLP features -> 16 -> 4 on standardised features
        sc = StandardScaler().fit(Xtr)
        Xb, yb = balance(Xtr, ytr)
        mlp = MLPClassifier(hidden_layer_sizes=(HIDDEN,), activation="relu", solver="adam",
                            alpha=1e-3, batch_size=512, learning_rate_init=3e-3, max_iter=200,
                            early_stopping=True, n_iter_no_change=10, random_state=0)
        mlp.fit(sc.transform(Xb), yb)
        mlp_f1v = f1_score(yva, mlp.predict(sc.transform(Xva)), average="macro")
        print(f"  mlp 9-16-4: val macro-F1 {mlp_f1v:.4f} ({mlp.n_iter_} epochs)")

        tree_bytes = tree.tree_.node_count * NODE_BYTES
        cands = [("tree", tree_f1v, tree_bytes), ("mlp", mlp_f1v, MLP_BYTES)]
        meet = [c for c in cands if c[1] >= TARGET_F1]
        sel = min(meet, key=lambda c: c[2]) if meet else max(cands, key=lambda c: c[1])

        p_tree = tree.predict(Xte)
        p_mlp = mlp.predict(sc.transform(Xte))
        np.savez(f"build/edgeai/pred_float_{window}.npz", tree=p_tree, mlp=p_mlp, y=yte)
        report["windows"][str(window)] = {
            "class_counts": counts,
            "tree": {"depth": d, "nodes": int(tree.tree_.node_count), "bytes": tree_bytes,
                     "val_macro_f1": tree_f1v, "test": metrics(yte, p_tree),
                     "depth_sweep_val_f1": {str(x[0]): x[2] for x in trees}},
            "mlp": {"bytes": MLP_BYTES, "val_macro_f1": mlp_f1v, "epochs": int(mlp.n_iter_),
                    "test": metrics(yte, p_mlp)},
            "selected": sel[0], "selected_meets_target": bool(meet),
            "selected_val_macro_f1": sel[1], "seconds": round(time.time() - t0, 1),
        }
        # Activation scale calibration for export (train split, float path).
        Ztr = sc.transform(Xtr)
        hid = np.maximum(Ztr @ mlp.coefs_[0] + mlp.intercepts_[0], 0)
        models["windows"][str(window)] = {
            "selected": sel[0],
            "tree": tree_to_json(tree),
            "mlp": {"mean": sc.mean_.tolist(), "std": sc.scale_.tolist(),
                    "W1": mlp.coefs_[0].tolist(), "b1": mlp.intercepts_[0].tolist(),
                    "W2": mlp.coefs_[1].tolist(), "b2": mlp.intercepts_[1].tolist(),
                    "z_absmax_p999": float(np.percentile(np.abs(Ztr), 99.9)),
                    "h_max_p999": float(np.percentile(hid, 99.9))},
        }
        print(f"  -> deploy {sel[0]} (val macro-F1 {sel[1]:.4f}); test macro-F1 "
              f"tree {report['windows'][str(window)]['tree']['test']['macro_f1']:.4f} "
              f"mlp {report['windows'][str(window)]['mlp']['test']['macro_f1']:.4f}", flush=True)

    ws = sorted(report["windows"], key=int)
    meet = [w for w in ws if report["windows"][w]["selected_meets_target"]]
    default = meet[0] if meet else max(ws, key=lambda w: report["windows"][w]["selected_val_macro_f1"])
    report["default_window"] = int(default)
    models["default_window"] = int(default)
    with open(os.path.join(a.out, "float_models.json"), "w") as f:
        json.dump(models, f)
    with open(os.path.join(a.out, "train_report.json"), "w") as f:
        json.dump(report, f, indent=1)
    print(f"default window: {default}")


if __name__ == "__main__":
    sys.exit(main())
