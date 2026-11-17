#!/usr/bin/env bash
# Capture the Phase 1 oracle stream set twice with the same seed and prove determinism.
# Run on the Pi 4 (never through NFS):  nice -n 19 tools/capture_set.sh [out_dir]
# Streams are stored gzip'd (gzip -n: byte-stable). Per-script command caps keep
# batch-heavy scripts (base64 frames) small enough for git.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=${1:-tests/streams}
SEED=${PIXIL_SEED:-1}
SCRIPTS=(
  main/bouncing_ball.pix:3000
  main/KITT_Scanner.pix:3000
  main/Hex_Grid_Glow.pix:3000
  main/Ink_Storm.pix:3000
  main/Metaballs_Effect.pix:300
  main/Mandelbrot_Zoom.pix:300
  main/Cloth_simulation.pix:600
  main/Circles.pix:3000
  main/Abstract_Geometry.pix:3000
)

mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
status=0
for entry in "${SCRIPTS[@]}"; do
  s=${entry%%:*}
  max=${entry##*:}
  name=$(basename "$s" .pix)
  a="$TMP/$name.a.jsonl"
  b="$TMP/$name.b.jsonl"
  start=$(date +%s)
  if ! python3 tools/capture_legacy_stream.py "$s" -o "$a" --max-commands "$max" --seed "$SEED" >"$TMP/$name.log" 2>&1; then
    echo "FAIL  $name (capture error)"; tail -20 "$TMP/$name.log"; status=1; continue
  fi
  python3 tools/capture_legacy_stream.py "$s" -o "$b" --max-commands "$max" --seed "$SEED" >/dev/null 2>&1 || true
  cmds=$(grep -c '"k": "cmd"' "$a" || true)
  gzip -n -9 -c "$a" >"$OUT/$name.seed$SEED.jsonl.gz"
  secs=$(( $(date +%s) - start ))
  if cmp -s "$a" "$b"; then
    echo "OK    $name  commands=$cmds  raw=$(stat -c %s "$a")  gz=$(stat -c %s "$OUT/$name.seed$SEED.jsonl.gz")  ${secs}s  deterministic"
  else
    echo "DIFF  $name  commands=$cmds  first difference: $(cmp "$a" "$b" | head -1)"; status=1
  fi
done
exit $status
