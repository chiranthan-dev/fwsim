#!/usr/bin/env python3
"""
compare_bench.py - did the Edge AI feature (off by default) change `make
bench`? Compares runs of the same experiments with the same commands:

  OLD  results/baseline/results.csv   pre-feature binaries (acac711), saved first
       <dir>/old/results.csv          the same binaries, run again
       <dir>/old2/results.csv         ... and again for the experiments that
                                      looked suspicious (optional)
  NEW  <dir>/new/results.csv          post-feature binaries, right after old/
       <dir>/new2/results.csv         ... right after old2/ (optional)

Most metrics are timed on the wall clock (time_scale 0.25) and differ from
run to run even with identical code. A metric is
  identical     every NEW value equals the OLD rerun exactly;
  within noise  the range of NEW values overlaps the range of OLD values,
                widened by max(10% of the OLD mean, 0.5);
  outside noise otherwise.
Deterministic metrics (counts, errors, durability violations, the CRC check
value, the time-scale-0 integrity experiment) must be identical.

  python3 tools/compare_bench.py build/bench-compare docs/baseline-check.md
"""
import csv
import os
import sys


def load(p):
    if not os.path.exists(p):
        return {}
    with open(p) as f:
        return {(r["experiment"], r["variant"], r["metric"]): float(r["value"])
                for r in csv.DictReader(f)}


def main():
    d, out = sys.argv[1], sys.argv[2]
    olds = [load("results/baseline/results.csv"), load(f"{d}/old/results.csv"), load(f"{d}/old2/results.csv")]
    news = [load(f"{d}/new/results.csv"), load(f"{d}/new2/results.csv")]
    keys = [k for k in olds[0] if k in olds[1] and k in news[0]]
    missing = sorted(set(olds[0]) ^ set(news[0]))
    rows, counts, flagged = [], {"identical": 0, "within noise": 0, "**outside noise**": 0}, []
    for k in keys:
        ov = [o[k] for o in olds if k in o]
        nv = [n[k] for n in news if k in n]
        if all(v == olds[1][k] for v in nv):
            verdict = "identical"
        else:
            tol = max(0.10 * abs(sum(ov) / len(ov)), 0.5)
            overlap = max(nv) >= min(ov) - tol and min(nv) <= max(ov) + tol
            verdict = "within noise" if overlap else "**outside noise**"
        counts[verdict] += 1
        if verdict != "identical" and verdict != "within noise":
            flagged.append(k)
        rows.append((k, ov, nv, verdict))
    det = [r for r in rows if r[0][0] in ("integrity", "meta") or r[0][2] in
           ("ops", "errors", "trials", "durability_violations", "unreadable_after_reboot")]
    det_ok = sum(1 for r in det if r[3] == "identical")
    fmtv = lambda vs: " / ".join(f"{v:.6g}" for v in vs)
    with open(out, "w", newline="\n") as f:
        f.write("# `make bench` before and after the Edge AI feature\n\n")
        f.write("The feature is off by default, so `make bench` must not change. Because most of its "
                "metrics are wall-clock measurements (time_scale 0.25 on a shared WSL2 VM) that vary "
                "between runs of identical code, the same commands as `make bench` were run with the "
                "pre-feature binaries (commit acac711) and the post-feature binaries, back to back:\n\n"
                "- OLD: the baseline saved before any change (`results/baseline/`), a rerun of the same "
                "binaries, and for the experiments that looked suspicious a third run\n"
                "- NEW: the post-feature binaries, each run directly after an OLD run\n\n"
                "A metric is *identical* when every NEW value equals the OLD rerun; *within noise* when "
                "the NEW values' range overlaps the OLD values' range widened by max(10% of the OLD "
                "mean, 0.5); otherwise *outside noise*.\n\n")
        f.write(f"**{len(keys)} metrics: {counts['identical']} identical, {counts['within noise']} within "
                f"run-to-run noise, {counts['**outside noise**']} outside.** Deterministic metrics (op "
                f"counts, errors, durability violations, the CRC check value, the time-scale-0 integrity "
                f"experiment): **{det_ok} of {len(det)} identical.**\n\n")
        if missing:
            f.write(f"Metrics present in only one run: {missing}\n\n")
        if flagged:
            f.write("Outside noise: " + ", ".join("/".join(k) for k in flagged) + "\n\n")
        f.write("| experiment | variant | metric | OLD runs | NEW runs | verdict |\n|---|---|---|---|---|---|\n")
        for (e, v, m), ov, nv, verdict in rows:
            f.write(f"| {e} | {v} | {m} | {fmtv(ov)} | {fmtv(nv)} | {verdict} |\n")
    print(f"{len(keys)} metrics: {counts['identical']} identical, {counts['within noise']} within noise, "
          f"{counts['**outside noise**']} outside; deterministic {det_ok}/{len(det)} identical")
    for k in flagged:
        print("  outside noise:", k, [o.get(k) for o in olds], [n.get(k) for n in news])
    return 0


if __name__ == "__main__":
    sys.exit(main())
