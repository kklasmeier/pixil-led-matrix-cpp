# ADR-001 — IPC between the native producer and the legacy consumer

* **Status:** Accepted (Phase 1)
* **Date:** 2026-11-14
* **Charter:** `docs/Native_Pixil_Runtime_Phase1_Plan.md` §6.1 (option B recommended), §6.5 (pacing)

## Context

Phase 1 replaces the Pixil *producer* (language, playlist, timers) with native
code while the LED *consumer* stays the legacy Python `rgb_matrix_lib`
(`RGB_Api` + `CommandExecutor`). Today the two halves talk through
`shared/command_queue.py`: a `multiprocessing.Queue` of `(command, delay)` tuples
plus `multiprocessing` events for drain, shutdown, script reset and test
snapshots. That transport is Python-shaped; C++ cannot join it.

Options considered (charter §6.1):

| | Option | Verdict |
|---|---|---|
| A | Spawn the legacy consumer via the existing `command_queue` protocol | Requires a Python producer-side half anyway; inherits multiprocessing failure modes |
| **B** | **Native producer → pipes → thin Python consumer server wrapping `CommandExecutor`** | **Chosen** |
| C | Embed CPython (pybind) | Tight coupling; complicates Phase 2 consumer replacement |

## Decision

The native `pixil` binary spawns `bridge/pixil_consumer_bridge.py` (lives in
this repo; **no legacy edits**), which imports the legacy `rgb_matrix_lib` from
`--legacy-root` and runs a loop equivalent to `MatrixCommandQueue._consumer_loop`.

### Channels

Three anonymous pipes, inherited as fixed fds in the bridge:

| fd | Direction | Purpose |
|---|---|---|
| 3 | producer → bridge | **data**: strict FIFO of framed records |
| 4 | producer → bridge | **control**: single out-of-band bytes, read by a separate thread |
| 5 | bridge → producer | **replies** |

Frame = `type:u8` + `length:u32 LE` + payload.

| Data frame | Meaning | Reply |
|---|---|---|
| `C` | UTF-8 consumer command (`clear`, `draw_batch("…")`, …) | — |
| `F` | fence, `u32` id | `F` with the same id after all earlier frames ran |
| `K` | drain marker (FIFO) | `K` + `u32` commands swallowed |
| `R` | script reset | `R` |
| `T` | test snapshot | `T` + fingerprint |
| `X` | graceful shutdown: finish queue, blackout, exit | `X` |

| Control byte | Meaning |
|---|---|
| `D` | drain requested: swallow `C` frames until the `K` marker; interrupts `rest` |
| `X` | force shutdown: blackout immediately (interrupts `rest` and backlog) |

Bridge-initiated replies: `H` (ready, JSON: pid, size, headless) once
`RGB_Api` is up; `E` (command failed; text) — the bridge then blacks out and exits.

### Semantics preserved from `command_queue.py`

* **Backpressure:** the bridge reader thread feeds a bounded queue (5000, like
  `MatrixCommandQueue`); when it is full the pipe fills and the producer's
  non-blocking write waits in `poll` (still watching replies and SIGINT).
* **Drain:** `RGB_Api.set_drain_checker` / `set_shutdown_checker` are wired to the
  control-plane events, so long `rest`s and fades abort exactly as today.
* **Idle:** with nothing queued the bridge calls `pump_fade_display()` (burnout fades).
* **Script reset:** `reset_fps`, end open frame, `clear`, `dispose_all_sprites`.
* **Blackout guarantee:**
  * graceful `X` / force `X` → `blackout_display()` in the bridge;
  * producer death (data pipe EOF) → bridge blacks out and exits;
  * bridge death or unresponsive bridge → producer kills it and runs
    `rgb_matrix_lib.emergency_blackout.run_emergency_blackout()` in a fresh
    Python process;
  * SIGINT is ignored by the bridge; the producer turns SIGINT/SIGTERM into a
    force shutdown.
* **Pacing:** no per-command delay field. Phase 1 drives the consumer as
  `throttle(0)` (charter §6.5, DEVIATIONS D-007).

### Headless mode

`--headless` installs a stub `rgbmatrix` module before import, so the **real**
`RGB_Api`/`CommandExecutor` run against an in-memory canvas. Drawing-buffer
fingerprints (`T`) therefore work without hardware, and `--record FILE` logs every
command the consumer executed (JSONL). Used for CI-style smoke tests and for
running beside a live show on the Pi.

## Consequences

* The legacy tree is untouched; the consumer is exactly today's code.
* Commands cross as text (same as today), so Phase 1 parity can be checked by
  comparing command streams (`tools/capture_legacy_stream.py`) — no binary
  protocol to validate yet. Binary frames are Phase 2 territory.
* One extra Python process (the bridge) replaces the `multiprocessing` consumer;
  startup cost is the same `RGB_Api` init.
* Recovery (`ConsumerRecoveryRequired` + restart) is simpler: bridge death is
  visible as pipe EOF/`E`, and the producer owns restart policy (M4+).
* Phase 2 can replace the bridge with a native consumer behind the same framing.

## Verification

* `pixil --hello --headless` exercises every frame type: fences, a drain that
  must interrupt `rest(10)` and swallow a 200-command backlog, script reset, the
  fingerprint snapshot and graceful shutdown.
* Failure drills (run on the Pi 4, headless): SIGINT mid-animation, then
  blackout and exit 130; bridge `kill -9`, then the producer errors and runs the
  emergency blackout (skipped headless); producer `kill -9`, then the bridge
  blacks out on data EOF.
* M2 gate: `sudo build/release/pixil --hello` on the physical panel.
