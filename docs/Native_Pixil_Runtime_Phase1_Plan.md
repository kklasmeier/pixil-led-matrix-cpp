# Native Pixil Runtime — Phase 1 Plan (Handoff)

**Status:** Design / planning only. Not started.  
**Date:** 2026-11-14 (updated same day with independent review inputs)  
**Source project (legacy):** `piMatrix/Lightshow/git` (Python Pixil producer + `rgb_matrix_lib` consumer)  
**Target:** New GitHub repository + separate directory on the Raspberry Pi  
**Audience:** Future implementation session (new repo / new Cursor project)

---

## 0. Decisions already made

| Decision | Choice |
|----------|--------|
| Product goal | Pixil as a **real language runtime** for pure `.pix` drawing/animation |
| Purity stance | Prefer language speed over growing domain primitives; existing primitives are **compat**, not the future vocabulary |
| Migration shape | **Phase 1:** replace producer. **Phase 2 (later):** replace consumer |
| Phase 1 consumer | Keep legacy **Python `rgb_matrix_lib`** (same machine, same LED stack) |
| Isolation | **New GitHub repo** + **separate Pi directory**; legacy project stays until deliberately deprecated |
| Bottleneck evidence | ~45% of long-run scripts are producer-bound (queue wait &lt;10%); ~38% already consumer-bound; Option A shipped on legacy consumer |
| Interpreter design | **Compile once, run many** — do **not** transliterate Python’s re-parse-every-line model into C++/Rust |
| Spec oracle | Where Scripting Guide and Python disagree, **Python behavior wins** unless owner overrides; log in `DEVIATIONS.md` |
| No human code review | Parity tests + sanitizers are the review process (§8, §8.4) |
| **`throttle` pacing** | **Accept** `throttle(...)` in `.pix` (parity). **Ignore replay pacing:** native producer always sends **`throttle(0)`** (or equivalent) to the Python consumer so scripts are paced by `fps` / `rest` / frames / queue backpressure. Remove `throttle` entirely in Phase 2 consumer. |
| **Implementation language** | **C++** (default) — same family as `rpi-rgb-led-matrix`, simpler one-language story into Phase 2; rely on sanitizers (§8.4). Rust remains acceptable if a future session prefers memory safety over familiarity. |
| **Legacy freeze** | **Yes** — no new language features or primitives on the legacy tree during the migration |
| Remaining owner questions | See §13 (RNG, ops shell, hardware targets, etc.) |

### Why a separate repo and directory

- Legacy keep running (`~/Lightshow/git`) while native is built.
- No mixed Python/C++ monster in one tree during the risky period.
- Clear cutover: `./run` in legacy vs `./run` (or binary) in the new tree.
- Deprecate only when native can run the show library against the same (or later native) consumer.
- Handoff to another AI session is cleaner with a greenfield repo + this document as the charter.

**Repository:** https://github.com/kklasmeier/pixil-led-matrix-cpp  

**Suggested Pi layout (on the matrix Pi 4, exported over NFS):**

```text
~/Lightshow/git/                     # LEGACY — do not break; reference + consumer host
~/Lightshow/pixil-led-matrix-cpp/    # NEW — native producer (this repo)
```

**Dev topology (owner):**

| Machine | Role |
|---------|------|
| **Pi 5** (Cursor) | Edit only — NFS client |
| **Pi 4** `192.168.1.58` | Source of truth disk + **build** + **run** (matrix) |

NFS mount (already in use for legacy):

```text
Pi 4:  /home/pi/Lightshow
Pi 5:  /home/pi/Programming/piMatrix/Lightshow
```

### Build / test rule (locked)

1. **Edit** on Pi 5 through the NFS mount (same as today).  
2. **Compile on the Pi 4 via SSH**, in the real path (`~/Lightshow/pixil-led-matrix-cpp`), so `.o` files hit **local disk**, not NFS.  
3. **Run** on the Pi 4 (matrix + legacy consumer).  

**Do not** run `cmake --build` on the Pi 5 against the NFS tree — compile I/O over NFS is slow and brittle.

