#!/usr/bin/env python3
"""
docs/make_gif.py - assemble the screenshots from capture_dashboard.mjs into
docs/dashboard.gif, keeping each frame's real capture timing.

    python docs/make_gif.py [frames_dir] [out.gif] [width]
"""
import glob
import json
import os
import sys

from PIL import Image

FRAMES = sys.argv[1] if len(sys.argv) > 1 else "docs/frames"
OUT = sys.argv[2] if len(sys.argv) > 2 else "docs/dashboard.gif"
WIDTH = int(sys.argv[3]) if len(sys.argv) > 3 else 760

files = sorted(glob.glob(os.path.join(FRAMES, "f*.png")))
times = json.load(open(os.path.join(FRAMES, "times.json")))
frames = []
for f in files:
    im = Image.open(f).convert("RGB")
    h = round(im.height * WIDTH / im.width)
    frames.append(im.resize((WIDTH, h), Image.LANCZOS))

# One shared palette keeps the dark background from flickering between
# frames. Build it from a mosaic of frames spread over the whole capture so
# every colour that ever appears (dirty, clean, lost, the arm) gets an entry.
picks = [frames[i * (len(frames) - 1) // 5] for i in range(6)]
mosaic = Image.new("RGB", (picks[0].width, picks[0].height * len(picks)))
for k, im in enumerate(picks):
    mosaic.paste(im, (0, k * im.height))
ref = mosaic.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
pal = [im.quantize(palette=ref, dither=Image.Dither.NONE) for im in frames]
durations = [max(40, times[i + 1] - times[i]) for i in range(len(times) - 1)] + [1500]
pal[0].save(OUT, save_all=True, append_images=pal[1:], duration=durations, loop=0,
            optimize=True, disposal=1)
print(f"wrote {OUT}: {len(pal)} frames, {os.path.getsize(OUT) / 1e6:.1f} MB")
