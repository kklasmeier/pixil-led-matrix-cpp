#!/usr/bin/env bash
# C5 — headless drawing-buffer fingerprint compare: Python consumer vs native.
#
# Run on the Pi 4 (not through NFS):
#   nice -n 19 tools/consumer_parity_compare.sh
#   tools/consumer_parity_compare.sh main/bouncing_ball.pix:800
#
# Exit 0 when every entry matches. Prints MATCH / DIFF / FAIL per script.
# Fingerprints are opaque pixels in the drawing buffer only (sprites/background
# composite is out of band — see DEVIATIONS.md D-012).
set -euo pipefail

cd "$(dirname "$0")/.."
PIXIL=${PIXIL:-./build/release/pixil}
SEED=${PIXIL_SEED:-1}
FONT=${PIXIL_FONT_PATH:-fonts/tiny64_font.txt}

if [[ ! -x "$PIXIL" ]]; then
  echo "missing pixil binary: $PIXIL (build on Pi 4 first)" >&2
  exit 2
fi

# Abstract_Geometry uses short fade burnouts (e.g. 80ms). Python's background
# burnout thread races fingerprint timing (D-010) — hashes are non-deterministic
# and are not a hard MATCH gate. Geometry-without-burnout parity is covered by
# Hex_Grid_Glow + unit/fade smoke; exercise Abstract manually under D-010.
DEFAULT_SET=(
  main/bouncing_ball.pix:800
  main/KITT_Scanner.pix:800
  main/Circles.pix:800
  main/Hex_Grid_Glow.pix:600
  main/Plasma_Tunnel.pix:600
)

ENTRIES=("$@")
if [[ ${#ENTRIES[@]} -eq 0 ]]; then
  ENTRIES=("${DEFAULT_SET[@]}")
fi

extract_hash() {
  # Prefer the last PIXIL_BUFFER_HASH= line from stdout/stderr.
  grep -E '^PIXIL_BUFFER_HASH=' "$1" | tail -1 | sed 's/^PIXIL_BUFFER_HASH=//'
}

run_one() {
  local kind=$1 script=$2 max=$3 out=$4
  env PIXIL_FONT_PATH="$FONT" PIXIL_SEED="$SEED" \
    timeout 120 "$PIXIL" --consumer "$kind" --headless --fingerprint \
      --seed "$SEED" --max-commands "$max" "$script" \
      >"$out" 2>&1 || true
}

status=0
printf '%-28s %-18s %-18s %s\n' SCRIPT PYTHON NATIVE RESULT
for entry in "${ENTRIES[@]}"; do
  script=${entry%%:*}
  max=${entry##*:}
  if [[ "$script" == "$max" ]]; then max=800; fi
  name=$(basename "$script" .pix)
  tmp=$(mktemp -d)
  py_log="$tmp/py.log"
  na_log="$tmp/na.log"

  run_one python "$script" "$max" "$py_log"
  run_one native "$script" "$max" "$na_log"

  py=$(extract_hash "$py_log" || true)
  na=$(extract_hash "$na_log" || true)

  if [[ -z "${py:-}" || -z "${na:-}" ]]; then
    printf '%-28s %-18s %-18s %s\n' "$name" "${py:-MISSING}" "${na:-MISSING}" FAIL
    echo "---- python log (tail) ----"; tail -15 "$py_log" || true
    echo "---- native log (tail) ----"; tail -15 "$na_log" || true
    status=1
  elif [[ "$py" == "$na" ]]; then
    printf '%-28s %-18s %-18s %s\n' "$name" "$py" "$na" MATCH
  else
    printf '%-28s %-18s %-18s %s\n' "$name" "$py" "$na" DIFF
    status=1
  fi
  rm -rf "$tmp"
done
exit $status
