#!/usr/bin/env python3
"""
export.py - quantise the trained classifier and generate
src/edgeai/model_data.h (int8 MLP + integer decision tree per window size).

Quantisation (see src/edgeai/infer.h for the C side):
  * input: the training-set standardisation and the int8 input scale
    s_in = (99.9th percentile of |z| on train) / 127 fold into one integer
    multiplier per feature:  q = clamp(round((x - mean) * 2^24 / (std * s_in)))
  * weights: symmetric int8 with ONE scale per layer: s_w = max|W| / 127
  * biases: int32 at the accumulator's scale (s_in * s_w1, s_h * s_w2)
  * hidden activations: s_h = (99.9th percentile of ReLU outputs on train)
    / 127; the layer-1 requantisation is acc * m1 >> 24 with
    m1 = round(s_in * s_w1 / s_h * 2^24)
  * tree: sklearn's float thresholds sit between integer feature values,
    so floor(threshold) gives the identical integer test x <= thr

This script also re-implements the C integer pipeline in numpy (bit-exact:
same rounding, same arithmetic shifts) to measure the quantisation error
before any C runs, and writes golden test vectors for tests/test_edgeai.c.

  python3 tools/edgeai/export.py                 # write model_data.h + golden files
  python3 tools/edgeai/export.py --check-c --cli CMD
        # after rebuilding edgeai_cli: C int8 vs Python float / int8-sim
        # agreement on the test seeds -> results/edgeai/agreement.json
"""
import argparse
import json
import math
import os
import shlex
import subprocess
import sys
import zlib

import numpy as np

IN_SHIFT = 24
REQ_SHIFT = 24
NFEAT = 9
HIDDEN = 16
NCLS = 4
GOLDEN_PER_CLASS = 250


def q_round(v):
    return np.floor(np.asarray(v, dtype=np.float64) + 0.5).astype(np.int64)


def quantise_mlp(p):
    mean, std = np.array(p["mean"]), np.array(p["std"])
    W1, b1 = np.array(p["W1"]), np.array(p["b1"])      # W1: (NFEAT, HIDDEN)
    W2, b2 = np.array(p["W2"]), np.array(p["b2"])      # W2: (HIDDEN, NCLS)
    s_in = p["z_absmax_p999"] / 127.0
    s_w1 = np.abs(W1).max() / 127.0
    s_h = max(p["h_max_p999"], 1e-6) / 127.0
    s_w2 = np.abs(W2).max() / 127.0
    q = {
        "in_zp": q_round(mean),
        "in_mult": q_round(2.0 ** IN_SHIFT / (std * s_in)),
        "w1": np.clip(q_round(W1.T / s_w1), -127, 127),
        "b1": q_round(b1 / (s_in * s_w1)),
        "m1": int(q_round(s_in * s_w1 / s_h * 2.0 ** REQ_SHIFT)),
        "w2": np.clip(q_round(W2.T / s_w2), -127, 127),
        "b2": q_round(b2 / (s_h * s_w2)),
        "logit_k_q16": int(q_round(s_h * s_w2 * math.log2(math.e) * 65536.0)),
        "scales": {"s_in": s_in, "s_w1": s_w1, "s_h": s_h, "s_w2": s_w2},
    }
    for k in ("b1", "b2"):
        assert np.abs(q[k]).max() < 2 ** 31, k
    return q


def int8_predict(q, X):
    """Bit-exact numpy version of edgeai_infer_mlp (class only)."""
    X = np.asarray(X, dtype=np.int64)
    half_in, half_req = 1 << (IN_SHIFT - 1), 1 << (REQ_SHIFT - 1)
    qi = np.clip(((X - q["in_zp"]) * q["in_mult"] + half_in) >> IN_SHIFT, -127, 127)
    acc1 = q["b1"] + qi @ q["w1"].T
    h = np.where(acc1 > 0, np.clip((acc1 * q["m1"] + half_req) >> REQ_SHIFT, -127, 127), 0)
    logits = q["b2"] + h @ q["w2"].T
    return logits.argmax(axis=1)


