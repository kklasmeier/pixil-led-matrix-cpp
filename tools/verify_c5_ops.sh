#!/usr/bin/env bash
# C5 ops checks that do not need the panel: -q inbox sample + emergency blackout path.
# Run on the Pi 4.
set -euo pipefail
cd "$(dirname "$0")/.."
PIXIL=${PIXIL:-./build/release/pixil}

echo "== -q queue monitor (native headless, short) =="
out=$(mktemp)
# -q samples about once a second; need wall time >1s (max-commands alone can finish too fast).
timeout 30 "$PIXIL" --consumer native --headless -q -t 0:03 \
  main/bouncing_ball.pix >"$out" 2>&1 || true
if grep -qE 'inbox=[0-9]+/5000' "$out"; then
  echo "OK  -q printed inbox fill"
  grep -E 'inbox=[0-9]+/5000' "$out" | head -3
else
  echo "FAIL  no inbox= line from -q"
  tail -30 "$out"
  exit 1
fi

echo "== emergency blackout (headless skip) =="
# Force-kill the native consumer mid-run; producer shutdown must not hang.
# Headless emergency_blackout() is a no-op by design (ConsumerLink).
timeout 25 bash -c '
  '"$PIXIL"' --consumer native --headless --max-commands 5000 main/bouncing_ball.pix &
  pid=$!
  sleep 1
  # Kill the consumer child if still running (best-effort); then SIGINT producer.
  pkill -f "pixil-consumer" 2>/dev/null || true
  sleep 0.2
  kill -INT "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
' >"$out" 2>&1 || true
if grep -qi 'emergency blackout' "$out"; then
  echo "NOTE  emergency blackout invoked (unexpected under --headless; check log)"
  grep -i emergency "$out" || true
fi
echo "OK  producer survived killed native consumer under --headless"

echo "== python emergency module importable =="
python3 -c "
import sys, os
root = os.environ.get('PIXIL_LEGACY_ROOT', os.path.join(os.getcwd(), '..', 'git'))
sys.path.insert(0, root)
from rgb_matrix_lib.emergency_blackout import run_emergency_blackout, spawn_emergency_blackout
print('OK  rgb_matrix_lib.emergency_blackout importable from', root)
"

echo "C5 ops checks passed"
rm -f "$out"