**Optional later:** a second clone on Pi 5’s local SSD for desktop-only unit tests (faster CPU). That is optional; Phase 1 default is **one tree on Pi 4, SSH build**.

Both boards are `aarch64`. If ever compiling on Pi 5 for a binary that runs on Pi 4, target **`cortex-a72` / `armv8-a`** (not Pi 5-tuned `-mcpu`) and match shared library versions — prefer SSH-on-Pi-4 to avoid that class of bugs.

---

## 1. Problem statement

Pixil scripts are the product. Authors should express animations in `.pix` without escaping to hand-tuned Python or ever-growing native verbs.

The current producer (`Pixil.py` + `pixil_utils`, ~16k+ lines) interprets that language in Python. Micro-optimizations (fast paths, compiled loops, flags) are near exhausted. Domain engines (field/grid/particles/…) were added as pressure valves; they help FPS but dilute language purity.

**Phase 1 success:** a native producer executes the same Pixil language, emits the same (or documented-compatible) command stream, drives the **existing** Python consumer, and is fast enough that pure `.pix` is viable on a Pi 4 without new primitives.

### 1.1 Root cause (why Python opts plateaued)

1. **Source text is re-processed on hot paths.** Line dispatch + regex + string substitution + `eval()` means loop bodies pay parse cost repeatedly (mitigated unevenly by compilers/caches).
2. **Python workarounds paper over (1)** — fast paths, JIT, condition templates, loop compiler fallbacks. Do not port them; replace the design.
3. **Commands as text** are formatted in the producer and parsed again in the consumer (batches help; Option A helped consumer parse).
4. **CPython on Pi ARM** has high per-op cost for scalar interpreter work.

**Implication:** a line-by-line C++ translation of `process_lines()` would waste most of the opportunity. The win is **parse/compile once → VM**, in a native language.

### 1.2 Expected improvement (order of magnitude)

| Workload | Likely producer speedup | Visible panel FPS |
|----------|-------------------------|-------------------|
| Pure `.pix` loops/math/draws | Often **~5–15×** | Large until consumer wall |
| Engine scripts (NumPy today → native engines) | **~1.5–3×+** if engines ported well | Often still draw-capped |
| Already consumer-bound (~38%) | Producer faster | Little FPS change |

Phase 1 also **proves language parity**. Full end-to-end speed needs Phase 2 for consumer-bound shows.

### 1.3 Corpus feature frequency (prioritize core)

Approx. share of `scripts/main` (~217) using each (independent inventory):

`fps` ~all · `while`/`for` ~all · `begin_frame` ~90% · `create_array` ~80% · `random` ~80% · procedures ~50% · `rest` ~40% · sprites ~25% · `particle_*` ~18% · text ~13% · backgrounds ~11% · `grid_program` ~7% · `points_*` ~7% · `field_program`/`agent_*`/`ink_*` ~5% · `chladni` rare.

Also: `%` heavily used (~200+ lines); `throttle` in many scripts (most often `throttle(0)`).

---

## 2. Architecture (unchanged product shape)

```text
.pix scripts
    → Producer (TODAY: Python Pixil.py | PHASE 1: native runtime)
         → command queue / IPC
    → Consumer (PHASE 1: legacy rgb_matrix_lib in Python)
         → rpi-rgb-led-matrix → 64×64 panel
```

**Frozen for Phase 1:** consumer behavior, LED hardware path, `.pix` author-facing language (see contracts).  
**Replaced for Phase 1:** entire producer process responsibilities (not only the eval loop).

### Phase 2 (out of scope for this document’s build work)

Replace Python consumer / possibly binary IPC. Only after Phase 1 is boringly solid. Not required to prove the language thesis.

---

## 3. Legacy reference map (read-only dependency)

Implementation should treat the legacy tree as **spec by behavior**, not copy-paste forever.

