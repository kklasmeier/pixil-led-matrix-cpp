# ADR-002 — Native C++ consumer

* **Status:** Accepted; **C6 cutover complete** (native-only)
* **Date:** 2026-11-17 (cutover 2026-11-19)
* **Charter:** `docs/Native_Pixil_Runtime_Phase2_Plan.md`

## Context

Phase 1 drove the panel through `bridge/pixil_consumer_bridge.py` and legacy
Python `rgb_matrix_lib`. Phase 2 replaces that with a C++ consumer behind the
same ADR-001 pipes. After owner A/B on hardware, the dual spawn path is gone.

## Decision

1. **Full replacement.** `pixil-consumer` implements the draw API itself.
2. **Same IPC.** ADR-001 framing, fds 3/4/5, 5000-command inbox, replies unchanged.
3. **Native only.** `pixil` always spawns `pixil-consumer`. Flags `--consumer`,
   `--bridge`, `--python`, and `--legacy-root` were removed at C6.
4. **Emergency blackout.** If the child dies uncleanly, the producer runs
   `pixil-consumer --blackout` (hardware fill + swap). Headless skips that path.
5. **Threading.** Reader fills the inbox; control handles `D`/`Q`/`X`; executor
   draws and calls `SwapOnVSync`. Burnout pumps on the executor (including while
   commands flow).
6. **Wire dialect.** Text commands + base64 batches.
7. **Headless.** `--headless` never opens GPIO; uses an in-memory canvas.

## Consequences

* Daily show is native producer + native consumer (`./run`).
* The old Python bridge directory is deleted from this tree.
* Expression oracles / goldens that use CPython for language parity remain
  optional tooling; they do not drive the panel.
