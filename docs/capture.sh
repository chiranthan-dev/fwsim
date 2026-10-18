#!/bin/bash
# docs/capture.sh - run the README example commands and save their real,
# unedited output to docs/screenshots/*.txt; docs/render.py then draws each
# capture as a terminal-style PNG. Run from the repository root after `make`.
set -e
OUT=docs/screenshots
mkdir -p "$OUT"
B=./build/lockfree

cap() {   # cap NAME COMMAND...
    local name=$1; shift
    { echo "\$ $*"; "$@" 2>&1; } > "$OUT/$name.txt"
    echo "captured $OUT/$name.txt"
}

cap cli_mixed      $B/fwsim --workload mixed --ops 2000 --qd 4 --region 65536 --time-scale 0.25
cap cli_writeback  $B/fwsim --workload randwrite --ops 1000 --qd 4 --time-scale 0.25
cap cli_writethru  $B/fwsim --workload randwrite --ops 1000 --qd 4 --write-through --time-scale 0.25
cap cli_faults     $B/fwsim --workload randread --ops 2000 --qd 4 --region 16384 --cache 64 --flip-prob 0.01 --time-scale 0.05
cap test_powerloss $B/tests/test_powerloss
cap bench_powerloss $B/fwbench --csv /tmp/fwsim-capture.csv --only powerloss --trials 20

# The sanitizer runs are long; keep the per-suite summaries and the verdicts.
{ echo "\$ make tsan    # (output filtered to the per-suite summaries)";
  make tsan 2>&1 | grep -E "^test_[a-z]+: |^run-tests:"; } > "$OUT/make_tsan.txt"
echo "captured $OUT/make_tsan.txt"
{ echo "\$ make asan    # (output filtered to the per-suite summaries)";
  make asan 2>&1 | grep -E "^test_[a-z]+: |^run-tests:"; } > "$OUT/make_asan.txt"
echo "captured $OUT/make_asan.txt"
rm -f /tmp/fwsim-capture.csv /tmp/powerloss_trials.csv
