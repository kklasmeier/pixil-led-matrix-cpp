# Command dialect — producer → consumer contract (Phase 1)

What the native producer must send to the legacy Python consumer
(`rgb_matrix_lib`) so the panel behaves exactly as under legacy `Pixil.py`.
Phase 1 freezes this dialect: **no new primitive verbs** (charter §0).

Sources of truth (legacy tree, read-only): `pixil_utils/parameter_types.py`,
`rgb_matrix_lib/commands.py`, `shared/{draw_batch,mplot,sprite_batch}_protocol.py`,
`Pixil.py` (`execute_command`, `flush_frame_commands`). The generated sections below
come from `tools/gen_command_dialect.py` (`--check` in CI keeps them current).

## 1. Command strings

Each queue entry is one text command executed by `CommandExecutor.execute_command`:

```text
command   = bare_verb | verb "(" [ arg { "," arg } ] ")"
bare_verb = "clear" | "end_frame" | "sync_queue" | "dispose_all_sprites"
          | "endsprite" | "hide_background"
arg       = number | bool | bare_word | '"' { char | '\"' } '"'
```

* Named colors and modes travel as bare words (`draw_circle(32, 32, 5, red, 100, true)`).
  Spectral colors are integers `0..99`.
* Strings containing commas or spaces are double-quoted; the consumer keeps
  backslash escapes.
* `draw_batch("…")`, `plot_batch("…")`, `sprite_batch("…")` carry a base64 payload
  and use the executor's fast path (no quote walk).

## 2. Session and frame ordering (legacy behaviour to reproduce)

