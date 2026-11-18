# Deviations from the legacy Python runtime

Every known, intentional (or owner-pending) behavioural difference between native
Pixil and the legacy oracle. Parity tests must cite an id from this file to accept
a difference — `tests/expr/expected_deviations.json` maps Tier 0 case ids to these
ids, and `make crosscheck` fails on any undocumented difference and flags stale
entries that now match.

Status: **Accepted** (owner-approved or charter-locked), **Proposed** (native
behaviour chosen, awaiting owner review), **Open** (owner decision required).

| Id | Area | Status | Corpus impact |
|----|------|--------|---------------|
| D-001 | Int vs float results from legacy fast paths | **Accepted** | Display stays `15`, not `15.0` |
| D-002 | `-v ** n` via textual substitution | **Accepted** | None found (`**` unused in scripts/main) |
| D-003 | Fractional power of a negative | **Accepted** | None found |
| D-004 | Integers beyond 64 bits | **Accepted** | None found |
| D-005 | Nested parentheses in conditions | **Accepted** | None found (legacy errors) |
| D-006 | Expressions as `&` operands | **Accepted** | None found (legacy errors) |
| D-007 | `throttle(n>0)` replay pacing | Accepted (charter §6.5) | Scripts with `throttle > 0` may look snappier |
| D-008 | `if` conditions evaluated more than once | **Accepted** | `Circles.pix` picks different random effects after its first effect change |
| D-009 | Bare procedure name ignored in some procedures | **Accepted** | `Fifteen_Puzzle.pix` title screen now draws its border |
| D-010 | Consumer burnout pump is executor-only | **Accepted** (Phase 2 C4) | Fade timing may differ slightly vs Python thread |
| D-011 | `throttle` removed from native consumer | **Accepted** (Phase 2 charter) | Same as D-007 for the panel; consumer ignores the verb |
| D-012 | Fingerprint is drawing-buffer only | **Accepted** (parity tool) | Sprites/backgrounds not in `PIXIL_BUFFER_HASH` |
| D-013 | Fancy text effects → NORMAL on native | **Proposed** (C3) | Only `tiny64` NORMAL path ported |
| D-014 | Emergency blackout helper stays Python | **Accepted** (while switch exists) | Headless skips; hardware fallback uses legacy module |
| D-015 | `draw_batch` intensity byte `0` → 100 | **Accepted** (Phase 2 C5) | Mirrors Python `intensity or 100`; `plot_batch` keeps byte 0 as 0 |
| D-016 | Burnout point lists are unique pixels | **Accepted** (Phase 2 C5) | Same as Python `set()`; required for fade clear correctness |

---

## D-001 — Legacy fast paths coerce ints to float (Accepted)

**Decision (owner, 2026-11-14):** integers stay integers. A whole number displays
as `15`, never `15.0`. Do not copy the legacy fast-path float coercion.

**Legacy:** `evaluate_math_expression` tries regex fast paths before `eval`
(`try_fast_arithmetic`, `try_fast_abs`, the `v_x % n` path, …). They wrap operands
in `float()`, so the *result type depends on expression shape*:

| Expression (`v_x = 10`, `v_neg = -1`) | Legacy value | Native value |
|---|---|---|
| `v_x + 5` | `15.0` | `15` |
| `abs(-7)` | `7.0` | `7` |
| `v_neg % 64` | `63.0` | `63` |
| `v_x * 2 + 1` (not a fast-path shape) | `21` | `21` |

The number is the same. On the panel it already looked like an integer:
`format_numeric_for_display` turns a whole-number float into `"15"` before
`draw_text`, and scripts such as `Pong.pix` also wrap scores in `int(...)`.
That is why `15.0` was not visible. Native gets the same text by keeping the
value an int. A true fraction (`2.5`) still prints as `2.5`.

The fast paths are not ported. They were regex shortcuts around Python `eval`
on every line. The C++ side compiles the expression once and evaluates it.
Those shortcuts are unnecessary there, and copying them would reintroduce this
shape-dependent type.

