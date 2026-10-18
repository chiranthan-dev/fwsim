#!/usr/bin/env python3
"""
docs/render.py - draw each docs/screenshots/*.txt capture (real output saved
by docs/capture.sh) as a terminal-window PNG for the README. The text is
rendered verbatim; nothing is edited.
"""
import glob
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, FancyBboxPatch

BG = "#1a1a19"
BAR = "#2c2c2a"
FG = "#e8e7e1"
PROMPT = "#86b6ef"
DOTS = ("#e66767", "#c98500", "#199e70")

FONT = 9.5          # points
LINE_H = 1.32       # line height, in font sizes
CHAR_W = 0.6        # monospace advance, in font sizes


def render(path):
    lines = open(path, encoding="utf-8").read().rstrip("\n").split("\n")
    cols = max(len(l) for l in lines)
    px_per_pt = 150 / 72
    w_pt = cols * FONT * CHAR_W + 40
    h_pt = len(lines) * FONT * LINE_H + 52
    fig = plt.figure(figsize=(w_pt / 72, h_pt / 72), dpi=150)
    fig.patch.set_facecolor("white")
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, w_pt)
    ax.set_ylim(h_pt, 0)
    ax.axis("off")
    ax.add_patch(FancyBboxPatch((2, 2), w_pt - 4, h_pt - 4, boxstyle="round,pad=0,rounding_size=8",
                                facecolor=BG, edgecolor="none"))
    ax.add_patch(FancyBboxPatch((2, 2), w_pt - 4, 22, boxstyle="round,pad=0,rounding_size=8",
                                facecolor=BAR, edgecolor="none"))
    ax.add_patch(plt.Rectangle((2, 14), w_pt - 4, 10, facecolor=BAR, edgecolor="none"))
    for i, c in enumerate(DOTS):
        ax.add_patch(Circle((16 + i * 14, 13), 4, facecolor=c, edgecolor="none"))
    title = os.path.basename(path)[:-4] + "  -  fwsim"
    ax.text(w_pt / 2, 13, title, color="#898781", fontsize=8, ha="center", va="center",
            family="DejaVu Sans")
    y = 24 + 12
    for line in lines:
        color = PROMPT if line.startswith("$ ") else FG
        ax.text(20, y, line, color=color, fontsize=FONT, family="DejaVu Sans Mono",
                va="top", ha="left")
        y += FONT * LINE_H
    out = path[:-4] + ".png"
    fig.savefig(out, dpi=150, facecolor="white")
    plt.close(fig)
    print("rendered", out, f"({px_per_pt:.0f} px/pt)")


if __name__ == "__main__":
    for p in sorted(glob.glob("docs/screenshots/*.txt")):
        render(p)
