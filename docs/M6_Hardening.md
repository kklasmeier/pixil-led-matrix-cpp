# M6 hardening

Run on the Pi 4, 15 Nov 2026. The panel was not used. Producer times are time to emit 3000 commands at seed 1, with no matrix and no queue backpressure.

## Conformance

| Check | Result |
| --- | --- |
| `make test` | 32 tests, 0 failures |
| `make crosscheck` | 220 expression cases, 203 match, 17 documented deviations, 0 unexpected |
| Dry-run smoke of `scripts/main` | 73 scripts, 20 commands each, 0 failures |

The 17 expression differences are D-001 through D-006 in `DEVIATIONS.md`. `Frost.pix` is not in this tree. In the legacy tree it calls `get_frost_color`, which the script never defines.

## Logging and metrics

Phase 1 does not write the legacy metrics database and does not draw the corner `Queue: N` counter.

The operator surface is:

- `pixil.log` when the show is started with `./run start`
- `/dev/shm/pixil_native_status.json` for the current script, its deadline, frame count, command count, and the next script
- `./run status`, `./run next`, `./run jump`, and `./run stop`
- `-d DEBUG_OFF`, `DEBUG_CONCISE`, `DEBUG_SUMMARY`, or `DEBUG_VERBOSE`. Verbose prints each consumer command. It does not trace every interpreter line.
- `-q` prints the script, elapsed time, frames, and commands about once a second

`-q` is a producer progress line. The pipe bridge does not report how many commands are waiting in the consumer, so Phase 1 does not recreate the legacy queue-depth overlay. `throttle(n)` is accepted and the consumer is driven as `throttle(0)` (D-007). Pacing is `fps` and `rest`.

## Producer timing

Native is `pixil --dry-run --max-commands 3000 --seed 1`. Legacy is `tools/capture_legacy_stream.py` with the same limit and seed. Pure-Pixil scripts are the legacy file run by both producers. Engine scripts are the forked file on the native side and the original file on the legacy side.

| Script | Native (s) | Legacy (s) | Legacy / native |
| --- | ---: | ---: | ---: |
| bouncing_ball | 0.019 | 3.889 | 210 |
| KITT_Scanner | 0.210 | 67.639 | 322 |
| Hex_Grid_Glow | 0.298 | 22.932 | 77 |
| Circles | 0.012 | 2.347 | 194 |
| Amiga_Boing_Ball | 0.035 | 5.969 | 173 |
| Tetris (`throttle(0.5)`) | 0.292 | 64.159 | 220 |
| Ink_Storm | 1.031 | 41.560 | 40 |
| Metaballs_Effect | 2.182 | 171.423 | 79 |
| Mandelbrot_Zoom | 4.881 | 122.576 | 25 |
| Cloth_simulation | 1.440 | 27.813 | 19 |

These numbers are producer time. They are not panel frames per second. On the matrix, `fps` and `rest` still cap the show, and the consumer still draws it. Tetris is included because it asks for `throttle(0.5)`. The capture does not apply that delay; the throttle factor lives in the consumer, which neither of these runs starts. A live native Tetris is paced by `fps(60)`.