**Test:** `tests/expr` cases `legacy-math-03`, `legacy-fast-02/03/04/06/07/08`,
`h-mod-neg-var`, `h-abs-int`, `h-abs-var`, `h-m-grid-index` — marked
`numeric_equal`, so the cross-check still fails if the *value* ever differs.

## D-002 — Negative variable as `**` base (Accepted)

**Decision (owner, 2026-11-14):** keep native. `v_neg ** 2` with `v_neg = -1` is `1`, not the legacy substitution bug `-1`.

**Legacy:** variables are substituted as text before `eval`, so with `v_neg = -1`
the expression `v_neg ** 2` becomes `-1 ** 2`, which Python parses as `-(1 ** 2)`
→ `-1`. **Native:** binds the value, giving `(-1) ** 2` → `1`. The legacy result is
a substitution bug; no script in `scripts/main` uses `**` outside print strings.
Cases: `h-pow-negvar`, `h-pow-negfvar`.

## D-003 — Fractional power of a negative number (Accepted)

**Decision (owner, 2026-11-14):** keep native. A negative number to a fractional power is an error.

**Legacy:** `(-8) ** 0.5` yields a Python `complex`, which then fails or prints
oddly downstream. **Native:** raises `ValueError` at the expression (Pixil has no
complex type). Case: `h-pow-neg-frac`.

## D-004 — Integers beyond 64 bits (Accepted)

**Decision (owner, 2026-11-14):** keep native. Overflow past 64 bits becomes a float.

**Legacy:** Python ints are unbounded (`10 ** 20` stays an int). **Native:**
`int64` arithmetic that overflows promotes to float (`1e+20`). The LED domain never
needs >63-bit integers. Case: `h-pow-big-int`.

## D-005 — Nested parentheses in conditions (Accepted)

**Decision (owner, 2026-11-14):** keep native. Nested parentheses in conditions are allowed.

**Legacy:** the condition splitter cannot parse `((a or b) and c)`; the Scripting
Guide documents nesting as unsupported. **Native:** the compile-once parser accepts
any well-formed nesting. A superset: no working script changes. Case:
`h-cond-nested-parens`.

## D-006 — Full expressions as `&` concatenation operands (Accepted)

**Decision (owner, 2026-11-14):** keep native. `"sum" & v_x + 1` concatenates to `"sum11"`.

**Legacy:** each `&` part must be a literal or a single variable; `"sum" & v_x + 1`
raises `Variable 'v_x + 1' not found`. **Native:** `&` binds looser than `+`
(`"sum" & (v_x + 1)` → `"sum11"`). A superset. Case: `h-cat-arith-part`.

## D-007 — `throttle` replay pacing (Accepted, charter §6.5)

**Legacy:** `put_command` attaches a delay ≈ Δt × 0.7 × throttle and the consumer
sleeps it, partly replaying producer slowness. **Native Phase 1:** `throttle(...)`
parses and is accepted, but the consumer is always driven as `throttle(0)`; the
ADR-001 bridge has no per-command delay. Pacing is `fps` / `rest` / frames /
queue backpressure.

## D-008 — `if` conditions evaluated more than once (Accepted)

**Decision (owner, 2026-11-15):** keep native. Each `if` condition is evaluated once per visit. Do not copy the extra rolls from the line interpreter.

**Legacy:** it depends on whether the loop or procedure around the `if` compiled.
In a compiled block each condition is evaluated once. When the block falls back
to the line interpreter, `Pixil.py` evaluates the first condition while it
collects the `if`, throws that result away, and evaluates it again to choose the
branch. With an `else`, the last condition is in the branch list twice, so a
false condition is evaluated a third time. A condition that calls `random()`
draws two or three numbers.

**Native:** every condition is evaluated once, the same as a compiled block.
Copying the fallback would mean porting the compiled/uncompiled split itself
(charter §6.4).

**Impact:** `Circles.pix` does not compile in legacy and puts `random()` in two
`if`/`else` conditions. Its seeded stream matches for 2344 commands, then picks a
different random effect. The effect is still one the script can choose.
`Radioactive_Decay.pix` compiles, and its random-condition loop matches.

## D-009 — Bare procedure name ignored in some procedures (Accepted)

**Decision (owner, 2026-11-15):** keep native. A bare word that names a defined procedure always calls it. The title border in `Fifteen_Puzzle.pix` is drawn.

