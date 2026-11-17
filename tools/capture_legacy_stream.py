#!/usr/bin/env python3
"""Record the command stream legacy Pixil.py produces for a script (the oracle).

Runs the unmodified legacy producer in-process with the consumer replaced by a
recorder. No legacy files are edited; hooks are monkeypatches applied here:

  * MatrixCommandQueue: no consumer process; put_command/put_command_priority
    record; control-plane calls (drain, script reset, shutdown) become markers.
  * rgbmatrix: stubbed module, so nothing can touch GPIO.
  * database.PixilMetricsDB, runtime_status, runtime_control: no-ops, so a
    capture never reads or clears the live show's control/status files or
    writes the metrics database.
  * PIXIL_SEED (default 1): seeds `random`, `numpy.random`, and unseeded
    numpy default_rng() calls; PYTHONHASHSEED=0 is enforced by re-exec.

Output JSONL, one record per producer event:
  {"i": n, "k": "cmd", "v": "<command>", "instant": bool}
  {"i": n, "k": "throttle", "v": factor}
  {"i": n, "k": "marker", "v": "script_reset" | "drain" | "shutdown" | ...}
with --decode, draw_batch commands also carry "ops": decoded records.

Usage:
  tools/capture_legacy_stream.py SCRIPT.pix -o out.jsonl [--max-commands N] [--seed S]
"""

from __future__ import annotations

import argparse
import json
import os
import runpy
import sys
import types

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Recorder:
    def __init__(self, path: str, max_commands: int, decode: bool):
        self.out = open(path, "w", encoding="utf-8")
        self.n = 0
        self.commands = 0
        self.max_commands = max_commands
        self.decode = decode
        self.stopping = False

    def write(self, rec: dict) -> None:
        rec = {"i": self.n, **rec}
        self.n += 1
        self.out.write(json.dumps(rec) + "\n")

    def command(self, command: str, instant: bool) -> None:
        if self.stopping:
            return
        rec = {"k": "cmd", "v": command, "instant": bool(instant)}
        if self.decode and command.startswith("draw_batch("):
            from shared.draw_batch_protocol import unpack_draw_batch
            from shared.mplot_protocol import decode_buffer

            payload = command[len("draw_batch(") : -1].strip().strip('"').strip("'")
            rec["ops"] = [[name, list(args)] for name, args in unpack_draw_batch(decode_buffer(payload))]
        self.write(rec)
        self.commands += 1
        if self.commands >= self.max_commands:
            self.stopping = True
            self.write({"k": "marker", "v": "max_commands"})
            from pixil_utils.shutdown import request_shutdown

            request_shutdown()

    def marker(self, name: str, value=None) -> None:
        rec = {"k": "marker", "v": name}
        if value is not None:
            rec["arg"] = value
        self.write(rec)

    def close(self) -> None:
        self.out.close()


def install_rgbmatrix_stub() -> None:
    mod = types.ModuleType("rgbmatrix")

    class _Unavailable:
        def __init__(self, *a, **k):
            raise RuntimeError("capture_legacy_stream: rgbmatrix is stubbed (no hardware access)")

    mod.RGBMatrix = _Unavailable
    mod.RGBMatrixOptions = lambda: types.SimpleNamespace()
    mod.graphics = types.SimpleNamespace()
    sys.modules["rgbmatrix"] = mod


def install_seed(seed: int) -> None:
    import random

    random.seed(seed)
    try:
        import numpy as np
    except ImportError:
        return
    np.random.seed(seed % (2**32))
    original = np.random.default_rng
    counter = [0]

    def seeded_default_rng(seed_arg=None, *a, **k):
        if seed_arg is None:
            counter[0] += 1
            seed_arg = [seed, counter[0]]
        return original(seed_arg, *a, **k)

    np.random.default_rng = seeded_default_rng