* Process start: `clear`, `sync_queue`.
* Every script start: `fps(0)` (before the script's own `fps(n)`).
* Between scripts: `__SCRIPT_RESET__` (consumer runs `reset_fps`, closes an open
  frame, `clear`, `dispose_all_sprites`) and waits for its acknowledgement.
* `begin_frame(p)` … `end_frame`. The producer buffers frame commands and flushes
  them at `end_frame` in this order:
  1. `begin_frame(false|true)` (deferred, so it is queued next to the batches)
  2. every `draw_batch(...)`
  3. every `sprite_batch(...)`
  4. every `plot_batch(...)`
  5. all other frame commands in script order (e.g. `draw_text` HUD on top)
  6. `end_frame`
* `mplot` without a frame batches into `plot_batch` until `mflush`.
* `throttle(n)` is producer-only (no consumer handler). Phase 1 always drives the
  consumer as `throttle(0)` — see DEVIATIONS D-007.

## 3. Control plane (ADR-001 frames, legacy equivalents)

| Native (ADR-001) | Legacy | Semantics |
|---|---|---|
| data `F`(id) → reply `F`(id) | `sync_queue` + `wait_until_empty` | all earlier commands executed |
| control `D` + data `K` → reply `K`(n) | `request_fast_drain` / `__DRAIN__` | discard queued commands up to the FIFO marker; `rest` is interrupted |
| data `R` → reply `R` | `__SCRIPT_RESET__` | between-script reset |
| data `T` → reply `T`(fp) | `__test_snapshot__` | `buffer_fingerprint` of the drawing buffer |
| data `X` → reply `X` | `__SHUTDOWN__` | finish queued work, blackout, exit |
| control `X` → reply `X` | `_force_shutdown` | immediate blackout (interrupts `rest`/backlog) |
| bridge death | consumer death | producer runs `emergency_blackout` |

## 4. `draw_batch` binary records

Little-endian. Each record = 1 op byte + payload. Shared tail fields:
`color:int16` (spectral `0..99` or named id below), `intensity:uint8` (`255` =
default), `burnout:uint32` (`0xFFFFFFFF` = none), `mode:uint8` (`0` instant,
`1` fade).

| Op | Name | Payload (`struct`) | Size incl. op |
|---|---|---|---|
| 1 | plot | `<HHhBIB8x` x, y, color, intensity, burnout, mode | 21 |
| 2 | line | `<4HhBIB3x` x0, y0, x1, y1, tail | 21 |
| 3 | rect | `<4HhBIBB2x` x, y, w, h, tail, fill | 21 |
| 4 | circle | `<3HhBIBB3x` x, y, r, tail, fill | 21 |
| 5 | polygon | `<4HfhBIBB3x` x, y, r, sides, rotation:f32, tail, fill | 25 |
| 6 | ellipse | `<4HfhBIBB3x` x, y, rx, ry, rotation:f32, tail, fill | 25 |
| 7 | arc | `<4HfhBIBB3x` x0, y0, x1, y1, bulge:f32, tail, fill | 25 |

Producer-side clipping (must match bit-for-bit; implemented in
`src/emit/draw_batch.cpp`, verified by `tests/golden/draw_batch.jsonl`):

* Coordinates: `int(float(v))` (truncate), clamped to `0..65535`.
* Lines and arcs: Cohen–Sutherland to `[0,63]²`, endpoints `round()` half-to-even.
  Fully outside → no record.
* Rectangles: clipped to the panel; empty or zero-size → no record.
* Circle/polygon/ellipse: dropped when `r <= 0`, the centre is negative, or the
  shape is fully off-panel (ellipse uses `max(rx, ry)`).
* Plot: intensity must be `0..100`, burnout `0..4294967294`.
* Float colors round half-to-even and clamp to `0..99`; digit strings are spectral.

Named color ids: black −1, white −2, gray −3, light_gray −4, dark_gray −5,
silver −6, red −7, crimson −8, maroon −9, rose −10, pink −11, salmon −12,
coral −13, brown −14, standard_brown −15, dark_brown −16, wood_brown −17, tan −18,
orange −19, gold −20, peach −21, bronze −22, yellow −23, lime −24, green −25,
olive −26, spring_green −27, forest_green −28, mint −29, teal −30, turquoise −31,
cyan −32, sky_blue −33, azure −34, blue −35, navy −36, royal_blue −37,
ocean_blue −38, indigo −39, purple −40, violet −41, magenta −42, lavender −43,
transparent −44.

`plot_batch` payload = concatenated 20-byte `mplot` records (`<HHhBIB8x`, no op byte).

`sprite_batch` records: op `B` (1 show, 2 move, 3 hide, 4 move+z), `name_len:B`,
`name` (UTF-8, ≤ 48 bytes), then for show/move `x:h, y:h, instance:B, cel:B`
(255 = none); move+z adds `z:h`; hide adds `instance:B`.

## 5. Script-side command signatures (generated)

Positional parameters after `PARAMETER_TYPES` conversion — the argument layout the
batch packers and consumer handlers receive.

<!-- BEGIN GENERATED: signatures -->
| Command | Parameters (`?` optional, `=default`) |
|---|---|
| `agent_chemotaxis` | `x:str`, `y:str`, `heading:str`, `field:str`, `size:float`, `sensor_dist:float?`, `step:float?`, `deposit:float?`, `count:int?`, `wrap:float?` |
| `array_lerp` | `a:str`, `b:str`, `out:str`, `t:float`, `count:int?` |
| `begin_frame` | `preserve_changes:bool?=false` |
| `chladni_step` | — |
| `clear` | — |
| `clear_text` | `x:int`, `y:int` |
| `define_sprite` | `name:str`, `width:int`, `height:int` |
| `dispose_all_sprites` | — |
| `dispose_sprite` | `name:str`, `instance_id:int?=0` |
| `draw_arc` | `x1:int`, `y1:int`, `x2:int`, `y2:int`, `bulge:float`, `color:color`, `intensity:int?=100`, `filled:bool?=false`, `duration:float?`, `burnout_mode:str?=instant` |
| `draw_circle` | `x:int`, `y:int`, `radius:int`, `color:color`, `intensity:int?`, `filled:bool`, `duration:float?`, `burnout_mode:str?=instant` |
| `draw_ellipse` | `x_center:int`, `y_center:int`, `x_radius:int`, `y_radius:int`, `color:color`, `intensity:int?=100`, `fill:bool?=false`, `rotation:float?=0`, `burnout:float?`, `burnout_mode:str?=instant` |
| `draw_line` | `x1:int`, `y1:int`, `x2:int`, `y2:int`, `color:color`, `intensity:int?`, `duration:float?`, `burnout_mode:str?=instant` |
| `draw_lines` | `x0:str`, `y0:str`, `x1:str`, `y1:str`, `color:color`, `intensity:str?`, `count:int?` |
| `draw_polygon` | `x:int`, `y:int`, `radius:int`, `sides:int`, `color:color`, `intensity:int?`, `rotation:float?=0`, `filled:bool?=false`, `duration:float?`, `burnout_mode:str?=instant` |
| `draw_rectangle` | `x:int`, `y:int`, `width:int`, `height:int`, `color:color`, `intensity:int?`, `filled:bool?=false`, `duration:float?`, `burnout_mode:str?=instant` |
| `draw_text` | `x:int`, `y:int`, `text:str`, `font_name:str`, `font_size:int`, `color:color`, `intensity:int?`, `effect:str?`, `effect_modifier:str?` |
| `field_render` | `program:str` |
| `fps` | `rate:float` |
| `grid_fill` | `array:str`, `value:float` |
| `grid_step` | `program:str` |
| `hide_background` | `layer:int?` |
| `hide_sprite` | `name:str`, `instance_id:int?=0` |
| `ink_step` | — |
| `mflush` | — |
| `move_sprite` | `name:str`, `x:int`, `y:int`, `instance_id:int?=0`, `cel_idx:int?`, `z_index:int?` |
| `move_sprites` | `name:str`, `x:str`, `y:str`, `cel:str?`, `z:str?`, `count:int?` |
| `mplot` | `x:int`, `y:int`, `color:color`, `intensity:int?`, `burnout:float?`, `burnout_mode:str?=instant` |
| `nudge_background` | `dx:int`, `dy:int`, `layer:int?=0`, `cel_index:int?` |
| `particle_apply_attractors` | `x:str`, `y:str`, `ax:str`, `ay:str`, `active:str`, `attractor_x:str`, `attractor_y:str`, `attractor_strength:str?`, `particle_strength:str?`, `falloff:float?`, `softening:float?`, `max_acceleration:str?`, `count:int?`, `attractor_count:int?`, `swirl:float?` |
| `particle_apply_springs` | `x:str`, `y:str`, `force_x:str`, `force_y:str`, `active:str`, `link_from:str`, `link_to:str`, `rest_length:str`, `stiffness:str?`, `tension:str?`, `count:int?`, `link_count:int?` |
| `particle_collide_bounds` | `x:str`, `y:str`, `vx:str`, `vy:str`, `active:str`, `left:float`, `right:float`, `top:float`, `bottom:float`, `restitution:str?`, `radius:str?`, `hit:str?`, `count:int?` |
| `particle_collide_circle_bounds` | `x:str`, `y:str`, `vx:str`, `vy:str`, `active:str`, `center_x:float`, `center_y:float`, `boundary_radius:float`, `restitution:str?`, `radius:str?`, `angular_speed:float?`, `grip:float?`, `hit:str?`, `count:int?` |
| `particle_collide_circles` | `x:str`, `y:str`, `vx:str`, `vy:str`, `active:str`, `radius:str`, `mass:str?`, `restitution:str?`, `hit:str?`, `count:int?` |
| `particle_collide_static_circles` | `x:str`, `y:str`, `vx:str`, `vy:str`, `active:str`, `obstacle_x:str`, `obstacle_y:str`, `radius:str`, `obstacle_radius:str`, `restitution:str?`, `hit:str?`, `count:int?`, `obstacle_count:int?`, `response:str?` |
| `particle_constrain_distances` | `x:str`, `y:str`, `active:str`, `link_from:str`, `link_to:str`, `rest_length:str`, `stiffness:str?`, `iterations:int?`, `tension:str?`, `count:int?`, `link_count:int?`, `min_distance:float?` |
| `particle_flock` | `x:str`, `y:str`, `vx:str`, `vy:str`, `ax:str`, `ay:str`, `active:str`, `separation_distance:float`, `neighbor_distance:float`, `separation_weight:float?`, `alignment_weight:float?`, `cohesion_weight:float?`, `max_speed:float?`, `max_force:float?`, `wrap_width:float?`, `wrap_height:float?`, `count:int?` |
| `particle_integrate` | `x:str`, `y:str`, `vx:str`, `vy:str`, `active:str`, `ax:str?`, `ay:str?`, `damping:float?`, `sleep_speed:float?`, `count:int?`, `integration_mode:str?`, `max_speed:str?`, `position_scale:float?` |
| `particle_verlet_integrate` | `x:str`, `y:str`, `old_x:str`, `old_y:str`, `active:str`, `ax:str?`, `ay:str?`, `damping:float?`, `count:int?` |
| `plot` | `x:int`, `y:int`, `color:color`, `intensity:int?`, `burnout:float?`, `burnout_mode:str?=instant` |
| `points_project3d` | `x:str`, `y:str`, `z:str`, `sx:str`, `sy:str`, `ax:float`, `ay:float`, `az:float`, `scale:float`, `camera:float`, `cx:float`, `cy:float`, `depth:str?`, `count:int?` |
| `rest` | `duration:float` |
| `set_background` | `sprite_name:str`, `layer:int?=0`, `cel_index:int?=0` |
| `set_background_offset` | `x:int`, `y:int`, `layer:int?=0`, `cel_index:int?` |
| `show_sprite` | `name:str`, `x:int`, `y:int`, `instance_id:int?=0`, `z_index:int?=0`, `cel_idx:int?=0` |
| `slime_step` | — |
| `sprite_cel` | `cel_index:int?` |
| `sprite_draw` | `sprite_name:str`, `command:str`, `args:str` |
| `sync_queue` | — |
| `throttle` | `factor:float` |
<!-- END GENERATED: signatures -->

## 6. Consumer verbs (generated)

Every verb `CommandExecutor` dispatches. The native producer may emit only these.

<!-- BEGIN GENERATED: consumer-verbs -->
`begin_frame`, `clear`, `clear_text`, `define_sprite`, `dispose_all_sprites`, `dispose_sprite`, `draw_arc`, `draw_batch`, `draw_circle`, `draw_ellipse`, `draw_line`, `draw_polygon`, `draw_rectangle`, `draw_text`, `end_frame`, `endsprite`, `fps`, `hide_background`, `hide_sprite`, `move_sprite`, `nudge_background`, `plot`, `plot_batch`, `rest`, `set_background`, `set_background_offset`, `show_sprite`, `sprite_batch`, `sprite_cel`, `sprite_draw`, `sync_queue`

(31 verbs)
<!-- END GENERATED: consumer-verbs -->