| Area | Legacy location |
|------|-----------------|
| Interpreter / main loop | `Pixil.py` (~2900 lines) |
| Language utilities | `pixil_utils/` (~13k lines) |
| CLI | `pixil_utils/cli.py` |
| Script discovery / playlist | `pixil_utils/script_manager.py`, `file_manager.py` |
| Command param schema | `pixil_utils/parameter_types.py` (`PARAMETER_TYPES`, 50 commands) |
| Math | `pixil_utils/math_functions.py` |
| Conditions | `pixil_utils/condition_templates.py` |
| Loops / procedures (Python opts) | `pixil_utils/loop_compiler.py` — **do not port; replace with native** |
| JIT | `pixil_utils/jit_compiler/` — **do not port** |
| Batch pack (producer) | `pixil_utils/draw_batch_dispatch.py`, `sprite_batch_dispatch.py` |
| Wire protocols | `shared/draw_batch_protocol.py`, `mplot_protocol.py`, `sprite_batch_protocol.py` |
| Queue / consumer spawn | `shared/command_queue.py` |
| Consumer | `rgb_matrix_lib/` (**keep running from legacy for Phase 1**) |
| Script language docs | `docs/Pixil_Scripting_Guide.txt` |
| Dev / file map | `docs/Pixil_Development_Guide.txt` |
| Show library | `scripts/main/*.pix` (~218 scripts) |
| Ops entry | `./run` |
| Tier 1 tests | `tests/pixil/` |
| Tier 2 script smokes | `tests/scripts/` |

**Pi hardware reference:** matrix at `pi@192.168.1.58`, legacy path `~/Lightshow/git`.

---

## 4. Phase 1 scope — full producer migration

Phase 1 migrates **all producer roles**, not “interpreter only.”

### In scope

1. **Language runtime** — lex/parse or line model matching today’s semantics; variables (`v_`), arrays, math, control flow, procedures, `print`, timers
2. **Command emission** — all draw/text/sprite/background/perf commands → queue strings the consumer already understands; `draw_batch` / `plot_batch` / `sprite_batch` packing
3. **Script I/O** — load `.pix`, path rules, `.pix` extension optional
4. **Script management** — single script, wildcards (`main/*`), duration `-t mm:ss`, playlist advance, jump/next/recovery skip parity with `./run`
5. **Host/CLI** — args compatible with today’s Pixil CLI; signals; shutdown; headless
6. **Logging / status / metrics** — lifecycle logging; debug levels; queue monitor `-q`; performance metrics (parity or versioned schema)
7. **Queue client** — enqueue commands; throttle/sync/drain semantics; **start or attach to legacy Python consumer**
8. **Primitives (compat)** — enough of field/grid/particle/points/agent/ink/chladni so existing `main/*` that use them still run; **no new primitive verbs**

### Explicitly out of scope (Phase 1)

- Rewriting `rgb_matrix_lib`
- Changing LED hardware config as a project goal
- Porting Python JIT / expression-cache / optimization flag matrix
- Expanding the primitive vocabulary
- Editing hundreds of `.pix` files “to be native-friendly”
- Deprecating or deleting the legacy repo

### Purity policy during Phase 1

- **Acceptance for speed:** pure Pixil (loops/math/draws) gets faster without new engines.
- **Primitives:** port for compatibility; document as transitional; do not add new ones in the native project unless the owner reopens that policy.

---

## 5. Contracts (acceptance gates)

Three contracts must be written down and tested. Prefer extracting tables from legacy rather than reinventing.

### 5.1 Language contract

Source of truth: `docs/Pixil_Scripting_Guide.txt` + behavioral tests from `tests/pixil` + representative `.pix`.

Minimum surface:

- [ ] Line-oriented scripts; comments `#`
- [ ] Variables `v_*`; assignments; strings in quotes
- [ ] Expressions / math catalog (match `math_functions.py` inventory)
- [ ] `if` / `elseif` / `else` / `endif`; `while` / `endwhile`; `for` / `endfor`
- [ ] `def` / `call` / procedures
- [ ] Arrays: `create_array`, indexing, numeric vs other modes as today
- [ ] `random(...)`, timers / script duration interaction
- [ ] `print(...)` + `sync_queue` expectations
- [ ] Frame model: `begin_frame` / `end_frame`, preserve flag
- [ ] Error behavior: what fails soft vs hard (document from legacy)

