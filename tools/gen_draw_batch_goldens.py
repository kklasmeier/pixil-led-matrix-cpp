#!/usr/bin/env python3
"""Generate draw_batch packing goldens from the legacy protocol (the oracle).

Writes JSONL: {"id", "cmd", "args", "hex"} or {"id", "cmd", "args", "error"}.
The final line is {"id": "batch", "b64": ...}: encode_buffer() of every
successful record concatenated in file order.

Usage: tools/gen_draw_batch_goldens.py [--legacy-root DIR] > tests/golden/draw_batch.jsonl
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NAMED = ["red", "Blue", "WHITE", "black", "transparent", "lavender", "sky_blue", "nope"]
COORDS = [0, 1, 31, 63, 64, 65, -1, -10, 100, 70000, 10.5, 11.5, -0.5, 62.6, 63.4, "12", "7.9"]
MODES = [None, "instant", "fade", "FADE", "'fade'", "\"instant\"", "bogus"]


def fixed_cases():
    yield "plot-basic", "plot", [10, 20, "red"]
    yield "plot-full", "plot", [10, 20, 50, 80, 1500, "fade"]
    yield "plot-strcolor", "mplot", ["5", "6", "42", "", "", None]
    yield "plot-floatcolor", "plot", [1, 2, 49.5]
    yield "plot-floatcolor-clamp", "plot", [1, 2, 150.2]
    yield "plot-neg", "plot", [-5, 70000, "blue"]
    yield "plot-bad-intensity", "plot", [1, 1, "red", 101]
    yield "plot-bad-color", "plot", [1, 1, 100]
    yield "line-inside", "draw_line", [0, 0, 63, 63, "white"]
    yield "line-clip", "draw_line", [-10, 32, 80, 32, "green", 75]
    yield "line-diag-clip", "draw_line", [-20.5, -3.25, 90.75, 70.5, 3, 100, 250, "fade"]
    yield "line-off", "draw_line", [-10, -10, -1, -5, "red"]
    yield "line-half", "draw_line", [0.5, 1.5, 2.5, 3.5, "red"]
    yield "rect-basic", "draw_rectangle", [5, 5, 10, 8, "yellow", 100, True]
    yield "rect-neg", "draw_rectangle", [-5, -5, 10, 10, "red", 60, "yes"]
    yield "rect-off", "draw_rectangle", [70, 5, 10, 10, "red"]
    yield "rect-zero", "draw_rectangle", [5, 5, 0, 10, "red"]
    yield "rect-burn", "draw_rectangle", [1.9, 2.9, 3.9, 4.9, 12, 40, False, 2000.0, "fade"]
    yield "circle-basic", "draw_circle", [32, 32, 10, "cyan", 100, True]
    yield "circle-negc", "draw_circle", [-1, 32, 10, "cyan"]
    yield "circle-r0", "draw_circle", [32, 32, 0, "cyan"]
    yield "circle-edge", "draw_circle", [70, 32, 7, "cyan"]
    yield "polygon-basic", "draw_polygon", [32, 32, 12, 6, "purple", 90, 22.5, True, 500, "fade"]
    yield "polygon-rot", "draw_polygon", [10, 10, 5, 3, 7, 100, 0.1]
    yield "ellipse-basic", "draw_ellipse", [32, 20, 15, 8, "orange", 80, False, 45.0]
    yield "ellipse-zero", "draw_ellipse", [32, 20, 0, 0, "orange"]
    yield "ellipse-yr", "draw_ellipse", [32, 20, 0, 4, "orange"]
    yield "arc-basic", "draw_arc", [5, 5, 50, 40, 0.3, "red", 100, False]
    yield "arc-clip", "draw_arc", [-5, 5, 80, 40, -1.7, 22, 55, True, 100, "fade"]
    yield "mode-bad", "draw_circle", [32, 32, 5, "red", 100, False, None, "bogus"]
    yield "intensity-255", "draw_circle", [32, 32, 5, "red", 255]
    yield "intensity-256", "draw_circle", [32, 32, 5, "red", 256]
    yield "intensity-none", "draw_circle", [32, 32, 5, "red", None]
    yield "intensity-float", "draw_line", [1, 1, 5, 5, "red", 50.9]


def rnd_color(rng):
    k = rng.random()
    if k < 0.4:
        return rng.choice(NAMED)
    if k < 0.7:
        return rng.randint(-2, 101)
    if k < 0.85:
        return str(rng.randint(0, 120))
    return round(rng.uniform(-5, 105), rng.choice([0, 1, 2]))


def rnd_coord(rng):
    if rng.random() < 0.3:
        return rng.choice(COORDS)
    return round(rng.uniform(-40, 110), rng.choice([0, 1, 3]))


def fuzz_cases(rng, count):
    shapes = {
        "plot": lambda: [rnd_coord(rng), rnd_coord(rng), rnd_color(rng)]
        + rng.choice([[], [rng.randint(0, 100)], [rng.randint(0, 100), rng.randint(0, 5000), rng.choice(MODES)]]),
        "draw_line": lambda: [rnd_coord(rng) for _ in range(4)] + [rnd_color(rng)]
        + rng.choice([[], [rng.randint(0, 100)], [rng.randint(0, 100), rng.choice([None, 300, 1200.0]), rng.choice(MODES)]]),
        "draw_rectangle": lambda: [rnd_coord(rng), rnd_coord(rng), rng.randint(-3, 70), rng.randint(-3, 70), rnd_color(rng)]
        + rng.choice([[], [rng.randint(0, 100), rng.choice([True, False, "true", "no", 1, 0])],
                      [rng.randint(0, 100), rng.choice([True, False]), rng.choice([None, 800]), rng.choice(MODES)]]),
        "draw_circle": lambda: [rnd_coord(rng), rnd_coord(rng), rng.choice([0, 1, 5, 30, 70, 2.7]), rnd_color(rng)]
        + rng.choice([[], [rng.randint(0, 100), rng.choice([True, False])],
                      [rng.randint(0, 100), True, rng.choice([None, 100]), rng.choice(MODES)]]),
        "draw_polygon": lambda: [rnd_coord(rng), rnd_coord(rng), rng.randint(0, 40), rng.randint(3, 9), rnd_color(rng)]
        + rng.choice([[], [rng.randint(0, 100), rng.uniform(-360, 360)],
                      [rng.randint(0, 100), rng.uniform(0, 90), rng.choice([True, False]), rng.choice([None, 50]), rng.choice(MODES)]]),
        "draw_ellipse": lambda: [rnd_coord(rng), rnd_coord(rng), rng.randint(0, 40), rng.randint(0, 40), rnd_color(rng)]
        + rng.choice([[], [rng.randint(0, 100), rng.choice([True, False]), rng.uniform(-180, 180)],
                      [rng.randint(0, 100), False, 30.0, rng.choice([None, 70]), rng.choice(MODES)]]),
        "draw_arc": lambda: [rnd_coord(rng) for _ in range(4)] + [rng.uniform(-2, 2), rnd_color(rng)]
        + rng.choice([[], [rng.randint(0, 100), rng.choice([True, False])],
                      [rng.randint(0, 100), False, rng.choice([None, 999]), rng.choice(MODES)]]),
    }
    names = sorted(shapes)
    for i in range(count):
        cmd = names[i % len(names)]
        yield f"fuzz-{i:04d}", cmd, shapes[cmd]()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--legacy-root", default=os.environ.get("PIXIL_LEGACY_ROOT", os.path.join(REPO, "..", "git")))
    ap.add_argument("--fuzz", type=int, default=700)
    ap.add_argument("--seed", type=int, default=20261114)
    args = ap.parse_args()

    sys.path.insert(0, os.path.abspath(args.legacy_root))
    from shared.draw_batch_protocol import encode_buffer, pack_draw_op  # noqa: E402

    rng = random.Random(args.seed)
    stream = bytearray()
    out = sys.stdout
    for case_id, cmd, cmd_args in list(fixed_cases()) + list(fuzz_cases(rng, args.fuzz)):
        rec = {"id": case_id, "cmd": cmd, "args": cmd_args}
        try:
            packed = pack_draw_op(cmd, list(cmd_args))
            rec["hex"] = packed.hex()
            stream.extend(packed)
        except Exception as exc:  # noqa: BLE001 - the error class is the golden
            rec["error"] = type(exc).__name__
        out.write(json.dumps(rec) + "\n")
    out.write(json.dumps({"id": "batch", "b64": encode_buffer(bytes(stream))}) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
