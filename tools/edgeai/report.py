#!/usr/bin/env python3
"""
report.py - turn `make bench-edgeai` raw output into results/edgeai/results.md
and charts. Needs numpy + matplotlib only (no scikit-learn).

Inputs (all produced by real runs):
  results/edgeai/system_<mix>.csv      one row per (mix, variant, seed)
  results/edgeai/classifier_<N>.json   edgeai_cli eval on held-out test seeds
  results/edgeai/classifier_mix_<m>.json  deployed model on each bench mix
  results/edgeai/cost_<N>.json         edgeai_cli cost (host CPU timing)
  results/edgeai/train_report.json, agreement.json, quantization.json

Statistics: mean and 95% CI (Student t) over seeds. Every variant runs on
the SAME environment seeds and the SAME host write sequence, so policy
comparisons are reported as PAIRED per-seed differences with their own 95%
CI - a difference is called significant when that CI excludes 0.
"""
import argparse
import csv
import json
import math
import os
import sys

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

R = "results/edgeai"
MIXES = ["shock", "vibration", "mixed", "quiet"]
VARIANTS = ["P0_none", "P1_thr150", "P1_thr250", "P1_thr500", "P1_thr1000", "P2_ai", "P3_hybrid"]
LABEL = {"P0_none": "P0 none", "P1_thr150": "P1 150 mg", "P1_thr250": "P1 250 mg",
         "P1_thr500": "P1 500 mg", "P1_thr1000": "P1 1000 mg", "P2_ai": "P2 AI",
         "P3_hybrid": "P3 hybrid"}
CLASSES = ["quiet", "shock", "vibration", "tap"]
COL = {"P0_none": "#9ca3af", "P1_thr150": "#bfdbfe", "P1_thr250": "#60a5fa",
       "P1_thr500": "#2563eb", "P1_thr1000": "#1e3a8a", "P2_ai": "#f59e0b", "P3_hybrid": "#10b981"}


def t975(df):
    """Student t 0.975 quantile (Cornish-Fisher expansion; 2.045 at df=29)."""
    if df <= 0:
        return float("nan")
    z = 1.959963985
    return (z + (z ** 3 + z) / (4 * df) + (5 * z ** 5 + 16 * z ** 3 + 3 * z) / (96 * df ** 2)
            + (3 * z ** 7 + 19 * z ** 5 + 17 * z ** 3 - 15 * z) / (384 * df ** 3))


def mci(x):
    x = np.asarray(x, float)
    if len(x) < 2:
        return float(x.mean()) if len(x) else float("nan"), float("nan")
    return float(x.mean()), float(t975(len(x) - 1) * x.std(ddof=1) / math.sqrt(len(x)))


def fmt(m, c, d=2):
    return f"{m:.{d}f} ± {c:.{d}f}"


def load_system():
    rows = []
    for m in MIXES:
        p = os.path.join(R, f"system_{m}.csv")
        if os.path.exists(p):
            with open(p) as f:
                rows += list(csv.DictReader(f))
    data = {}
    for r in rows:
        data.setdefault((r["mix"], r["variant"]), {})[int(r["seed"])] = r
    return data


def col(data, mix, var, key):
    d = data.get((mix, var), {})
    return np.array([float(d[s][key]) for s in sorted(d)])


def paired(data, mix, a, b, key):
    da, db = data.get((mix, a), {}), data.get((mix, b), {})
    seeds = sorted(set(da) & set(db))
    diff = np.array([float(da[s][key]) - float(db[s][key]) for s in seeds])
    return mci(diff)


METRICS = [("corrupt_per_1k_writes", "corrupted sectors / 1k writes", 2),
           ("write_iops", "write IOPS", 1), ("p99_ms", "p99 write latency (ms)", 1),
           ("false_stall_ms_per_s", "needless stall (ms/s)", 1),
           ("stall_ms_per_s", "total stall (ms/s)", 1)]