def float_mlp_predict(p, X):
    z = (np.asarray(X, float) - np.array(p["mean"])) / np.array(p["std"])
    h = np.maximum(z @ np.array(p["W1"]) + np.array(p["b1"]), 0)
    return (h @ np.array(p["W2"]) + np.array(p["b2"])).argmax(axis=1)


def tree_nodes(t):
    nodes = []
    for i in range(t["nodes"]):
        leaf = t["left"][i] < 0
        nodes.append({
            "feat": -1 if leaf else t["feature"][i],
            "thr": 0 if leaf else int(math.floor(t["threshold"][i])),
            "left": 0 if leaf else t["left"][i], "right": 0 if leaf else t["right"][i],
            "cls": t["cls"][i] if leaf else 0,
            "conf": int(round(100 * t["purity"][i])) if leaf else 0,
        })
    return nodes


def tree_predict(nodes, X):
    out = np.empty(len(X), dtype=np.int64)
    for r, x in enumerate(np.asarray(X, dtype=np.int64)):
        i = 0
        while nodes[i]["feat"] >= 0:
            i = nodes[i]["left"] if x[nodes[i]["feat"]] <= nodes[i]["thr"] else nodes[i]["right"]
        out[r] = nodes[i]["cls"]
    return out


def c_array(vals, per_line=16, indent="        "):
    """A 2-D array as C rows, one braced row per line (-Wmissing-braces)."""
    rows = np.asarray(vals).reshape(-1, per_line)
    return ",\n".join(indent + "{ " + ", ".join(str(int(v)) for v in r) + " }" for r in rows)


def emit_header(models, quant, trees, default_window, path):
    body = []
    ws = sorted(models["windows"], key=int)
    for w in ws:
        q = quant[w]
        body.append(f"""static const edgeai_mlp_t mlp_{w} = {{
    {{ {", ".join(str(v) for v in q["in_zp"])} }},
    {{ {", ".join(str(v) for v in q["in_mult"])} }},
    {{
{c_array(q["w1"], NFEAT)}
    }},
    {{ {", ".join(str(v) for v in q["b1"])} }},
    {q["m1"]},
    {{
{c_array(q["w2"], HIDDEN)}
    }},
    {{ {", ".join(str(v) for v in q["b2"])} }},
    {q["logit_k_q16"]},
}};
""")
        rows = ",\n".join(
            f"    {{ {n['feat']}, {n['cls']}, {n['conf']}, 0, {n['thr']}, {n['left']}, {n['right']} }}"
            for n in trees[w])
        body.append(f"static const edgeai_node_t tree_{w}[{len(trees[w])}] = {{\n{rows},\n}};\n")
    table = ",\n".join(
        f"    {{ {w}, {'EDGEAI_MODEL_TREE' if models['windows'][w]['selected'] == 'tree' else 'EDGEAI_MODEL_MLP'}, "
        f"&mlp_{w}, {{ tree_{w}, {len(trees[w])} }} }}" for w in ws)
    text = "".join(body) + f"static const edgeai_model_t edgeai_models[EDGEAI_NMODELS] = {{\n{table},\n}};\n"
    model_id = zlib.crc32(text.encode()) & 0xFFFFFFFF
    hdr = f"""/*
 * model_data.h - GENERATED by tools/edgeai/export.py. Do not edit.
 *
 * Vibration classifier trained on SYNTHETIC data (env_sim), one model pair
 * per window size; `selected` is what the firmware deploys, chosen by the
 * rule in tools/edgeai/train.py. Included only by infer.c.
 */
#ifndef FWSIM_EDGEAI_MODEL_DATA_H
#define FWSIM_EDGEAI_MODEL_DATA_H

#define EDGEAI_MODEL_ID       0x{model_id:08X}u
#define EDGEAI_DEFAULT_WINDOW {default_window}u
#define EDGEAI_NMODELS        {len(ws)}u

{text}
#endif
"""
    with open(path, "w", newline="\n") as f:
        f.write(hdr)
    return model_id


