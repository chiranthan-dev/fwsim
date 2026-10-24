"""agree.py - does the firmware's integer model say what Python says?

    python tools/smart/agree.py DATA_DIR OUT_DIR path/to/fwsim-smart

Takes every row of the Backblaze TEST window (time split) that train.py
dumped, scores it with the C code (fwsim-smart score) and compares with
  (a) the Python integer reference (export.int_logit_q8): must be identical,
  (b) the float scikit-learn model: the quantisation error.
Writes OUT_DIR/agreement.json.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import export  # noqa: E402
import smartlib as sl  # noqa: E402


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    data, out, tool = sys.argv[1:]
    with open(os.path.join(out, "model_deploy.json")) as fh:
        model = json.load(fh)
    qm = export.quantize(model)
    nf = len(sl.DEPLOY_FEATURES)
    xpath = os.path.join(data, "deploy_test_X.int16")
    X = np.fromfile(xpath, dtype="<i2").reshape(-1, nf)
    zf = np.load(os.path.join(data, "deploy_test_logit.npy")) + model["prior_logit_shift"]

    opath = os.path.join(data, "deploy_test_c.int32")
    subprocess.run([tool, "score", xpath, opath], check=True)
    c = np.fromfile(opath, dtype="<i4").reshape(-1, 2)
    assert len(c) == len(X), (len(c), len(X))
    c_logit, c_risk = c[:, 0].astype(np.int64), c[:, 1].astype(np.int64)

    py_logit = export.int_logit_q8(X, qm)
    py_risk = export.risk_bp(py_logit, qm)
    pf = 1.0 / (1.0 + np.exp(-zf))                   # float probability
    thr_f = model["threshold_logit"] + model["prior_logit_shift"]
    flag_c = c_logit >= qm["thresh_q8"]
    flag_f = zf >= thr_f
    res = {
        "rows": int(len(X)),
        "c_vs_python_int_logit_identical_pct": 100.0 * float(np.mean(c_logit == py_logit)),
        "c_vs_python_int_risk_identical_pct": 100.0 * float(np.mean(c_risk == py_risk)),
        "c_vs_float_flag_agree_pct": 100.0 * float(np.mean(flag_c == flag_f)),
        "flag_disagreements": int(np.sum(flag_c != flag_f)),
        "max_abs_logit_diff_vs_float": float(np.max(np.abs(c_logit / 256.0 - zf))),
        "mean_abs_logit_diff_vs_float": float(np.mean(np.abs(c_logit / 256.0 - zf))),
        "max_abs_prob_diff_vs_float": float(np.max(np.abs(c_risk / 10000.0 - pf))),
        "flagged_rows_c": int(flag_c.sum()),
        "flagged_rows_float": int(flag_f.sum()),
    }
    # What 8-bit weights would have cost (Python integer reference only).
    q8 = export.quantize(model, bits=8)
    l8 = export.int_logit_q8(X, q8)
    res["weight_bits"] = qm["bits"]
    res["int8_max_abs_logit_diff_vs_float"] = float(np.max(np.abs(l8 / 256.0 - zf)))
    res["int8_max_abs_prob_diff_vs_float"] = float(np.max(np.abs(export.risk_bp(l8, q8) / 10000.0 - pf)))
    res["int8_flag_agree_pct"] = 100.0 * float(np.mean((l8 >= q8["thresh_q8"]) == flag_f))
    with open(os.path.join(out, "agreement.json"), "w") as fh:
        json.dump(res, fh, indent=2)
    for k, v in res.items():
        print(f"agree: {k} = {v}")
    ok = res["c_vs_python_int_logit_identical_pct"] == 100.0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