def system_tables(data, seeds_expected):
    out = []
    for mix in MIXES:
        if not any((mix, v) in data for v in VARIANTS):
            continue
        n = len(data.get((mix, "P0_none"), {}))
        out.append(f"\n#### {mix} mix ({n} seeds, mean ± 95% CI)\n")
        out.append("| policy | " + " | ".join(m[1] for m in METRICS) + " | verified writes | silent |")
        out.append("|---|" + "---|" * (len(METRICS) + 2))
        for v in VARIANTS:
            if (mix, v) not in data:
                continue
            cells = [fmt(*mci(col(data, mix, v, k)), d) for k, _, d in METRICS]
            ver = col(data, mix, v, "verified_writes") / col(data, mix, v, "writes")
            sil = int(col(data, mix, v, "silent").sum())
            out.append(f"| {LABEL[v]} | " + " | ".join(cells) + f" | {100 * ver.mean():.0f}% | {sil} |")
            if len(data[(mix, v)]) != seeds_expected:
                out.append(f"\n**warning: {len(data[(mix, v)])} seeds for {v}, expected {seeds_expected}**\n")
    return out


def best_p1(data, mix):
    """The P1 threshold with the fewest corrupted sectors on this mix."""
    c = [(mci(col(data, mix, v, "corrupt_per_1k_writes"))[0], v) for v in VARIANTS
         if v.startswith("P1") and (mix, v) in data]
    return min(c)[1] if c else None


def headline(data):
    out = ["\n| mix | comparison | Δ corrupted / 1k writes | Δ write IOPS | Δ p99 (ms) | Δ needless stall (ms/s) |",
           "|---|---|---|---|---|---|"]
    verdicts = []
    for mix in MIXES:
        if (mix, "P2_ai") not in data:
            continue
        b = best_p1(data, mix)
        for ai in ("P2_ai", "P3_hybrid"):
            for ref in sorted({"P1_thr250", b}):
                cells = []
                sig = {}
                for k in ("corrupt_per_1k_writes", "write_iops", "p99_ms", "false_stall_ms_per_s"):
                    m, c = paired(data, mix, ai, ref, k)
                    s = "" if abs(m) <= c else (" ▼" if m < 0 else " ▲")
                    sig[k] = 0 if abs(m) <= c else (-1 if m < 0 else 1)
                    cells.append(f"{m:+.2f} ± {c:.2f}{s}")
                tag = f"{LABEL[ai]} − {LABEL[ref]}" + (" (best P1)" if ref == b else "")
                out.append(f"| {mix} | {tag} | " + " | ".join(cells) + " |")
                if ref == b:
                    verdicts.append((mix, ai, ref, sig))
    out.append("\n▼ / ▲: the paired 95% CI excludes 0 (significantly lower / higher). "
               "Lower is better for corruption, p99 and stall; higher is better for IOPS.")
    return out, verdicts


def chart(data, key, ylabel, fname, log=False):
    fig, ax = plt.subplots(figsize=(10, 4.2))
    w = 0.12
    for i, v in enumerate(VARIANTS):
        ms, cs, xs = [], [], []
        for j, mix in enumerate(MIXES):
            if (mix, v) not in data:
                continue
            m, c = mci(col(data, mix, v, key))
            ms.append(m); cs.append(c); xs.append(j + (i - 3) * w)
        ax.bar(xs, ms, w, yerr=cs, capsize=2, color=COL[v], label=LABEL[v], edgecolor="none")
    ax.set_xticks(range(len(MIXES)))
    ax.set_xticklabels([f"{m} mix" for m in MIXES])
    ax.set_ylabel(ylabel)
    if log:
        ax.set_yscale("symlog", linthresh=0.1)
    ax.grid(axis="y", alpha=0.3)
    ax.spines[["top", "right"]].set_visible(False)
    ax.legend(ncol=7, fontsize=8, frameon=False, loc="upper center", bbox_to_anchor=(0.5, 1.13))
    fig.tight_layout()
    fig.savefig(os.path.join(R, fname), dpi=130)
    plt.close(fig)


