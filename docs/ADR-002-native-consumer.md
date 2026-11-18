# ADR-002 — Native C++ consumer (Phase 2)

* **Status:** Accepted (C0/C1)
* **Date:** 2026-11-17
* **Charter:** `docs/Native_Pixil_Runtime_Phase2_Plan.md`

## Context

Phase 1 drives the panel through `bridge/pixil_consumer_bridge.py`, which imports
legacy Python `rgb_matrix_lib`. The producer is often fast enough that the show is
consumer-bound. Phase 2 replaces that Python consumer with a C++ process behind the
same ADR-001 pipes.

## Decision

1. **Full replacement.** `pixil-consumer` implements the draw API itself. It does not
   call into Python `rgb_matrix_lib` for drawing.
2. **Same IPC.** ADR-001 framing, fds 3/4/5, 5000-command inbox, replies unchanged.
3. **Temporary spawn switch.** The producer may launch either consumer:

   | `--consumer` | Child |
   |---|---|
   | `python` (default) | `python3 -u bridge/pixil_consumer_bridge.py …` |
   | `native` | `pixil-consumer` (same build tree as `pixil`) |

4. **Remove the switch after cutover.** Once the owner switches the panel, delete the
   Python spawn path and `--consumer`. The legacy tree remains a manual fallback only.
5. **Threading.** Reader fills the inbox; control handles `D`/`Q`/`X`; executor draws
   and calls `SwapOnVSync`. No multi-writer canvas. Burnout pumps on the executor.
6. **Wire dialect.** Keep text commands + base64 batches for the first cut.
7. **Headless.** `--headless` never opens GPIO; uses an in-memory canvas.

## Consequences

* Daily show stays on Python until an explicit cutover.
* Parity work compares native fingerprints / behavior to the Python oracle.
* Emergency blackout while the switch exists may still use the legacy Python helper
  when the native child fails to black out.