**Deliverable artifact:** `LANGUAGE.md` in the new repo + conformance `.pix` suite.

### 5.2 Command dialect contract (producer → consumer)

Consumer parses text commands (plus Option A fast-path for `draw_batch`/`plot_batch` in legacy).

Commands registered in `PARAMETER_TYPES` today (50):

```
plot, mplot, mflush, begin_frame,
draw_line, draw_lines, draw_rectangle, draw_circle, draw_ellipse, draw_polygon, draw_arc,
draw_text, clear_text, clear,
define_sprite, sprite_cel, sprite_draw, show_sprite, hide_sprite, move_sprite, move_sprites,
dispose_sprite, dispose_all_sprites,
set_background, hide_background, nudge_background, set_background_offset,
fps, rest, throttle, sync_queue,
grid_step, grid_fill, field_render, chladni_step, ink_step, slime_step,
particle_integrate, particle_verlet_integrate, particle_collide_bounds,
particle_collide_circle_bounds, particle_collide_circles, particle_collide_static_circles,
particle_constrain_distances, particle_apply_attractors, particle_apply_springs, particle_flock,
points_project3d, array_lerp, agent_chemotaxis
```

Plus producer-generated (not always in PARAMETER_TYPES as author verbs):

- [ ] `end_frame`
- [ ] `draw_batch("<base64>")`
- [ ] `plot_batch("<base64>")`
- [ ] `sprite_batch("<base64>")`
- [ ] Control: `__SCRIPT_RESET__`, `__DRAIN__`, `__SHUTDOWN__`, `__test_snapshot__` as required for ops/tests

**Wire format rules to preserve in Phase 1:**

- [ ] Frame flush order: `begin_frame` → batches → other frame cmds (e.g. `draw_text`) → consumer `end_frame` (match `Pixil.py` `flush_frame_commands`)
- [ ] Record layouts: `shared/draw_batch_protocol.py`, `mplot_protocol.py`, `sprite_batch_protocol.py`
- [ ] Colors / intensity / burnout / burnout_mode encoding
- [ ] Off-screen clipping behavior before pack (legacy skips/clips)

**Deliverable artifact:** `COMMAND_DIALECT.md` + golden command-stream captures from legacy for N scripts.

### 5.3 Host / ops contract

Match what `./run` and Pixil CLI do today enough that show operation feels the same:

- [ ] `Pixil <script|pattern> [-t mm:ss] [-d DEBUG_*] [-q]`
- [ ] Script paths relative to scripts dir; optional `.pix`
- [ ] Wildcard playlist; per-script duration
- [ ] `next` / `jump` / stop / status behaviors if provided via `./run` + runtime control files
- [ ] Starts or attaches consumer so the panel lights without a second manual process
- [ ] Clean shutdown / blackout behavior compatible with legacy expectations

**Deliverable artifact:** `OPS.md` + “day in the life” checklist on the Pi.

---

## 6. Design — Phase 1 native producer

### 6.1 Process model (recommended)

```text
[native pixil binary]
   - load/run .pix
   - emit commands
   - manage playlist / timers / logging
   - IPC to consumer

[python consumer worker]   # from LEGACY tree initially
   - rgb_matrix_lib
   - same as today
```

**IPC options (choose early, document in ADR):**

| Option | Pros | Cons |
|--------|------|------|
| **A. Spawn legacy consumer** via existing `command_queue` protocol (bridge in C++ or small Python helper) | Fastest path to pixels | Multiprocessing/queue is Python-shaped |
| **B. Native producer → Unix socket / pipe → thin Python consumer server** wrapping `CommandExecutor` | Cleaner language boundary | Small new shim in legacy or new repo |
| **C. Embed Python consumer** (pybind) | One process | Tight coupling; harder later Phase 2 |

**Recommendation:** **B** (socket/pipe / stdin framed records + thin Python consumer daemon wrapping today’s `_consumer_loop` behavior). Avoid embedding CPython or pretending C++ can attach to `multiprocessing.Queue`. Record the choice in `docs/ADR-001-ipc.md`.

**Control-plane semantics to preserve** (failure modes documented in `shared/command_queue.py` — treat as a checklist, not code to copy):

