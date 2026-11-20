# pixil-led-matrix-cpp

Native (C++) **Pixil** runtime for the LED matrix show: producer (`pixil`) and
consumer (`pixil-consumer`).

## Purpose

Pixil scripts (`.pix`) are the product. This repo runs them end-to-end without
the legacy Python `Pixil.py` / `rgb_matrix_lib` stack. IPC between producer and
consumer is [ADR-001](docs/ADR-001-ipc.md); the consumer design is
[ADR-002](docs/ADR-002-native-consumer.md).

## Docs

* [docs/OPS.md](docs/OPS.md): build, run, status, blackout
* [docs/CUTOVER.md](docs/CUTOVER.md): daily show
* [docs/LANGUAGE.md](docs/LANGUAGE.md): language reference
* [docs/COMMAND_DIALECT.md](docs/COMMAND_DIALECT.md): producer → consumer commands
* [DEVIATIONS.md](DEVIATIONS.md): known semantic differences vs historical Python Pixil
* Phase plans (historical): Phase 1 / Phase 2 under `docs/`

## Layout on the Pi

| Path | Role |
|------|------|
| `~/Lightshow/pixil-led-matrix-cpp/` | **This repo** (edit via NFS from Pi 5; **build/run on Pi 4**) |

```bash
ssh pi@192.168.1.58 'cd ~/Lightshow/pixil-led-matrix-cpp && nice -n 19 make -j2 && make test'
```

Requirements: g++ ≥ 12 (C++20), GNU make, and `librgbmatrix` for hardware
(headless builds work without it). Python 3 is optional for expression
cross-checks / golden regenerators only — not for the live show.

## Source tree

| Path | Contents |
|---|---|
| `src/runtime/` | Values, operators, RNG, evaluator |
| `src/syntax/` | Lexer, parser, AST |
| `src/emit/` | `draw_batch` packer, color ids |
| `src/ipc/` | `ConsumerLink` → spawns `pixil-consumer` |
| `src/host/` | `pixil` CLI |
| `src/consumer/` | `pixil-consumer` |
| `src/tools/` | `pixil-expr` |
| `tools/` | Cross-check, golden generators, ops smoke |
| `scripts/main/` | Show library |
| `fonts/` | Bitmap fonts (e.g. tiny64) |

## Daily show

```text
./run start
./run status
./run next
./run stop
```

See `docs/OPS.md` and `docs/CUTOVER.md`.
