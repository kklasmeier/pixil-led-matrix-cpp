#!/usr/bin/env python3
"""Tier 0 parity: legacy Python evaluator (oracle) vs native pixil-expr.

A case matches when both sides succeed with the same Python type and repr, or
both fail. Known differences are listed in tests/expr/expected_deviations.json
(each entry points at a DEVIATIONS.md id). Exit status is non-zero on any
unexpected mismatch, so this is usable as a gate.

    python3 tools/expr_crosscheck.py --native build/release/pixil-expr
    python3 tools/expr_crosscheck.py --native build/release/pixil-expr \
        --native-arg=--inject-fault=c_mod --expect-failure   # negative test
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def run_jsonl(cmd: list[str], stdin_path: str) -> dict[str, dict]:
    with open(stdin_path, "rb") as fh:
        proc = subprocess.run(cmd, stdin=fh, capture_output=True, check=False)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace"))
        raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    results = {}
    for line in proc.stdout.decode().splitlines():
        line = line.strip()
        if line.startswith("{"):
            row = json.loads(line)
            results[row["id"]] = row
    return results


def describe(row: dict | None) -> str:
    if row is None:
        return "<missing>"
    if row.get("ok"):
        return f"{row['type']} {row['repr']}"
    return f"error {row.get('error_type')}: {row.get('error')}"


def same(a: dict, b: dict) -> bool:
    if a.get("ok") and b.get("ok"):
        return a["type"] == b["type"] and a["repr"] == b["repr"]
    return not a.get("ok") and not b.get("ok")


def numerically_equal(a: dict, b: dict | None) -> bool:
    """Type-only deviations (int vs float) must still agree on the value."""
    if b is None or not (a.get("ok") and b.get("ok")):
        return False
    try:
        return float(a["repr"]) == float(b["repr"])
    except ValueError:
        return False


def deviation_id(entry) -> str:
    return entry if isinstance(entry, str) else entry["deviation"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cases", default=os.path.join(REPO, "tests", "expr", "cases.jsonl"))
    ap.add_argument("--deviations", default=os.path.join(REPO, "tests", "expr", "expected_deviations.json"))
    ap.add_argument("--native", default=os.path.join(REPO, "build", "release", "pixil-expr"))
    ap.add_argument("--native-arg", action="append", default=[])
    ap.add_argument("--legacy-root", default=None)
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--expect-failure", action="store_true",
                    help="negative test: succeed only if an unexpected mismatch is detected")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    oracle_cmd = [args.python, os.path.join(HERE, "expr_oracle.py")]
    if args.legacy_root:
        oracle_cmd += ["--legacy-root", args.legacy_root]
    oracle = run_jsonl(oracle_cmd, args.cases)
    native = run_jsonl([args.native, *args.native_arg], args.cases)
    with open(args.deviations, encoding="utf-8") as fh:
        expected = {k: v for k, v in json.load(fh).items() if not k.startswith("_")}

    unexpected, deviations, stale = [], [], []
    for case_id, o in oracle.items():
        n = native.get(case_id)
        matched = n is not None and same(o, n)
        if case_id in expected:
            entry = expected[case_id]
            if matched:
                stale.append((case_id, o, n))
            elif isinstance(entry, dict) and entry.get("numeric_equal") and not numerically_equal(o, n):
                unexpected.append((case_id, o, n))
            else:
                deviations.append((case_id, o, n))
        elif not matched:
            unexpected.append((case_id, o, n))
        elif args.verbose:
            print(f"  ok   {case_id}: {describe(o)}")

    for case_id, o, n in deviations:
        print(f"  DEV  {case_id} [{deviation_id(expected[case_id])}]: legacy={describe(o)} | native={describe(n)}")
    for case_id, o, n in stale:
        print(f"  STALE {case_id} [{deviation_id(expected[case_id])}] now matches: {describe(o)}")
    for case_id, o, n in unexpected:
        print(f"  FAIL {case_id}: legacy={describe(o)} | native={describe(n)}")
    total = len(oracle)
    print(f"cases={total} match={total - len(unexpected) - len(deviations)} "
          f"documented_deviations={len(deviations)} stale={len(stale)} unexpected={len(unexpected)}")

    failed = bool(unexpected or stale)
    if args.expect_failure:
        if failed:
            print("negative test OK: injected fault was detected")
            return 0
        print("negative test FAILED: injected fault went unnoticed")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