- [ ] `__SCRIPT_RESET__` + acknowledgment between playlist scripts
- [ ] `__DRAIN__` FIFO fence + discard pending on skip/time-up
- [ ] `__SHUTDOWN__` / force-shutdown → **panel blackout guarantee** (`emergency_blackout`)
- [ ] `__test_snapshot__` buffer fingerprint for Tier-2-style tests
- [ ] `sync_queue` / wait-until-empty
- [ ] Optional: consumer heartbeat / recovery signaling
- [ ] Per-command **delay** field until pacing model is decided (§6.5)

### 6.2 Interpreter pipeline (required design)

```text
Lexer → Parser → AST → semantic resolve (names→slots, commands→opcodes)
      → run via tree-walker VM  (start here)
      → optional bytecode later if benchmarks demand it
```

- Parse each script **once** at load; compile `def` / `grid_program` / `field_program` bodies once.
- `v_*` → slot indices; arrays → typed vectors.
- Built-ins resolve at compile time (no string command lookup in the hot loop).
- AST nodes carry **line numbers** for errors.
- **Write a formal grammar** (EBNF) from Scripting Guide + `regex_patterns.py` + `process_lines` dispatch order; prove the corpus parses before claiming language done.
- `grid_expr` / field mini-language: **same** parser/AST stack, not a second hand-rolled dialect.

### 6.3 Internal native modules (suggested layout)

```text
pixil-native/
  README.md
  docs/
    Native_Pixil_Runtime_Phase1_Plan.md  # this charter (copy)
    LANGUAGE.md
    COMMAND_DIALECT.md
    OPS.md
    ADR-*.md
  crates/ or src/
    runtime/      # values, env, eval
    syntax/       # parse .pix
    emit/         # commands + batch packers
    host/         # CLI, playlist, timers, signals
    ipc/          # talk to consumer
    engines/      # compat primitives (optional isolated crate)
  tests/
    conformance/  # .pix + expected traces
    unit/
  tools/
    capture_legacy_stream.py  # run against legacy to record goldens (lives here or called from legacy)
```

Language of implementation: **C++** (locked default — easier continuity with the LED stack and Phase 2). Sanitizers required (§8.4).

### 6.4 What not to design like Python

- Do **not** port `optimization_flags.py`, JIT, condition templates, or expression cache.
- Do **not** recreate “compiled loop vs interpreter” dual paths — native loops should just be fast.
- Do **not** make engines the center of the architecture; keep them as compat built-ins.
- Do **not** transliterate `process_lines` regex cascades into C++.

### 6.5 Pacing / `throttle` (owner decision — locked)

Today `MatrixCommandQueue.put_command()` attaches a **delay** ≈ *(wall time since previous command) × 0.7 × throttle_factor*. The consumer **sleeps that delay before executing**, so it partly **replays producer slowness**. A faster producer would therefore change how `throttle > 0` scripts feel.

**Locked policy:**

1. **Language:** still **accept** `throttle(...)` in `.pix` (do not syntax-error old scripts).
2. **Consumer during Phase 1:** native producer **always effectively uses `throttle(0)`** toward `rgb_matrix_lib` (send `throttle(0)` at script start and/or ignore non-zero script values when emitting). Pacing = `fps` / `rest` / frame boundaries / queue backpressure.
3. **Phase 2:** `throttle` may be removed from the consumer entirely; language can keep a no-op or drop later with owner OK.

**Visual note:** scripts that depended on slow-interpreter + `throttle > 0` replay may look different (usually snappier). Spot-check a few after M3; that difference is **accepted**, not a regression against this policy.

### 6.6 Python semantic hazards (silent parity bugs)

Expressions today go through Python `eval()`. Native must match these behaviors (unit-test each):

