#!/usr/bin/env python3
"""Generate tests/expr/cases.jsonl for the Tier 0 expression cross-check.

Sources:
  * legacy tests/pixil/_math_cases.py and _condition_cases.py (imported, not copied)
  * HAZARD_CASES below: the Python-semantics hazards of plan §6.6 plus arrays,
    strings, randomness, and error behaviour.

    python3 tools/gen_expr_cases.py --legacy-root ../git > tests/expr/cases.jsonl
"""
from __future__ import annotations

import argparse
import json
import os
import sys

BASE_VARS = {"v_x": 10, "v_y": 5, "v_z": 15, "v_a": 1, "v_b": 0}
HAZARD_VARS = {
    **BASE_VARS,
    "v_neg": -1,
    "v_f": 2.5,
    "v_negf": -2.5,
    "v_tenth": 0.1,
    "v_zero": 0,
    "v_name": "bob",
    "v_color": "red",
    "v_i": 2,
}
ARRAYS = {
    "v_values": {"type": "numeric", "size": 5, "data": [10, 20, 30, 40, 50]},
    "v_empty": {"type": "numeric", "size": 3, "data": []},
    "v_words": {"type": "string", "size": 2, "data": ["Hello", "World"]},
}

# (id, kind, src[, seed])
HAZARD_CASES = [
    # % floor modulo
    ("mod-neg-lit", "expr", "-1 % 64"),
    ("mod-neg-var", "expr", "v_neg % 64"),
    ("mod-neg7-3", "expr", "-7 % 3"),
    ("mod-7-neg3", "expr", "7 % -3"),
    ("mod-negf", "expr", "-7.5 % 2"),
    ("mod-f-negdiv", "expr", "5.5 % -2"),
    ("mod-negf-var", "expr", "v_negf % 1"),
    ("mod-zero-num", "expr", "0 % 5"),
    ("mod-by-zero", "expr", "v_x % 0"),
    ("mod-f-by-zero", "expr", "v_f % 0"),
    ("mod-wrap-cond", "cond", "v_neg % 64 == 63"),
    ("mod-cond", "cond", "v_x % 3 == 1"),
    # / is true division; // floors
    ("div-10-4", "expr", "10 / 4"),
    ("div-10-5", "expr", "10 / 5"),
    ("div-vars", "expr", "v_x / v_y"),
    ("div-third", "expr", "1 / 3"),
    ("div-by-zero", "expr", "v_x / 0"),
    ("fdiv-7-2", "expr", "7 // 2"),
    ("fdiv-neg7-2", "expr", "-7 // 2"),
    ("fdiv-neg7f-2", "expr", "-7.0 // 2"),
    ("fdiv-7.5-neg2", "expr", "7.5 // -2"),
    ("fdiv-negvar", "expr", "v_neg // 2"),
    # round (banker's)
    ("round-2.5", "expr", "round(2.5)"),
    ("round-3.5", "expr", "round(3.5)"),
    ("round-neg2.5", "expr", "round(-2.5)"),
    ("round-0.5", "expr", "round(0.5)"),
    ("round-1.5", "expr", "round(1.5)"),
    ("round-var", "expr", "round(v_f)"),
    ("round-2.675-2", "expr", "round(2.675, 2)"),
    ("round-1.5-0", "expr", "round(1.5, 0)"),
    ("round-int", "expr", "round(7)"),
    ("round-15-neg1", "expr", "round(15, -1)"),
    ("round-25-neg1", "expr", "round(25, -1)"),
    ("round-pi-3", "expr", "round(3.14159, 3)"),
    # ** power
    ("pow-2-10", "expr", "2 ** 10"),
    ("pow-2-neg1", "expr", "2 ** -1"),
    ("pow-float", "expr", "2.0 ** 3"),
    ("pow-neg-lit", "expr", "-2 ** 2"),
    ("pow-paren-neg", "expr", "(-2) ** 2"),
    ("pow-negvar", "expr", "v_neg ** 2"),
    ("pow-negfvar", "expr", "v_negf ** 2"),
    ("pow-fn-sqrt", "expr", "pow(2, 0.5)"),
    ("pow-neg-frac", "expr", "pow(-8, 1/3)"),
    ("pow-0-0", "expr", "0 ** 0"),
    ("pow-sqrt2", "expr", "2 ** 0.5"),
    ("pow-big-int", "expr", "10 ** 20"),
    ("pow-0-neg", "expr", "0 ** -1"),
    # float formatting / repr
    ("repr-0.1+0.2", "expr", "0.1 + 0.2"),
    ("repr-1e16", "expr", "1e16"),
    ("repr-1e15", "expr", "1e15"),
    ("repr-1e-5", "expr", "1e-5 * 1"),
    ("repr-0.0001", "expr", "0.0001"),
    ("repr-big", "expr", "123456789.0 * 1000000000"),
    ("repr-3.0", "expr", "3 * 1.0"),
    ("repr-tenth3", "expr", "v_tenth * 3"),
    ("repr-small", "expr", "2.5e-7 * 2"),
    ("repr-negzero", "expr", "-0.0"),
    ("repr-f-times-0", "expr", "v_f * 0"),
    # int conversion family
    ("int-neg3.7", "expr", "int(-3.7)"),
    ("int-3.9", "expr", "int(3.9)"),
    ("int-var", "expr", "int(v_f)"),
    ("trunc-neg", "expr", "trunc(-2.5)"),
    ("floor-neg", "expr", "floor(-3.5)"),
    ("ceil-neg", "expr", "ceil(-3.5)"),
    ("floor-int", "expr", "floor(3)"),
    ("ceil-var", "expr", "ceil(v_f)"),
    # abs / min / max keep argument types
    ("abs-int", "expr", "abs(-3)"),
    ("abs-float", "expr", "abs(-3.0)"),
    ("abs-var", "expr", "abs(v_neg)"),
    ("min-tie", "expr", "min(1, 1.0)"),
    ("max-tie", "expr", "max(2, 2.0, 1)"),
    ("min-vars", "expr", "min(v_x, v_y, v_z)"),
    ("max-mixed", "expr", "max(v_f, 2)"),
    # and/or value semantics, truthiness, chains
    ("and-value", "expr", "v_x and v_y"),
    ("and-falsy", "expr", "v_b and v_y"),
    ("or-value", "expr", "v_b or v_y"),
    ("or-first", "expr", "v_a or v_b"),
    ("not-int", "expr", "not v_x"),
    ("not-zero", "expr", "not v_b"),
    ("chain-lt", "expr", "1 < 2 < 3"),
    ("chain-gt", "expr", "3 > 2 > 2"),
    ("bool-arith", "expr", "True + 1"),
    # conditions beyond the legacy table
    ("cond-and-vars", "cond", "v_x and v_y"),
    ("cond-not-and", "cond", "not v_b and v_a == 1"),
    ("cond-arith-both", "cond", "v_x + 1 > v_y * 2"),
    ("cond-float-ge", "cond", "v_f >= 2.5"),
    ("cond-int-float-eq", "cond", "v_x == 10.0"),
    ("cond-str-ne", "cond", 'v_name != "bob"'),
    ("cond-nested-parens", "cond", "((v_x > 5 or v_y < 10) and v_z == 15) or v_a == 1"),
    ("cond-nested-and", "cond", "v_x > 5 and (v_y > 2 and (v_z > 1))"),
    ("cond-falsy-var", "cond", "v_b"),
    ("cond-float-var", "cond", "v_tenth"),
    ("cond-zero-eq", "cond", "v_zero == 0"),
    ("cond-rhs-arith", "cond", "v_x >= v_z - 5"),
    ("cond-neg-rhs", "cond", "v_x - v_y > v_neg"),
    ("cond-true", "cond", "true"),
    ("cond-false", "cond", "false"),
    ("cond-and-or", "cond", "v_a == 1 and v_b == 0 or v_x < 0"),
    ("cond-not-paren-or", "cond", "not (v_x < 5 or v_y > 1)"),
    ("cond-str-eq-single", "cond", "v_color == 'red'"),
    ("cond-undefined", "cond", "v_missing > 1"),
    # & concatenation
    ("cat-lits", "expr", '"a" & "b"'),
    ("cat-strvar", "expr", 'v_name & "!"'),
    ("cat-int", "expr", '"x" & v_x'),
    ("cat-float", "expr", '"x" & v_f'),
    ("cat-neg", "expr", '"n=" & v_neg'),
    ("cat-tenth", "expr", '"t" & v_tenth'),
    ("cat-arith-part", "expr", '"sum" & v_x + 1'),
    ("cat-array", "expr", 'v_words[0] & " " & v_words[1]'),
    # arrays
    ("arr-lit-index", "expr", "v_values[2]"),
    ("arr-var-index", "expr", "v_values[v_i]"),
    ("arr-expr-index", "expr", "v_values[v_i + 1] * 2"),
    ("arr-float-index", "expr", "v_values[1.9]"),
    ("arr-oob", "expr", "v_values[5]"),
    ("arr-negative", "expr", "v_values[-1]"),
    ("arr-div", "expr", "v_values[0] / 3"),
    ("arr-nested", "expr", "v_values[v_values[0] - 9]"),
    ("arr-unassigned", "expr", "v_empty[0]"),
    ("arr-sum-neighbors", "expr", "v_values[v_i] + v_values[v_i - 1]"),
    ("arr-string", "expr", "v_words[1]"),
    # random (seeded; native PyRandom mirrors CPython MT19937)
    ("rand-fixed", "expr", "random(4, 4, 0)"),
    ("rand-int", "expr", "random(0, 63, 0)", 42),
    ("rand-1dp", "expr", "random(1, 5, 1)", 7),
    ("rand-2dp", "expr", "random(-1, 1, 2)", 123),
    ("rand-two-draws", "expr", "random(0, 1, 3) + random(0, 1, 3)", 99),
    ("rand-var-bounds", "expr", "random(v_y, v_x, 0)", 5),
    ("rand-times", "expr", "random(0, 10, 0) * 2", 2026),
    ("rand-bigseed", "expr", "random(0, 1000, 3)", 2**40 + 5),
    # errors
    ("err-undefined", "expr", "v_undefined + 1"),
    ("err-sqrt-neg", "expr", "sqrt(-1)"),
    ("err-log0", "expr", "log(0)"),
    ("err-unknown-fn", "expr", "unknown_fn(1)"),
    ("err-bare-word", "expr", "red"),
    ("err-trailing-op", "expr", "1 +"),
    ("err-unbalanced", "expr", "(1 + 2"),
    ("err-acos", "expr", "acos(2)"),
    # math catalog extras
    ("m-degrees-pi", "expr", "degrees(pi)"),
    ("m-atan2-vars", "expr", "atan2(v_y, v_x)"),
    ("m-log-base", "expr", "log(8, 2)"),
    ("m-hypot", "expr", "sqrt(v_x * v_x + v_y * v_y)"),
    ("m-orbit", "expr", "cos(v_x * 0.1) * 20 + 32"),
    ("m-fmod-neg", "expr", "fmod(-7, 3)"),
    ("m-remainder", "expr", "remainder(5, 2)"),
    ("m-copysign-var", "expr", "copysign(3, v_neg)"),
    ("m-test-mode", "expr", 'get_system("test_mode")'),
    ("m-sin-sum", "expr", "sin(v_x) + sin(v_y)"),
    ("m-int-div-grid", "expr", "int(v_x / 8)"),
    ("m-grid-index", "expr", "v_y * 8 + v_x"),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--legacy-root", default=os.environ.get("PIXIL_LEGACY_ROOT")
                    or os.path.normpath(os.path.join(here, "..", "..", "git")))
    args = ap.parse_args()
    sys.path.insert(0, os.path.abspath(args.legacy_root))
    from tests.pixil._condition_cases import CONDITION_WITH_SETUP, LEGACY_CONDITION_CASES
    from tests.pixil._math_cases import FAST_MATH_CASES, MATH_EXPR_CASES

    rows = []
    for i, (src, _expected) in enumerate(MATH_EXPR_CASES):
        rows.append({"id": f"legacy-math-{i:02d}", "kind": "expr", "src": src, "vars": BASE_VARS})
    for i, (src, _expected) in enumerate(FAST_MATH_CASES):
        rows.append({"id": f"legacy-fast-{i:02d}", "kind": "expr", "src": src, "vars": BASE_VARS})
    for i, (src, _expected) in enumerate(LEGACY_CONDITION_CASES):
        rows.append({"id": f"legacy-cond-{i:02d}", "kind": "cond", "src": src, "vars": BASE_VARS})
    for i, (src, _expected, setup) in enumerate(CONDITION_WITH_SETUP):
        rows.append({"id": f"legacy-cond-setup-{i:02d}", "kind": "cond", "src": src,
                     "vars": {**BASE_VARS, **setup}})
    for case in HAZARD_CASES:
        case_id, kind, src = case[:3]
        row = {"id": f"h-{case_id}", "kind": kind, "src": src, "vars": HAZARD_VARS, "arrays": ARRAYS}
        if len(case) > 3:
            row["seed"] = case[3]
        rows.append(row)
    ids = [r["id"] for r in rows]
    assert len(ids) == len(set(ids)), "duplicate case ids"
    for row in rows:
        print(json.dumps(row))
    return 0


if __name__ == "__main__":
    sys.exit(main())
