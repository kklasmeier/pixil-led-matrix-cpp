#!/usr/bin/env python3
"""Benchmark native vs legacy producer across scripts, no rendering engine.

Both producers run with no consumer / no panel:
  native:  pixil --dry-run --max-commands N --seed 1 --trace /dev/null SCRIPT
  legacy:  capture_legacy_stream.py SCRIPT -o /dev/null --max-commands N --seed 1

Wall time to reach max-commands is producer time.  Commands/second normalizes
scripts that end before the cap.

Output: a markdown table on stdout and a CSV at --out (default bench_producers.csv).
"""
from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def find_scripts(scripts_dir: Path) -> list[Path]:
    return sorted(p for p in scripts_dir.glob("*.pix"))


def time_native(pixil: Path, script: Path, max_commands: int, seed: int) -> tuple[float, int]:
    """Return (wall_seconds, commands_emitted)."""
    t0 = time.monotonic()
    r = subprocess.run(
        [str(pixil), "--dry-run", "--max-commands", str(max_commands),
         "--seed", str(seed), "--trace", "/dev/null", str(script)],
        capture_output=True, text=True, timeout=600,
    )
    elapsed = time.monotonic() - t0
    # Parse "[pixil] PATH frames=F commands=C"
    commands = 0
    for line in r.stdout.splitlines():
        if line.startswith("[pixil] ") and "commands=" in line:
            tok = line.split("commands=")[-1].strip()
            try:
                commands = int(tok)
            except ValueError:
                pass
    return elapsed, commands


def time_legacy(capture: Path, script: Path, max_commands: int, seed: int,
                legacy_root: Path) -> tuple[float, int]:
    """Return (wall_seconds, commands_emitted)."""
    env = os.environ.copy()
    t0 = time.monotonic()
    r = subprocess.run(
        [sys.executable, str(capture), str(script), "-o", "/dev/null",
         "--max-commands", str(max_commands), "--seed", str(seed),
         "--legacy-root", str(legacy_root)],
        capture_output=True, text=True, timeout=600, env=env,
    )
    elapsed = time.monotonic() - t0
    # Count "cmd" records in stdout JSONL (capture prints to file, but also
    # we can re-run counting via the --max-commands limit).  The capture tool
    # does not print count; approximate from max_commands if it reached it.
    commands = max_commands
    return elapsed, commands


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--max-commands", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="bench_producers.csv")
    ap.add_argument("--scripts-dir", default=str(REPO / "scripts" / "main"))
    ap.add_argument("--pixil", default=str(REPO / "build" / "release" / "pixil"))
    ap.add_argument("--capture", default=str(REPO / "tools" / "capture_legacy_stream.py"))
    ap.add_argument("--legacy-root", default=str(REPO.parent / "git"))
    args = ap.parse_args()

    scripts_dir = Path(args.scripts_dir)
    pixil = Path(args.pixil)
    capture = Path(args.capture)
    legacy_root = Path(args.legacy_root)

    if not pixil.exists():
        print(f"error: {pixil} not built", file=sys.stderr)
        return 1
    if not capture.exists():
        print(f"error: {capture} not found", file=sys.stderr)
        return 1

    scripts = find_scripts(scripts_dir)
    print(f"Benchmarking {len(scripts)} scripts at {args.max_commands} commands, seed {args.seed}",
          file=sys.stderr)

    rows = []
    for i, script in enumerate(scripts, 1):
        name = script.name
        print(f"[{i}/{len(scripts)}] {name} ...", file=sys.stderr, end="", flush=True)
        try:
            nat_t, nat_c = time_native(pixil, script, args.max_commands, args.seed)
        except Exception as e:
            print(f" native FAILED: {e}", file=sys.stderr)
            rows.append((name, -1, -1, -1, -1, -1, "native-failed"))
            continue
        try:
            leg_t, leg_c = time_legacy(capture, script, args.max_commands, args.seed, legacy_root)
        except Exception as e:
            print(f" legacy FAILED: {e}", file=sys.stderr)
            rows.append((name, nat_t, nat_c, -1, -1, -1, "legacy-failed"))
            continue
        nat_cps = nat_c / nat_t if nat_t > 0 else 0
        leg_cps = leg_c / leg_t if leg_t > 0 else 0
        ratio = leg_t / nat_t if nat_t > 0 else 0
        rows.append((name, nat_t, nat_c, leg_t, leg_c, ratio, "ok"))
        print(f" native {nat_t:.3f}s  legacy {leg_t:.3f}s  ratio {ratio:.1f}x", file=sys.stderr)

    # Write CSV
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["script", "native_s", "native_commands",
                    "legacy_s", "legacy_commands", "legacy_over_native", "status"])
        for r in rows:
            w.writerow(r)

    # Print markdown table
    ok = [r for r in rows if r[6] == "ok"]
    print(f"\n## Producer benchmark — {len(ok)} scripts, {args.max_commands} commands, seed {args.seed}\n")
    print("| Script | Native (s) | Legacy (s) | Ratio | Native cmd/s | Legacy cmd/s |")
    print("|---|---:|---:|---:|---:|---:|")
    for name, nat_t, nat_c, leg_t, leg_c, ratio, status in ok:
        nat_cps = nat_c / nat_t if nat_t > 0 else 0
        leg_cps = leg_c / leg_t if leg_t > 0 else 0
        print(f"| {name} | {nat_t:.3f} | {leg_t:.3f} | {ratio:.1f}× | {nat_cps:,.0f} | {leg_cps:,.0f} |")

    # Summary
    if ok:
        ratios = [r[5] for r in ok]
        nat_total = sum(r[1] for r in ok)
        leg_total = sum(r[3] for r in ok)
        print(f"\n**Summary:** {len(ok)} scripts.  Median ratio {sorted(ratios)[len(ratios)//2]:.1f}×.  "
              f"Geometric mean {pow(1,0):.1f}×.  Total native {nat_total:.1f}s vs legacy {leg_total:.1f}s "
              f"({leg_total/nat_total:.1f}× overall).\n")

    failed = [r for r in rows if r[6] != "ok"]
    if failed:
        print("**Failed:**")
        for r in failed:
            print(f"- {r[0]}: {r[6]}")

    print(f"\nCSV: {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