| Hazard | Python | Naive native footgun | Risk |
|--------|--------|----------------------|------|
| `%` negatives | Floor mod: `-1 % 64 == 63` | C `fmod` → `-1` | **Very high** (wrap-around) |
| `/` | Always float div | Int div if both ints | High |
| `round` | Banker's rounding | `std::round` half-away | Medium |
| `**` | Exponent | No operator | Low (rare) |
| `and`/`or` | Short-circuit; value semantics | Bool-only | Check corpus |
| Truthiness | `0` / `""` false | Must define | High |
| Float `str`/`print` | `3.0`, `0.1`, … | `3.000000` | Visible |
| `random` | MT + custom decimals | Different RNG | Parity testing |
| `get_datetime` tokens | Custom format table | Must reimplement | Medium |
| Math catalog | See `test_math_catalog.py` | Missing fn / domain | High |

Deliverable: expression cross-check harness (Python oracle vs native) over `tests/pixil` math/condition cases + corpus-extracted expressions. Keep `DEVIATIONS.md` for intentional differences.

### 6.7 Relationship to legacy Option A

Legacy consumer already fast-paths `draw_batch`/`plot_batch` parsing. Phase 1 native producer can keep emitting base64 text batches for compatibility. Binary frames = Phase 2 territory.

---

## 7. Build plan (milestones)

Each milestone leaves the LED matrix demonstrably better or the contract stronger. Prefer vertical slices.  
**Parity infrastructure precedes a large interpreter** (M1 before deep M3).

### M0 — Repo bootstrap

- [ ] Create GitHub repo; clone to `~/Lightshow/pixil-native` on Pi
- [ ] Copy this plan into `docs/`
- [ ] README: purpose, non-goals, how to point at legacy consumer
- [ ] ADR-001: IPC choice (pacing/language/freeze already locked in §0)
- [ ] Tooling: build on Pi 4 + desktop; smoke binary `--help`
- [ ] CI skeleton with sanitizers (§8.4)

### M1 — Contracts + parity tooling (before big runtime)

- [ ] `LANGUAGE.md` + EBNF grammar draft; corpus parse attempt list
- [ ] `COMMAND_DIALECT.md` from `PARAMETER_TYPES` + batch protocols
- [ ] Legacy hooks (prefer small legacy-side env flags, keep oracle runnable):
  - [ ] `PIXIL_SEED` (or equivalent) for determinism
  - [ ] Command-stream recorder (text + decoded batches + frame markers)
  - [ ] Optional: headless consumer path for desktop stream capture
- [ ] Capture streams for 5–10 scripts (light + heavy + `throttle>0` sample)
- [ ] Expression oracle harness scaffold
- [ ] `DEVIATIONS.md` started

### M2 — Hello matrix (native → legacy consumer)

- [ ] IPC works; `clear` + `sync_queue` + simple `draw_rectangle` + `rest`
- [ ] Manual: one hardcoded demo without full language

**Gate:** panel shows something driven by native process, consumer still Python from legacy tree.

### M3 — Language core

- [ ] Variables, arithmetic, `plot`/`draw_*`, `for`/`while`/`if`, `rest`/`fps`/`throttle`
- [ ] `begin_frame`/`end_frame` + `draw_batch` packing
- [ ] Run `bouncing_ball.pix`, `KITT_Scanner.pix` (or trimmed cousins) unchanged

**Gate:** pure-Pixil demos run; queue depth / timing shows producer improvement vs legacy on the same scripts.

### M4 — Host parity

- [ ] CLI flags; duration; debug levels; wildcard playlist
- [ ] Status/logging good enough for `./run`-style operation
- [ ] Clean shutdown / blackout

**Gate:** operate a short `main/*` playlist from the native tree without using legacy Pixil.py.

### M5 — Compat engines

- [ ] Port or relink primitives required by inventory of `main/*` usage
- [ ] Metaballs / RD / particle scripts run without script edits
- [ ] Still no new verbs

**Gate:** “show library boots”; list any scripts deferred with reasons.

### M6 — Hardening

- [ ] Conformance suite green on Pi
- [ ] Metrics/logging parity decision documented
- [ ] Performance report: legacy vs native on agreed script set (producer-bound + engine)
- [ ] Cutover doc: how to run native day-to-day; legacy remains fallback

### M7 — Phase 1 done criteria