def classifier_section():
    out = []
    evals = {}
    for n in (64, 128, 256):
        p = os.path.join(R, f"classifier_{n}.json")
        if os.path.exists(p):
            evals[n] = json.load(open(p))
    costs = {n: json.load(open(os.path.join(R, f"cost_{n}.json"))) for n in evals
             if os.path.exists(os.path.join(R, f"cost_{n}.json"))}
    if not evals:
        return out, evals
    out.append("\n| window | latency (window) | deployed | macro-F1 | recall quiet / shock / vibration / tap "
               "| model bytes | features / window | inference / window |")
    out.append("|---|---|---|---|---|---|---|---|")
    for n, e in evals.items():
        c = costs.get(n, {})
        b = c.get("tree_bytes") if e["model"] == "tree" else c.get("mlp_bytes")
        inf = c.get("tree_ns") if e["model"] == "tree" else c.get("mlp_ns")
        out.append(f"| {n} | {n} ms | {e['model']} | {e['macro_f1']:.3f} | "
                   + " / ".join(f"{r:.3f}" for r in e["recall"])
                   + f" | {b} | {c.get('features_ns', 0) / 1000:.2f} µs | {inf / 1000 if inf else 0:.2f} µs |")
    out.append("\nTimes are wall-clock on the HOST CPU (WSL2 x86-64) averaged over all windows of a "
               "20 s recording; they are not MCU timings. On a microcontroller the cost is dominated by "
               f"the Goertzel loop: 20 bins x N multiply-accumulates "
               f"({', '.join(str(c.get('goertzel_mults')) for c in costs.values())} for N = 64/128/256).")
    for n, e in evals.items():
        out.append(f"\n**Window {n}, {e['model']}** - confusion matrix on held-out test seeds "
                   f"{e['seeds']} ({e['windows']} windows; rows = truth, columns = prediction):\n")
        out.append("| truth \\ pred | " + " | ".join(CLASSES) + " | recall |")
        out.append("|---|---|---|---|---|---|")
        for i, row in enumerate(e["confusion"]):
            out.append(f"| {CLASSES[i]} | " + " | ".join(str(x) for x in row) + f" | {e['recall'][i]:.3f} |")
    return out, evals


def detection_section(evals):
    out = ["\n| window | shock events | detected by model | model latency p50 / p90 | detected by 250 mg threshold "
           "| threshold latency p50 / p90 | taps tripping threshold | taps called shock | "
           "vibration segments tripping threshold |",
           "|---|---|---|---|---|---|---|---|---|"]
    for n, e in evals.items():
        out.append(f"| {n} | {e['shock_events']} | {e['shock_detected_ai']} | "
                   f"{e['shock_latency_ai_ms_p50']:.0f} / {e['shock_latency_ai_ms_p90']:.0f} ms | "
                   f"{e['shock_detected_thr']} | {e['shock_latency_thr_ms_p50']:.0f} / "
                   f"{e['shock_latency_thr_ms_p90']:.0f} ms | "
                   f"{e['tap_tripped_threshold']} / {e['tap_events']} | {e['tap_classified_shock']} / {e['tap_events']} | "
                   f"{e['vibration_segments_tripping_threshold']} / {e['vibration_segments']} |")
    return out


def mix_section():
    out = ["\n| bench mix (seeds 5001-5030) | windows | macro-F1 | recall quiet / shock / vibration / tap |",
           "|---|---|---|---|"]
    for m in MIXES:
        p = os.path.join(R, f"classifier_mix_{m}.json")
        if os.path.exists(p):
            e = json.load(open(p))
            out.append(f"| {m} | {e['windows']} | {e['macro_f1']:.3f} | "
                       + " / ".join(f"{r:.3f}" for r in e["recall"]) + " |")
    return out


