#!/usr/bin/env python3
"""Evaluate expression cases with the LEGACY Python Pixil evaluator (the oracle).

Reads the same JSON Lines case format as build/<mode>/pixil-expr and prints one
JSON result per line. The legacy tree is imported read-only; nothing is written.

    python3 tools/expr_oracle.py --legacy-root ../git tests/expr/cases.jsonl
"""
from __future__ import annotations

import argparse
import json
import os
import random
import sys


def default_legacy_root() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    return os.environ.get("PIXIL_LEGACY_ROOT") or os.path.normpath(os.path.join(here, "..", "..", "git"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cases", nargs="?", help="cases .jsonl (default: stdin)")
    ap.add_argument("--legacy-root", default=default_legacy_root())
    args = ap.parse_args()

    sys.path.insert(0, os.path.abspath(args.legacy_root))
    from pixil_utils.array_manager import PixilArray
    from pixil_utils.condition_templates import clear_condition_cache
    from pixil_utils.math_functions import (
        clear_all_math_caches,
        evaluate_condition,
        evaluate_math_expression,
    )
    from pixil_utils.variable_registry import VariableRegistry

    src = open(args.cases, encoding="utf-8") if args.cases else sys.stdin
    for line in src:
        if not line.strip():
            continue
        case = json.loads(line)
        clear_all_math_caches()
        clear_condition_cache()
        reg = VariableRegistry()
        for name, value in (case.get("vars") or {}).items():
            reg.register(name)
            reg.set(name, value)
        for name, spec in (case.get("arrays") or {}).items():
            data = spec.get("data") or []
            arr = PixilArray(spec.get("size", len(data)), spec.get("type", "numeric"))
            for i, v in enumerate(data):
                arr[i] = v
            reg.register(name)
            reg.set(name, arr)
        if "seed" in case:
            random.seed(case["seed"])
        out = {"id": case["id"]}
        try:
            if case.get("kind") == "cond":
                result = bool(evaluate_condition(case["src"], reg))
            else:
                result = evaluate_math_expression(case["src"], reg)
            out.update(ok=True, type=type(result).__name__, repr=repr(result))
        except Exception as exc:  # the oracle reports failures, it does not stop
            out.update(ok=False, error_type=type(exc).__name__, error=str(exc).splitlines()[0])
        print(json.dumps(out), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
