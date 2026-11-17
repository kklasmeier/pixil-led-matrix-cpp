# pixil-led-matrix-cpp

Native (C++) **Pixil language runtime**: the Phase 1 producer for the LED matrix show stack.

## Purpose

Pixil scripts (`.pix`) stay the product. This repo replaces the Python producer
(`Pixil.py`) with a compile-once C++ runtime that feeds the **existing Python**
`rgb_matrix_lib` consumer from the legacy tree (see [ADR-001](docs/ADR-001-ipc.md)).

Phase 2 (native consumer) is later and out of scope until Phase 1 is solid.

## Charter and contracts

* [docs/Native_Pixil_Runtime_Phase1_Plan.md](docs/Native_Pixil_Runtime_Phase1_Plan.md): charter. Read it first.
* [docs/ADR-001-ipc.md](docs/ADR-001-ipc.md): producer ↔ consumer IPC (pipes + a thin Python bridge).
* [docs/LANGUAGE.md](docs/LANGUAGE.md): language reference and EBNF draft.
* [docs/COMMAND_DIALECT.md](docs/COMMAND_DIALECT.md): what the producer sends to the consumer.
* [DEVIATIONS.md](DEVIATIONS.md): every known difference from legacy behaviour.
* [docs/OPS.md](docs/OPS.md): build, run, blackout and capture procedures.

## Layout on the Pi

| Path | Role |
|------|------|
| `~/Lightshow/git/` | **Legacy** Python Pixil + `rgb_matrix_lib` (oracle and consumer host; read-only from here) |
| `~/Lightshow/pixil-led-matrix-cpp/` | **This repo** |

The dev machine (Pi 5) mounts the Pi 4's `~/Lightshow` at
`/home/pi/Programming/piMatrix/Lightshow/`. **Edit** on the Pi 5 via NFS, and
**build and run** on the Pi 4 via SSH (never compile through NFS):

```bash
ssh pi@192.168.1.58 'cd ~/Lightshow/pixil-led-matrix-cpp && nice -n 19 make -j2 && make test'
```

Requirements: g++ ≥ 12 (C++20) and GNU make. Python 3 with the legacy tree's
dependencies is needed for the bridge, the oracles and the capture tools.

## Source tree

| Path | Contents |
|---|---|
| `src/runtime/` | Values and Python-exact operators, float repr, CPython-compatible MT19937, evaluator |
| `src/syntax/` | Lexer, Pratt parser, AST (compile-once) |
| `src/emit/` | `draw_batch` packer (byte-exact with the legacy protocol), color ids, base64 |
| `src/ipc/` | `ConsumerLink`: spawns and drives the consumer bridge |
| `src/host/` | `pixil` CLI |
| `src/tools/` | `pixil-expr` (Tier 0 expression runner) |
| `bridge/` | `pixil_consumer_bridge.py`: thin server around legacy `rgb_matrix_lib` |
| `tools/` | Legacy oracles, cross-check, golden generators, stream capture |
| `tests/unit/` | Unit tests (no external framework) |
| `tests/expr/` | Tier 0 expression cases plus expected deviations |
| `tests/golden/` | `draw_batch` goldens generated from the legacy protocol |
| `tests/streams/` | Seeded legacy command-stream captures (oracle for M3) |

## Status

* **M1 (contracts and parity tooling):** done.
  * Expression oracle cross-check: 220 cases, all either matching or documented in DEVIATIONS.
  * `draw_batch` goldens: 733 cases, byte-exact.
  * Legacy stream capture tool, seeded and deterministic.
  * Docs drafted.
* **M2 (hello matrix):** `sudo build/release/pixil --hello` drives the physical
  panel through the legacy consumer via ADR-001. It holds 30 fps through
  `draw_batch`; drain, reset, snapshot and blackout drills pass, both on
  hardware and headless.
* **M3 (language core), in progress:** the first 3000 consumer commands match the
  legacy stream for `bouncing_ball.pix`, `KITT_Scanner.pix`, `Hex_Grid_Glow.pix`,
  `Abstract_Geometry.pix`, `snake.pix`, `Atom_Viewer.pix`,
  `Radioactive_Decay.pix`, `Digital_Rain.pix`, `Pong.pix`,
  `Amiga_Boing_Ball.pix`, and the forked `Cloth_simulation.pix`.
  `Circles.pix` and `Fifteen_Puzzle.pix` match except
  for the accepted D-008 and D-009 differences.
  Procedures, numeric and string arrays, bare commands, `while`,
  `draw_text` / `clear_text`, sprites (including `sprite_cel`), `print`
  (including f-strings), `get_datetime`, `import`, and calls with arguments
  are in. `import particle` loads `scripts/lib/particle.pix` and registers
  its procedures as `particle.verlet`. A path such as `import arcade/ai`
  loads `scripts/lib/arcade/ai.pix` and uses the file name as the prefix.
  The particle library covers integrate, attractors, verlet, distance
  constraints, springs, flocking, and the circle and wall collisions.
  The 32 particle-only shows are forked under `scripts/main/`; 28 of them
  match the legacy stream for 800 commands, and 25 of those still match at
  3000. `Boids_Flocking_Simulation`, `Elastic_Collision`, and
  `Murmuration_Panic` cross one pixel after a few hundred frames.
  `Boid_Digits` and `Hyperspace_Transition` read the wall clock, so a seeded
  capture cannot match them. `Gravity_Flip` and `Gravity_Wells` match for 29
  and 61 frames, then one plotted point sits on the other side of a pixel.
  The points library covers `points.project3d` and `points.lerp`. The nine
  shows that use them are forked under `scripts/main/` and match the legacy
  stream for 3000 commands. `draw_lines` and `move_sprites` expand into the
  frame batches. Grid and field programs compile in the producer:
  `grid_fill`, `grid_reset`, `grid_step`, and `field_render`. Twenty shows
  that use those, including `Magnetic_Pendulum`, `Soap_Bubbles`, and
  `Twin_Star_Vortex`, match the legacy stream for 3000 commands. Agents are
  `scripts/lib/agent.pix`. `Ember_Moths` and `Slime_Mold_Highways` match
  for 3000 commands. `Tide`, `Iron_Filings`, `Slime_Mold`, `Ink_Storm`,
  `Whirlpool`, and `Wildfire` call `random` inside an `if`. Their first
  differences are not yet confirmed as that effect. `Lightning_Roots` and
  `Coral_Rim` are not diagnosed. Ink is `scripts/lib/ink.pix`;
  `Ink_In_Water` matches for 3000 commands. Chladni is
  `scripts/lib/chladni.pix`. Each step draws from a fresh NumPy Generator,
  seeded the way a capture seeds an unseeded `default_rng()`, and
  `Chladni_Patterns` matches for 3000 commands. Of the scripts that do not
  call an effect engine, 137 run; `Frost.pix` calls `get_frost_color`,
  which the script never defines. `./run` plays one script or a `main/*`
  playlist for `-t mm:ss` per script, and `./run stop` blacks the panel out.
  Day-to-day use and the fallback to the legacy producer are in `docs/CUTOVER.md`.
  The M6 conformance run and the producer timing comparison are in `docs/M6_Hardening.md`.
  See `docs/OPS.md`.

## Legacy reference

- Scripts: `~/Lightshow/git/scripts/`
- Language guide: `~/Lightshow/git/docs/Pixil_Scripting_Guide.txt`
- Consumer: `~/Lightshow/git/rgb_matrix_lib/`