def confusion_chart(evals):
    fig, axs = plt.subplots(1, len(evals), figsize=(4 * len(evals), 3.6))
    axs = np.atleast_1d(axs)
    for ax, (n, e) in zip(axs, evals.items()):
        cm = np.array(e["confusion"], float)
        cmn = cm / np.maximum(cm.sum(axis=1, keepdims=True), 1)
        ax.imshow(cmn, cmap="Blues", vmin=0, vmax=1)
        for i in range(4):
            for j in range(4):
                ax.text(j, i, f"{cmn[i, j]:.2f}", ha="center", va="center",
                        color="white" if cmn[i, j] > 0.6 else "black", fontsize=9)
        ax.set_xticks(range(4)); ax.set_xticklabels(CLASSES, rotation=30, fontsize=8)
        ax.set_yticks(range(4)); ax.set_yticklabels(CLASSES, fontsize=8)
        ax.set_title(f"window {n} ({e['model']}), macro-F1 {e['macro_f1']:.3f}", fontsize=9)
        ax.set_xlabel("predicted", fontsize=8)
    axs[0].set_ylabel("truth", fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(R, "classifier_confusion.png"), dpi=130)
    plt.close(fig)


def model_section():
    out = []
    tr = json.load(open(os.path.join(R, "train_report.json")))
    ag = json.load(open(os.path.join(R, "agreement.json")))
    out.append("\n| window | tree depth / nodes | tree val / test macro-F1 | MLP val / test macro-F1 | deployed "
               "| C int8 vs Python float (test) | C int8 vs numpy int8 | C tree vs sklearn |")
    out.append("|---|---|---|---|---|---|---|---|")
    for w, r in tr["windows"].items():
        a = ag[w]
        out.append(f"| {w} | {r['tree']['depth']} / {r['tree']['nodes']} | {r['tree']['val_macro_f1']:.3f} / "
                   f"{r['tree']['test']['macro_f1']:.3f} | {r['mlp']['val_macro_f1']:.3f} / "
                   f"{r['mlp']['test']['macro_f1']:.3f} | {r['selected']} | "
                   f"{100 * a['c_int8_mlp_vs_python_float_mlp']:.2f}% | {100 * a['c_int8_mlp_vs_python_int8_sim']:.2f}% | "
                   f"{100 * a['c_tree_vs_sklearn_tree']:.2f}% |")
    out.append(f"\nDeployed default window: **{tr['default_window']}** (selection rule in tools/edgeai/train.py).")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=30)
    a = ap.parse_args()
    md = ["# Edge AI vibration classifier - benchmark results",
          "",
          "Generated by `make bench-edgeai` (tools/edgeai/report.py) from real runs. All data is SYNTHETIC "
          "(src/periph/env_sim.h); timings in the classifier table are host-CPU wall clock, not MCU time; "
          "every system metric is simulated drive time.",
          "", "## Classifier (held-out test seeds 161-200)"]
    cls, evals = classifier_section()
    md += cls
    if evals:
        confusion_chart(evals)
        md += ["", "![confusion matrices](classifier_confusion.png)", "", "### Detection vs a threshold"]
        md += detection_section(evals)
        md += ["", "### Deployed model on the system-benchmark mixes"] + mix_section()
    md += ["", "### Training, quantisation and C/Python agreement"] + model_section()

    data = load_system()
    if data:
        md += ["", f"## System: P0 vs P1 vs P2 vs P3 ({a.seeds} seeds per mix)",
               "", "Closed-loop random 4 KiB writes, QD 1, write-through, 1 GiB region, 30 s of drive time "
               "per run; corruption is counted on read-back after a reboot without the environment."]
        md += system_tables(data, a.seeds)
        hl, verdicts = headline(data)
        md += ["", "### Paired differences (same seeds, same writes)"] + hl
        chart(data, "corrupt_per_1k_writes", "corrupted sectors per 1,000 writes", "system_corruption.png", log=True)
        chart(data, "write_iops", "write IOPS (drive time)", "system_iops.png")
        chart(data, "false_stall_ms_per_s", "needless write stall (ms per s)", "system_false_stall.png")
        md += ["", "![corruption](system_corruption.png)", "", "![IOPS](system_iops.png)", "",
               "![needless stall](system_false_stall.png)"]
        json.dump({"verdicts": [{"mix": m, "policy": p, "vs": r, "sig": s} for m, p, r, s in verdicts]},
                  open(os.path.join(R, "verdicts.json"), "w"), indent=1)
    with open(os.path.join(R, "results.md"), "w", newline="\n") as f:
        f.write("\n".join(md) + "\n")
    print(f"wrote {R}/results.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