- [ ] Owner can deprecate *daily use* of legacy producer (legacy tree still installed)
- [ ] Phase 2 briefly re-estimated (consumer native / binary IPC) — not started unless requested

---

## 8. Testing strategy

Because there is **no human code review**, the suite is the reviewer. Prefer semantic stream compare (parse both sides; numeric tolerance) over brittle float string diffs.

### 8.1 Parity tiers

| Tier | What | When |
|------|------|------|
| 0 | Expression/condition oracle vs Python | Before trusting evaluator |
| 1 | Parse entire corpus | Grammar milestone |
| 2 | Ported behavioral unit tests from `tests/pixil` | Continuous |
| 3 | **Command-stream parity** (first N seconds/frames) | **Phase 1 gate** |
| 4 | Frame-buffer parity (headless) | Mostly Phase 2; optional later Phase 1 |
| 5 | Hardware smokes / goldens on Pi | Release |
| 6 | Owner visual review (esp. §6.5 pacing list) | End of Phase 1 |

Include at least one **negative test** (break a builtin on purpose; confirm Tier 3 catches it).

### 8.2 Randomness

`random` is everywhere. Unseeded runs block stream goldens.

- Prefer: seed hook on legacy + native; optionally match CPython MT for bit-exact parity (§13).
- Fallback: structural parity only (command counts/types/ranges) for random-heavy scripts.

### 8.3 Performance baselines

Record legacy producer-bound vs consumer-bound baselines on the **Pi** for an agreed set.

Suggested compare set:

- Light: `bouncing_ball.pix`, `KITT_Scanner.pix`
- Medium: `Hex_Grid_Glow.pix`, `Ink_Storm.pix`
- Heavy: `Metaballs_Effect.pix`, `Mandelbrot_Zoom.pix`, `Cloth_simulation.pix`
- Consumer-bound control: `Circles.pix` or `Amiga_Boing_Ball.pix`
- Pacing risk: 2–3 scripts with `throttle > 0`
- Playlist smoke: short `main/*` subset

### 8.4 Guardrails for AI-written native code

- Modern C++ (C++20) or safe Rust idioms; RAII; no raw owning pointers in interpreter code
- `-Wall -Wextra -Werror` + clang-tidy (or equivalent)
- Tests under **ASan + UBSan**; threaded paths under **TSan** in CI (x86 OK)
- Fuzz lexer/parser with corpus seeds
- Debug builds: bounds-checked indexing
- Top-level catch + **blackout on fatal**; keep emergency blackout tool
- Small PRs: each names the spec/behavior + tests that prove it
- Keep `DEVIATIONS.md` + ADRs so later sessions don’t undo decisions

---

## 9. Risks

| Risk | Mitigation |
|------|------------|
| Semantic drift (`%`, `/`, `round`, floats) | §6.6 harness + Tier 0/3 |
| Re-parse design copied into C++ | §6.2 compile-once required |
| Pacing / `throttle` surprises | §6.5 + owner decision + visual list |
| IPC tar pit with multiprocessing | ADR-001; pipe/socket bridge |
| Boiling the ocean on engines | Corpus frequency; defer rare engines |
| C++ memory/races with no reviewer | §8.4 sanitizers + fuzzing |
| Touching legacy accidentally | Separate repo/dir; oracle stays runnable |
| Premature Phase 2 | Forbidden until M7 |

---

## 10. Checklist — “ready to open the new project”

Use this before the next AI session starts coding.

### Owner / setup

- [ ] Create empty GitHub repo
- [ ] Clone on Pi under `~/Lightshow/pixil-native` (or chosen path)
- [ ] Confirm legacy still at `~/Lightshow/git` and matrix works (`./run -- main/bouncing_ball.pix -t 0:05`)
- [ ] Skim remaining §13 optionals (RNG / ops shell / hardware) or accept defaults
- [ ] Implementation language: **C++** (locked)
- [ ] Decide IPC option (ADR-001)
- [ ] Copy this document into the new repo `docs/`
- [ ] Freeze new language features/primitives on legacy during migration (**locked**)

### Charter pasted into new session