def load_test(window):
    d = np.loadtxt(f"build/edgeai/test_{window}.csv", delimiter=",", skiprows=1, dtype=np.int64)
    return d[:, 3:3 + NFEAT], d[:, 2], d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="results/edgeai/float_models.json")
    ap.add_argument("--header", default="src/edgeai/model_data.h")
    ap.add_argument("--check-c", action="store_true")
    ap.add_argument("--cli", default="./build/lockfree/edgeai_cli")
    a = ap.parse_args()
    models = json.load(open(a.models))
    ws = sorted(models["windows"], key=int)
    quant = {w: quantise_mlp(models["windows"][w]["mlp"]) for w in ws}
    trees = {w: tree_nodes(models["windows"][w]["tree"]) for w in ws}

    if not a.check_c:
        mid = emit_header(models, quant, trees, models["default_window"], a.header)
        print(f"wrote {a.header} (model id 0x{mid:08X})")
        os.makedirs("tests/data", exist_ok=True)
        qrep = {}
        for w in ws:
            X, y, d = load_test(w)
            p = models["windows"][w]["mlp"]
            pf = float_mlp_predict(p, X)
            pq = int8_predict(quant[w], X)
            pt = tree_predict(trees[w], X)
            ref = np.load(f"build/edgeai/pred_float_{w}.npz")
            assert (pf == ref["mlp"]).all(), "float re-implementation != sklearn"
            assert (pt == ref["tree"]).all(), "integer tree != sklearn tree"
            qrep[w] = {"int8_sim_vs_float_agreement": float((pf == pq).mean()),
                       "int8_sim_accuracy": float((pq == y).mean()),
                       "float_accuracy": float((pf == y).mean()),
                       "scales": quant[w]["scales"]}
            print(f"window {w}: int8 (numpy sim) agrees with float on {100 * qrep[w]['int8_sim_vs_float_agreement']:.2f}% "
                  f"of {len(y)} test windows; integer tree == sklearn tree on 100%")
            # Golden vectors: up to GOLDEN_PER_CLASS test windows per class.
            rng = np.random.default_rng(int(w))
            pick = np.concatenate([rng.permutation(np.flatnonzero(y == c))[:GOLDEN_PER_CLASS]
                                   for c in range(NCLS)])
            with open(f"tests/data/edgeai_golden_{w}.csv", "w", newline="\n") as f:
                f.write("label,float_mlp,int8_mlp,tree," + ",".join(f"f{j}" for j in range(NFEAT)) + "\n")
                for i in pick:
                    f.write(f"{y[i]},{pf[i]},{pq[i]},{pt[i]}," + ",".join(str(v) for v in X[i]) + "\n")
        json.dump(qrep, open("results/edgeai/quantization.json", "w"), indent=1)
        return 0

    # --- C agreement on the full test split
    agree = {}
    for w in ws:
        X, y, _ = load_test(w)
        ref = np.load(f"build/edgeai/pred_float_{w}.npz")
        out = {}
        for kind in ("mlp", "tree"):
            cmd = shlex.split(a.cli) + ["predict", "--window", w, "--model", kind,
                                        "--in", f"build/edgeai/test_{w}.csv"]
            txt = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
            pc = np.array([int(l.split(",")[0]) for l in txt.splitlines()[1:]])
            out[kind] = pc
        pq = int8_predict(quant[w], X)
        agree[w] = {
            "test_windows": int(len(y)),
            "c_int8_mlp_vs_python_float_mlp": float((out["mlp"] == ref["mlp"]).mean()),
            "c_int8_mlp_vs_python_int8_sim": float((out["mlp"] == pq).mean()),
            "c_tree_vs_sklearn_tree": float((out["tree"] == ref["tree"]).mean()),
            "c_int8_mlp_accuracy": float((out["mlp"] == y).mean()),
            "python_float_mlp_accuracy": float((ref["mlp"] == y).mean()),
        }
        print(f"window {w}: C int8 MLP vs Python float {100 * agree[w]['c_int8_mlp_vs_python_float_mlp']:.2f}%, "
              f"vs numpy int8 sim {100 * agree[w]['c_int8_mlp_vs_python_int8_sim']:.2f}%, "
              f"C tree vs sklearn {100 * agree[w]['c_tree_vs_sklearn_tree']:.2f}% ({len(y)} windows)")
    json.dump(agree, open("results/edgeai/agreement.json", "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