**Legacy:** a line holding only a procedure name (`draw_frame_border`, with no
`call`) runs that procedure inside a compiled procedure. When the procedure falls
back to the line interpreter, the line matches nothing and is skipped silently.

**Native:** a bare word that names a defined procedure always calls it.

**Impact:** `Fifteen_Puzzle.pix` calls `draw_frame_border` that way from
`draw_board` (compiled: the border was drawn) and from `show_title` (not
compiled: the border was skipped). Native draws the border on the title screen
too. `Geometric_Kaleidoscope.pix` uses the same form for `clear_with_fade`.

## D-010 — Consumer burnout pump is executor-only (Accepted, Phase 2 C4)

**Legacy:** `ThreadedBurnoutManager` mutates `drawing_buffer` from a background
thread (~60 Hz). **Native:** all burnout bookkeeping and buffer writes run on the
executor via `pump_fade` (idle inbox pop timeout + `rest` at ~16 ms, with a
forced tick when `rest` ends). Short rests such as Qix `rest(0.05)` still clear
trails. Visual instant/fade semantics match; mid-fade samples can differ by a
few frames under load.

**Fingerprint impact:** scripts with short fade burnouts (e.g. `Abstract_Geometry.pix`
at 80ms) yield **non-deterministic** Python hashes under `--fingerprint` because the
background thread can expire mid-batch / before `T`. Native is stable (no pump
during `draw_batch`). Stripping burnout from the same batch MATCHES. Keep such
scripts out of the hard `consumer-parity` gate; compare geometry via
`Hex_Grid_Glow` and dedicated fade smokes with `rest`.

**Test:** unit `consumer_burnout_*`; headless `tools/consumer_parity_compare.sh`
on non-race scripts; burnout smoke via short `.pix` + `rest`.

## D-011 — `throttle` removed from native consumer (Accepted)

Phase 2 consumer treats `throttle(...)` as a no-op. Producer still emits
`throttle(0)` for stream compatibility. Same panel pacing story as D-007.

## D-012 — Fingerprint is drawing-buffer only (Accepted)

`PIXIL_BUFFER_HASH` / ADR-001 `T` hashes opaque pixels in the drawing buffer
(transparent sentinel skipped), matching `rgb_matrix_lib.test_inspect`. Sprites
and background layers are composited at present and are **not** in the hash.
C5 compare scripts should exercise buffer draws (frames/shapes), not sprite-only
scenes, when using this fingerprint.

## D-013 — Fancy text effects → NORMAL (Proposed)

Native C3 ports `tiny64_font` NORMAL bitmap blit (plus center/right modifiers).
Named fancy effects (`SCROLL`, etc.) fall through to NORMAL. Document further if
a main-library script depends on a non-NORMAL effect for cutover.

## D-014 — Emergency blackout helper stays Python (Accepted while switch exists)

When the consumer dies without acknowledging shutdown, `ConsumerLink` still runs
the legacy `rgb_matrix_lib.emergency_blackout` helper (skipped under `--headless`).
After C6 removes the Python spawn path, keep this fallback until the owner
retires the legacy tree, or replace it with a native one-shot blackout binary.

## D-015 — `draw_batch` intensity byte `0` → 100 (Accepted, Phase 2 C5)

**Legacy:** `unpack_draw_batch` uses `intensity or 100`, so a packed intensity
byte of `0` is treated as full brightness (100). Scripts such as
`Hex_Grid_Glow.pix` emit intensity `0` for some filled polys and rely on this.
**Native:** `draw_batch_intensity_of` mirrors that. `plot_batch` / mplot still
treat byte `0` as intensity 0 (Python mplot unpack does not use `or 100`).

## D-016 — Burnout point lists are unique pixels (Accepted, Phase 2 C5)

**Legacy:** shape APIs register burnout via a `set` of pixels. **Native:**
`BurnoutManager::add` deduplicates points (first color wins). Without this,
outline duplicates break fade `last_pixel_colors` vs `point_indexes` last-wins
and expired fades leave residual opaque pixels (e.g. `Abstract_Geometry.pix`).
