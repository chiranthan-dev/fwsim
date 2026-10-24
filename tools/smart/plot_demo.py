"""plot_demo.py - risk-score timeline of the firmware deployment demo.

    python tools/smart/plot_demo.py demo_timeline.csv demo_timeline.png

Needs only matplotlib (no pandas), so `make smart-demo` works anywhere.
"""
import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def main() -> int:
    src, dst = sys.argv[1:3]
    with open(src) as fh:
        rows = list(csv.DictReader(fh))
    day = [int(r["day"]) for r in rows]
    risk = [int(r["risk_bp"]) / 100.0 for r in rows]
    logit = [int(r["logit_q8"]) / 256.0 for r in rows]
    thr = int(rows[0]["threshold_q8"]) / 256.0
    a5 = [int(r["smart_5"]) for r in rows]
    a187 = [int(r["smart_187"]) for r in rows]
    a197 = [int(r["smart_197"]) for r in rows]
    flag_day = next((d for d, r in zip(day, rows) if r["predicted_failure"] == "1"), None)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 6), dpi=120, sharex=True,
                                   gridspec_kw={"height_ratios": [3, 2]})
    ax1.plot(day, logit, "-", color="#1f77b4", lw=2, label="model log-odds (SMART_READ logit_q8 / 256)")
    ax1.axhline(thr, color="#d62728", ls="--", lw=1.2, label=f"flag threshold ({thr:+.2f})")
    if flag_day is not None:
        ax1.axvline(flag_day, color="#d62728", lw=0.8, alpha=0.6)
        ax1.annotate(f"flag trips, day {flag_day}", (flag_day, thr), xytext=(6, -18),
                     textcoords="offset points", color="#d62728", fontsize=8)
    ax1b = ax1.twinx()
    ax1b.plot(day, risk, ":", color="#555555", lw=1.2, label="risk, % (fail within 30 days)")
    ax1b.set_ylabel("risk %", color="#555555")
    ax1.set_ylabel("log-odds")
    h1, l1 = ax1.get_legend_handles_labels()
    h2, l2 = ax1b.get_legend_handles_labels()
    ax1.legend(h1 + h2, l1 + l2, fontsize=7, loc="upper left")
    ax1.set_title("fwsim SMART deployment demo - SYNTHETIC counters (scripted defects)\n"
                  "shows the firmware pipeline works; accuracy claims come only from the "
                  "Backblaze test set", fontsize=9)
    ax1.grid(alpha=0.3)
    ax2.plot(day, a5, label="5 reallocated")
    ax2.plot(day, a187, label="187 reported uncorrectable")
    ax2.plot(day, a197, label="197 pending")
    ax2.set_xlabel("simulated day")
    ax2.set_ylabel("raw count")
    ax2.legend(fontsize=7, loc="upper left")
    ax2.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(dst)
    print(f"plot_demo: {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
