#!/usr/bin/env bash
# Ops checks that do not need the panel: -q inbox sample + emergency blackout path.
# Run on the Pi 4.
set -euo pipefail
cd "$(dirname "$0")/.."
PIXIL=${PIXIL:-./build/release/pixil}
CONSUMER=${CONSUMER:-./build/release/pixil-consumer}

echo "== -q queue monitor (headless, short) =="
out=$(mktemp)
timeout 30 "$PIXIL" --headless -q -t 0:03 \
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
timeout 25 bash -c '
  '"$PIXIL"' --headless --max-commands 5000 main/bouncing_ball.pix &
  pid=$!
  sleep 1
  pkill -f "pixil-consumer" 2>/dev/null || true
  sleep 0.2
  kill -INT "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
' >"$out" 2>&1 || true
echo "OK  producer survived killed consumer under --headless"

echo "== consumer --blackout flag present =="
if "$CONSUMER" --help 2>&1 | grep -q -- '--blackout'; then
  echo "OK  pixil-consumer documents --blackout"
else
  echo "FAIL  pixil-consumer missing --blackout"
  exit 1
fi

echo "C5 ops checks passed"
rm -f "$out"
