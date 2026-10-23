#!/usr/bin/env python3
"""
ui/record.py - record a real dashboard session for the static replay.

Connects to a running fwsim-ui (make run-ui), drives it through a scripted
scenario with the same /api calls the buttons make, and saves every
Server-Sent Event with its arrival time to ui/static/demo-events.json. The
GitHub Pages copy of the dashboard replays that file with the original
timing; nothing in it is synthesized.

    python3 ui/record.py [--port 8080]
"""
import json
import subprocess
import sys
import threading
import time
import urllib.request

PORT = int(sys.argv[sys.argv.index("--port") + 1]) if "--port" in sys.argv else 8080
BASE = f"http://localhost:{PORT}"
OUT = "ui/static/demo-events.json"

# (seconds from start, api call)
SCRIPT = [
    (0.5, "reset"),
    (2.0, "start?workload=0&mode=0&qd=4&sorted=1&region=0&flip=0&scale=10"),
    (16.0, "cut"),
    (22.0, "start?workload=0&mode=1&qd=4&sorted=1&region=0&flip=0&scale=10"),
    (31.0, "cut"),
]
END = 36.0

events = []
t0 = time.monotonic()


def reader():
    # curl -N hands over each event the moment it arrives; urllib's response
    # object buffers a never-ending SSE body and stalls.
    p = subprocess.Popen(["curl", "-s", "-N", BASE + "/events"], stdout=subprocess.PIPE,
                         bufsize=0)
    for raw in iter(p.stdout.readline, b""):
        line = raw.decode("utf-8").rstrip("\n")
        if line.startswith("data: "):
            ms = (time.monotonic() - t0) * 1000.0
            events.append([round(ms, 1), json.loads(line[6:])])


threading.Thread(target=reader, daemon=True).start()
time.sleep(0.3)
for at, call in SCRIPT:
    time.sleep(max(0.0, at - (time.monotonic() - t0)))
    urllib.request.urlopen(BASE + "/api/" + call).read()
    print(f"{at:5.1f}s  {call}")
time.sleep(max(0.0, END - (time.monotonic() - t0)))

# On connect the server replays its last power-loss verdict, which belongs to
# an earlier session: keep the hello, then only what follows our reset.
first_reset = next(i for i, (_, e) in enumerate(events) if e["type"] == "reset")
events = [events[0]] + events[first_reset:]

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    json.dump({"recorded": time.strftime("%Y-%m-%d"), "events": events}, f, separators=(",", ":"))
kinds = {}
for _, e in events:
    kinds[e["type"]] = kinds.get(e["type"], 0) + 1
print(f"wrote {OUT}: {len(events)} events {kinds}")
for _, e in events:
    if e["type"] == "verify":
        print("  verify:", e)