def install_queue_recorder(rec: Recorder) -> None:
    from shared import command_queue as cq

    Q = cq.MatrixCommandQueue

    def put_command(self, command, force_instant=False):
        rec.command(command, force_instant)

    def put_command_priority(self, command, timeout=2.0):
        rec.marker("priority", command)

    def set_throttle(self, factor):
        rec.write({"k": "throttle", "v": factor})

    def request_fast_drain(self, timeout=3.0):
        rec.marker("drain")
        return 0

    def prepare_for_next_script(self, timeout=3.0):
        rec.marker("script_reset")

    def shutdown_display(self, timeout=5.0):
        rec.marker("shutdown")

    patches = {
        "start_consumer": lambda self: None,
        "put_command": put_command,
        "put_command_priority": put_command_priority,
        "set_throttle": set_throttle,
        "reset_throttle": lambda self: rec.write({"k": "throttle", "v": "reset"}),
        "wait_until_empty": lambda self, timeout=None: True,
        "wait_for_completion": lambda self, cooldown=1.0: None,
        "_consumer_is_healthy": lambda self: True,
        "is_empty": lambda self: True,
        "request_fast_drain": request_fast_drain,
        "prepare_for_next_script": prepare_for_next_script,
        "reset_for_next_script": prepare_for_next_script,
        "script_transition_cleanup": lambda self, cooldown=0.3: rec.marker("transition_cleanup"),
        "shutdown_display": shutdown_display,
        "stop_consumer": lambda self: None,
        "stop_consumer_graceful": lambda self, timeout=4.0: None,
        "stop_consumer_force": lambda self: None,
        "cleanup": lambda self: None,
        "wait_for_test_snapshot": lambda self, timeout=3.0: None,
        "drain_test_snapshot_reply": lambda self: None,
        "discard_pending": lambda self: 0,
    }
    for name, fn in patches.items():
        if hasattr(Q, name):
            setattr(Q, name, fn)


def install_exit_hook(rec: Recorder) -> None:
    from pixil_utils import shutdown

    def exit_pixil(queue_instance=None, queue_monitor=None):
        # Legacy ends with os._exit(0), which would drop buffered output.
        rec.marker("exit_pixil")
        raise SystemExit(0)

    shutdown.exit_pixil = exit_pixil


def install_side_effect_stubs() -> None:
    import database

    class NullMetricsDB:
        def __init__(self, *a, **k):
            pass

        def __getattr__(self, name):
            return lambda *a, **k: None

    database.PixilMetricsDB = NullMetricsDB

    from pixil_utils import runtime_control, runtime_status

    for name in ("initialize_status", "clear_status", "_write_status", "_clear_owned_queue_status"):
        if hasattr(runtime_status, name):
            setattr(runtime_status, name, lambda *a, **k: None)
    runtime_status.set_current_script = lambda *a, **k: 0.0
    runtime_status.read_status = lambda *a, **k: None

    class NullReporter:
        def __init__(self, *a, **k):
            pass

        def start(self):
            pass

        def stop(self):
            pass

        def reset_for_script(self, *a, **k):
            pass

    runtime_status.QueueStatusReporter = NullReporter
    runtime_control.clear_control_request = lambda *a, **k: None
    runtime_control.poll_control_request = lambda *a, **k: None
    runtime_control.consume_transition_request = lambda *a, **k: False
    runtime_control.consume_jump_target = lambda *a, **k: None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("script", help=".pix path (relative paths resolve against the legacy scripts/ dir)")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--legacy-root", default=os.environ.get("PIXIL_LEGACY_ROOT", os.path.join(REPO, "..", "git")))
    ap.add_argument("--max-commands", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=int(os.environ.get("PIXIL_SEED", "1")))
    ap.add_argument("--duration", default="10:00", help="legacy -t limit (stream is cut by --max-commands first)")
    ap.add_argument("--decode", action="store_true", help="decode draw_batch payloads into ops")
    args = ap.parse_args()

    if os.environ.get("PYTHONHASHSEED") != "0":
        os.environ["PYTHONHASHSEED"] = "0"
        os.execv(sys.executable, [sys.executable] + sys.argv)

    legacy = os.path.abspath(args.legacy_root)
    output = os.path.abspath(args.output)
    script = args.script
    os.chdir(legacy)
    sys.path.insert(0, legacy)

    install_rgbmatrix_stub()
    rec = Recorder(output, args.max_commands, args.decode)
    install_queue_recorder(rec)
    install_exit_hook(rec)
    install_side_effect_stubs()
    install_seed(args.seed)
    rec.write({"k": "meta", "v": {"script": script, "seed": args.seed, "max_commands": args.max_commands}})

    sys.argv = ["Pixil.py", script, "-t", args.duration]
    try:
        sys.stdin = open(os.devnull)  # Pixil.is_headless(): no terminal handler
        runpy.run_path(os.path.join(legacy, "Pixil.py"), run_name="__main__")
    except SystemExit as exc:
        if exc.code not in (0, None):
            rec.marker("exit", exc.code)
    finally:
        rec.close()
    print(f"[capture] {rec.commands} commands -> {output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