- [ ] Product goal: pure Pixil language runtime
- [ ] Phase 1 = full native **producer**; Python **consumer** stays
- [ ] Separate repo; legacy = oracle (+ optional shim/recorder hooks only)
- [ ] No new primitives; compat only
- [ ] **Compile-once** interpreter — do not re-parse every line like Python
- [ ] Port behavior, not Python optimizations
- [ ] Follow milestones M0→M7; **M1 parity tooling before deep language**
- [ ] First hardware milestone: M2 Hello matrix

### Reference paths for the new session

```text
Legacy repo:  /home/pi/Lightshow/git   (or Programming path on desktop)
Key docs:     docs/Pixil_Scripting_Guide.txt
              docs/Pixil_Development_Guide.txt
              docs/Native_Pixil_Runtime_Phase1_Plan.md
Protocols:    shared/*_protocol.py, shared/command_queue.py
Consumer:     rgb_matrix_lib/
Scripts:      scripts/main/
```

---

## 11. Suggested first prompt for the next AI session

> You are implementing **Phase 1 of a native Pixil runtime** in this new repository.  
> Read `docs/Native_Pixil_Runtime_Phase1_Plan.md` end-to-end and follow it.  
> **Do not** rewrite the LED consumer; drive the legacy Python `rgb_matrix_lib` via the IPC approach in ADR-001 (create ADR-001 if missing).  
> **Do not** add new Pixil primitive verbs.  
> **Do not** transliterate Python’s line-regex-eval loop — use compile-once (AST/VM).  
> Build **parity infrastructure (M1)** before a large interpreter: seed/recorder hooks against the legacy oracle, expression cross-checks, `DEVIATIONS.md`.  
> Then **M2 Hello matrix**.  
> Legacy reference tree is at `~/Lightshow/git` (read-only unless a thin consumer shim or oracle hooks are required).  
> Success for this session: contracts + tooling started, and/or native process clears/draws on the physical matrix through the legacy consumer.

---

## 12. One-page summary

- **New repo + new Pi directory**; legacy stays until you deprecate it.  
- **Phase 1 = entire producer** (language, files, playlist, logging, IPC, compat engines).  
- **Consumer stays Python** until a later phase.  
- **Compile once**; don’t port Python opts; match Python semantics (`%`, `/`, …).  
- **Watch `throttle` pacing** — faster producer can change look.  
- **Parity tooling before big rewrite**; sanitizers replace human review.  
- This document is the handoff charter for the next project/session.

---

## 13. Open questions for the owner

### Resolved

| # | Topic | Decision |
|---|--------|----------|
| 1 | **Pacing / `throttle`** | Accept in language; Phase 1 always drive consumer as **`throttle(0)`**; remove from consumer in Phase 2 |
| 2 | **C++ vs Rust** | **C++** (easier continuity for non-dev / Phase 2 matrix lib); sanitizers mandatory |
| 3 | **Legacy freeze** | **Yes** — no new language features or primitives on legacy during migration |

### Still open (optional at M0; defaults in parentheses)

1. **RNG:** Bit-exact CPython MT, or structural parity for random scripts? (**Default: structural / seeded, not bit-exact MT.**)
2. **Ops shell:** All-native host, or thin Python launcher for CLI/playlist/metrics? (**Default: all-native host for Phase 1 parity; thin launcher only if it unblocks M4.**)
3. **Spec vs Python:** Python wins on conflicts? (**Default: yes**, log in `DEVIATIONS.md`.)
4. **Hardware:** Pi 4 only, or also Pi 3 / Pi 5? (**Default: Pi 4 only.**)
5. **Phase order:** Already affirmed — Phase 1 producer-first.

---

## 14. Retire list (do not port)

- JIT, loop-compiler-as-fallback-system, condition templates, optimization flag matrix, parse-value fast-path maze
- Regex `process_lines` design (becomes grammar + compiler)
- Phase 2 only: text command parsing in consumer; batch protocols *as wire format* (keep frame ordering semantics)
- Python-only metrics columns (fast-path %, JIT %, …) — keep run-level metrics; migrate schema once

Keep legacy Python **runnable as oracle** until Phase 1 (and preferably Phase 2) passes owner cutover.
