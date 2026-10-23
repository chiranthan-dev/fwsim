#!/bin/bash
# ui/tsan_smoke.sh - drive fwsim-ui through a scripted session (runs, FLUSH,
# two power cuts, a wipe, a stop) under ThreadSanitizer, with an event-stream
# client attached, and report any TSan warnings. Run from the repo root:
#     make SAN=tsan ui && bash ui/tsan_smoke.sh
set -u
PORT=${PORT:-8091}
BIN=./build/lockfree-tsan/fwsim-ui
LOG=/tmp/fwsim-ui-tsan.log
SSE=/tmp/fwsim-ui-sse.txt
export TSAN_OPTIONS="halt_on_error=0"

setarch "$(uname -m)" -R "$BIN" --port "$PORT" --static ui/static \
    --media /tmp/fwsim-ui-tsan.media >"$LOG" 2>&1 &
SERVER=$!
sleep 2
timeout 40 curl -s -N "http://localhost:$PORT/events" >"$SSE" &
sleep 1
api() { curl -s "http://localhost:$PORT/api/$1" >/dev/null; }

api "start?workload=0&mode=0&scale=4";        sleep 5
api flush;                                    sleep 2
api cut;                                      sleep 5
api "start?workload=4&mode=3&scale=10&qd=8";  sleep 5
api cut;                                      sleep 5
api reset;                                    sleep 2
api "start?workload=2&mode=1&scale=4&flip=0.01"; sleep 4
api stop;                                     sleep 3

kill "$SERVER"
wait "$SERVER" 2>/dev/null
echo "event stream: $(grep -c '^data' "$SSE") events"
grep -o '"type":"[a-z]*"' "$SSE" | sort | uniq -c
grep -o '{"type":"verify"[^}]*}' "$SSE"
N=$(grep -c 'WARNING: ThreadSanitizer' "$LOG")
echo "ThreadSanitizer warnings: $N"
[ "$N" -eq 0 ] || head -60 "$LOG"
[ "$N" -eq 0 ]
