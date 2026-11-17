#!/usr/bin/env python3
"""Thin consumer server: native pixil producer -> legacy rgb_matrix_lib (ADR-001).

Spawned by the native producer with three inherited pipes:

  fd 3  data     producer -> bridge   framed records, strict FIFO
  fd 4  control  producer -> bridge   single out-of-band bytes (drain / shutdown)
  fd 5  replies  bridge -> producer   framed records

Frame = 1 type byte + little-endian u32 payload length + payload.

Data frames:    C command (UTF-8)   F fence (u32 id)   K drain marker
                R script reset      T test snapshot    X graceful shutdown
Control bytes:  D drain requested   X force shutdown
Reply frames:   H ready (JSON)      F fence ack (u32)  K drain done (u32 swallowed)
                R reset done        T fingerprint      X shutdown done
                E consumer error (bridge blacks out and exits after sending)

The execution loop mirrors shared/command_queue.py MatrixCommandQueue._consumer_loop
(without the per-command replay delay: Phase 1 pacing is throttle(0), plan §6.5).
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import signal
import struct
import sys
import threading
import time
import traceback
import types

HEADER = struct.Struct("<BI")
DATA_FD, CONTROL_FD, REPLY_FD = 3, 4, 5
QUEUE_SIZE = 5000  # same bound as MatrixCommandQueue
EOF = object()


def install_headless_rgbmatrix() -> None:
    """Stand-in for the rgbmatrix C extension: real RGB_Api, no GPIO."""

    class Canvas:
        def SetPixel(self, x, y, r, g, b):  # noqa: N802 - mirrors rgbmatrix API
            pass

        def Fill(self, r, g, b):  # noqa: N802
            pass

        def SetImage(self, image, offset_x=0, offset_y=0, unsafe=True):  # noqa: N802
            pass

        def Clear(self):  # noqa: N802
            pass

    class RGBMatrixOptions:
        def __init__(self):
            self.rows = 64
            self.cols = 64

    class RGBMatrix:
        def __init__(self, options=None):
            opts = options or RGBMatrixOptions()
            self.height = int(getattr(opts, "rows", 64))
            self.width = int(getattr(opts, "cols", 64)) * int(getattr(opts, "chain_length", 1) or 1)
            self.brightness = 100

        def CreateFrameCanvas(self):  # noqa: N802
            return Canvas()

        def SwapOnVSync(self, canvas, framerate_fraction=1):  # noqa: N802
            return canvas

        def Clear(self):  # noqa: N802
            pass

    mod = types.ModuleType("rgbmatrix")
    mod.RGBMatrix = RGBMatrix
    mod.RGBMatrixOptions = RGBMatrixOptions
    mod.graphics = types.SimpleNamespace()
    sys.modules["rgbmatrix"] = mod


class ReplyWriter:
    def __init__(self, fd: int):
        self._fd = fd
        self._lock = threading.Lock()

    def send(self, kind: bytes, payload: bytes = b"") -> None:
        frame = HEADER.pack(kind[0], len(payload)) + payload
        with self._lock:
            try:
                view = memoryview(frame)
                while view:
                    n = os.write(self._fd, view)
                    view = view[n:]
            except (BrokenPipeError, OSError):
                pass  # producer gone; data-pipe EOF handles shutdown


def read_exact(fd: int, n: int) -> bytes | None:
    buf = bytearray()
    while len(buf) < n:
        chunk = os.read(fd, n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


class Bridge:
    def __init__(self, args):
        self.args = args
        self.replies = ReplyWriter(REPLY_FD)
        self.inbox: queue.Queue = queue.Queue(maxsize=QUEUE_SIZE)
        self.drain_requested = threading.Event()
        self.force_shutdown = threading.Event()
        self.swallowed = 0
        self.api = None
        self.record = open(args.record, "w", encoding="utf-8") if args.record else None
        self.t0 = time.monotonic()

    # -- recorder (headless capture of what reached the consumer) ---------
    def log(self, kind: str, value=None) -> None:
        if self.record is None:
            return
        rec = {"t": round(time.monotonic() - self.t0, 6), "k": kind}
        if value is not None:
            rec["v"] = value
        self.record.write(json.dumps(rec) + "\n")

    # -- input threads -----------------------------------------------------
    def data_reader(self) -> None:
        try:
            while True:
                header = read_exact(DATA_FD, HEADER.size)
                if header is None:
                    break
                kind, length = HEADER.unpack(header)
                payload = read_exact(DATA_FD, length) if length else b""
                if payload is None:
                    break
                self.inbox.put((kind, payload))  # blocks when full: backpressure
        except OSError:
            pass
        self.inbox.put((None, EOF))

    def control_reader(self) -> None:
        try:
            while True:
                b = os.read(CONTROL_FD, 1)
                if not b:
                    return
                if b == b"D":
                    self.drain_requested.set()
                elif b == b"Q":
                    # Depth of commands waiting to be drawn. Answered here, not
                    # through the inbox, so the sample is not stuck behind them.
                    depth = self.inbox.qsize()
                    cap = self.inbox.maxsize or 0
                    self.replies.send(b"Q", struct.pack("<II", int(depth), int(cap)))
                elif b == b"X":
                    self.force_shutdown.set()
        except OSError:
            return

    # -- consumer actions (same order as _consumer_loop) --------------------
    def blackout_and_exit(self) -> None:
        try:
            self.api.blackout_display()
        except Exception as exc:  # noqa: BLE001
            print(f"[BRIDGE] Blackout error: {exc}", flush=True)
        self.log("blackout")

    def script_reset(self) -> None:
        api = self.api
        api.reset_fps()
        if api.frame_mode:
            try:
                api.end_frame()
            except Exception:  # noqa: BLE001
                api.frame_mode = False
                api.preserve_frame_changes = False
        api.clear()
        api.dispose_all_sprites()

    def snapshot(self) -> str:
        from rgb_matrix_lib.test_inspect import buffer_fingerprint

        return buffer_fingerprint(self.api)

    def run(self) -> int:
        if self.args.headless:
            install_headless_rgbmatrix()
        sys.path.insert(0, self.args.legacy_root)
        os.chdir(self.args.legacy_root)
        from rgb_matrix_lib.api import get_api_instance

        self.api = get_api_instance()
        self.api.set_drain_checker(self.drain_requested.is_set)
        self.api.set_shutdown_checker(self.force_shutdown.is_set)

        threading.Thread(target=self.data_reader, name="bridge-data", daemon=True).start()
        threading.Thread(target=self.control_reader, name="bridge-control", daemon=True).start()
        ready = {
            "pid": os.getpid(),
            "legacy_root": self.args.legacy_root,
            "headless": bool(self.args.headless),
            "width": self.api.matrix.width,
            "height": self.api.matrix.height,
        }
        self.replies.send(b"H", json.dumps(ready).encode())

        try:
            return self.loop()
        finally:
            self.api.set_drain_checker(None)
            self.api.set_shutdown_checker(None)
            try:
                self.api.cleanup()
            except Exception:  # noqa: BLE001
                pass
            if self.record is not None:
                self.record.close()

    def loop(self) -> int:
        while True:
            if self.force_shutdown.is_set():
                self.blackout_and_exit()
                self.replies.send(b"X")
                return 0
            try:
                kind, payload = self.inbox.get(timeout=0.01)
            except queue.Empty:
                if self.drain_requested.is_set():
                    continue  # wait for the FIFO marker
                try:
                    if not self.api.drain_abort_requested():
                        self.api.pump_fade_display()
                except AttributeError:
                    pass
                continue

            if payload is EOF:
                print("[BRIDGE] producer closed the data pipe; blacking out", flush=True)
                self.blackout_and_exit()
                return 0
            if self.force_shutdown.is_set() or kind == ord("X"):
                self.blackout_and_exit()
                self.replies.send(b"X")
                return 0
            if kind == ord("R"):
                self.script_reset()
                self.log("script_reset")
                self.replies.send(b"R")
                continue
            if kind == ord("K"):
                self.drain_requested.clear()
                self.log("drain", self.swallowed)
                self.replies.send(b"K", struct.pack("<I", self.swallowed))
                self.swallowed = 0
                continue
            if kind == ord("F"):
                self.replies.send(b"F", payload)
                continue
            if kind == ord("T"):
                try:
                    fp = self.snapshot()
                except Exception as exc:  # noqa: BLE001
                    fp = f"error:{exc}"
                self.log("snapshot", fp)
                self.replies.send(b"T", fp.encode())
                continue
            if kind != ord("C"):
                self.replies.send(b"E", f"unknown frame type {kind!r}".encode())
                self.blackout_and_exit()
                return 2
            if self.drain_requested.is_set():
                self.swallowed += 1
                continue

            command = payload.decode("utf-8")
            self.log("cmd", command)
            try:
                self.api.execute_command(command)
            except Exception:  # noqa: BLE001
                msg = f"command failed: {command[:200]}\n{traceback.format_exc()}"
                print("[BRIDGE] " + msg, flush=True)
                self.replies.send(b"E", msg.encode())
                self.blackout_and_exit()
                return 3


def main() -> int:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--legacy-root", default=os.environ.get("PIXIL_LEGACY_ROOT", os.path.join(here, "..", "git")))
    ap.add_argument("--headless", action="store_true", help="stub rgbmatrix (no GPIO); real RGB_Api")
    ap.add_argument("--record", help="JSONL of every command/marker that reached the consumer")
    args = ap.parse_args()
    args.legacy_root = os.path.abspath(args.legacy_root)

    # Shutdown is orchestrated by the producer (control byte X / data EOF).
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    for fd in (DATA_FD, CONTROL_FD, REPLY_FD):
        try:
            os.fstat(fd)
        except OSError:
            print(f"[BRIDGE] fd {fd} not open; must be spawned by the native producer", file=sys.stderr)
            return 2
    return Bridge(args).run()


if __name__ == "__main__":
    sys.exit(main())
