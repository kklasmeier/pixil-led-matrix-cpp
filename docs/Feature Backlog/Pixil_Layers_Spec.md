# Pixil Spec: Drawing Layers

**Status:** Phase 1+2 implemented (hide/show/delete/reorder/set_sprite_*)  
**Repo:** kklasmeier/pixil-led-matrix-cpp  
**Companion:** Sprite scale/rotate is `Pixil_Sprite_Transforms_Spec.md` (after this)

---

## What this gives you

Today Pixil has **one** drawing surface. Sprites always sit on top of everything you draw.

This feature adds **named drawing layers**. You pick a layer number, draw on it, and it stays there until you clear or delete it. Sprites can live on a layer too, so a deer can walk **between** forest and grass instead of always on top.

**Backgrounds** (`set_background`) stay as they are: tiled scrolling planes that always sit **behind layer 0**.

---

## How stacking works (read bottom → top)

```text
FRONT (nearest you)
  … higher layer numbers …
  layer 30  →  drawing on 30, then sprites on 30 (by z_index)
  layer 20  →  drawing on 20, then sprites on 20
  layer 10  →  drawing on 10, then sprites on 10
  layer 0   →  drawing on 0,  then sprites on 0     ← default
  backgrounds (set_background)                      ← always under layer 0
BACK
```

Rules in plain language:

1. **Layer number = stack position.** Higher number is in front. You choose the numbers (`0`, `10`, `20`, `100`, `300`, …). Gaps are fine. Layer must be **≥ 0**.
2. **On one layer:** all drawing on that layer is painted first, then that layer’s sprites. Sprites win over drawing on the **same** layer.
3. **Among sprites on the same layer:** `z_index` decides order (higher in front). Same `z_index` → whoever was `show_sprite`’d later wins where they overlap.
4. **Sprites remember their layer** from whenever they were shown (see below). Moving them does not change layer unless you say so.
5. **Burnout stays inside its layer.** Overlay / fade ownership among pixels on one layer works as it does today (a newer plot can take over a cell that was almost ready to burn out). That stacking does **not** cross layers — a burnout on layer 10 never “jumps in front of” layer 20. `reorder_layer` moves that layer’s burnouts with the buffer.

---

## Commands (full syntax)

### Pick where drawing goes

```pixil
set_layer(layer)
```

- All following drawing goes onto that layer: `plot`, `mplot`, lines, shapes, `draw_text`, etc.
- Layer is created empty the first time you use it.
- If you never call `set_layer`, everything draws on **layer 0** (same as today).
- `begin_frame` / `begin_frame(true)` reset the “current layer” back to **0** so a forgotten HUD layer does not leak into the next frame. This does **not** hide sprites or clear their layer assignment.

```pixil
set_layer(10)
draw_rectangle(0, 40, 64, 24, green, 80, true)   # on layer 10

set_layer(30)
draw_text(2, 2, "SCORE", tiny64_font, 5, white, 80)  # on layer 30
```

---

### Clear a layer

```pixil
clear_layer(layer)
```

Erase every pixel on that layer (make them transparent).

- The layer **still exists**.
- Visibility is unchanged.
- Sprites on that layer are **unchanged** (they keep drawing).
- Use this when you want to redraw only that layer (e.g. clear the HUD digits, then draw new ones).

```pixil
clear_layer(100)
set_layer(100)
draw_text(1, 1, "" & v_score, tiny64_font, 5, white, 60)
```

---

### Delete a layer

```pixil
delete_layer(layer)
```

Remove that layer’s buffer and free its memory.

- **Sprites that were on that layer are hidden** (not disposed — templates and instances remain; they stop compositing).
- They **stay hidden**. Recreating the layer with `set_layer` / `show_layer` does **not** bring them back. Use `show_sprite` again (or an explicit unhide path you define) after assigning them to a living layer.
- If you delete the layer you are currently drawing on, the current layer goes back to **0**.
- The next `set_layer(layer)` for that number creates a fresh empty layer.

```pixil
delete_layer(20)    # trees buffer gone; sprites that were on 20 are hidden
```

---

### Hide a layer

```pixil
hide_layer(layer)
```

Leave the layer in memory, but skip it when compositing.

- Pixels stay as they are (not erased).
- Sprites on that layer are also not drawn while the layer is hidden.
- Use this for temporary “turn off this band” without losing the art.

```pixil
hide_layer(20)      # trees (and sprites on 20) vanish; buffer kept
```

---

### Show a layer

```pixil
show_layer(layer)
```

Include that layer in the composite again after `hide_layer`.

- Pixels are whatever you left them.
- Sprites on that layer draw again if those sprites are still visible (`hide_sprite` was not used on them).

```pixil
show_layer(20)      # trees (and their sprites) come back
```

---

### Reorder a layer (change its stack number)

```pixil
reorder_layer(from_layer, to_layer)
```

Take everything that lives on `from_layer` and make it live on `to_layer` instead — **without redrawing**.

- The pixel buffer moves (or is re-keyed) to `to_layer`.
- Visibility moves with it.
- **Sprites that were on `from_layer` move with it** (their layer assignment becomes `to_layer`).
- **Burnouts that were on `from_layer` move with it** (same ownership / overlay order as before the reorder — see Burnout below).
- `from_layer` no longer exists afterward (same as if it had been vacated).
- If `to_layer` **already exists**, it is **overwritten**: its old pixels are discarded, burnouts on the old `to_layer` end quietly, and sprites that were only on the old `to_layer` are **hidden** (same rule as `delete_layer` for that old content).
- If `from_layer` is the current drawing layer, the current layer becomes `to_layer`.
- If `from_layer == to_layer`, do nothing.
- Both numbers must be ≥ 0; missing `from_layer` is a no-op (or a clear runtime error — implement either consistently).

Use this when you already drew on layer `1` and later want that band in front of layer `9` without redrawing:

```pixil
set_layer(1)
call draw_forest

set_layer(5)
call draw_hud

reorder_layer(1, 10)    # forest buffer is now layer 10 → in front of HUD on 5
```

If something was already on 10:

```pixil
reorder_layer(1, 10)    # old layer 10 is replaced; forest is now on 10
```

Layers are for **stacking**, not sliding on screen. To move things around the panel, use **sprites** (`move_sprite`) or scrolling **backgrounds** (`nudge_background` / `set_background_offset`).

---

### Sprites and layers

Existing commands (unchanged spellings):

```pixil
show_sprite(name, x, y, [instance_id], [z_index], [cel_idx])
hide_sprite(name, [instance_id])
move_sprite(name, x, y, [instance_id], [cel_idx], [z_index])
move_sprites(name, x_array, y_array, [cel_array], [z_array])
dispose_sprite(name, [instance_id])
dispose_all_sprites()
```

**Which layer does a sprite join?**

- At `show_sprite`, the sprite joins whatever **current layer** `set_layer` last set.
- If you never called `set_layer`, that is **layer 0**.
- That layer sticks on the instance until you change it (below).

**What is `z_index` for?**

- Only orders sprites **on the same layer** (same meaning as today for DNA-style scripts that stay on one layer).
- It does **not** put a sprite on a different layer. For “between forest and trees,” put forest and trees on different layers and show the deer while that middle layer is current (or use `set_sprite_layer`).

```pixil
set_layer(10)
call draw_forest
show_sprite(rabbit, 8, 40, 0, 0, 0)    # layer 10, z_index 0
show_sprite(fox, 22, 42, 0, 1, 0)      # layer 10, z_index 1 → in front of rabbit

set_layer(15)
show_sprite(deer, 30, 38)              # layer 15 (between 10 and 20)

set_layer(20)
call draw_near_trees                   # covers deer where they overlap
```

**Change a sprite’s layer later** (without depending on current `set_layer`):

```pixil
set_sprite_layer(name, layer, [instance_id])
```

**Change `z_index` without advancing animation** (because bare `move_sprite` advances cel if you omit cel):

```pixil
set_sprite_z(name, z_index, [instance_id])
```

`move_sprite` / `move_sprites` can still pass `z_index` as today (DNA helix). That only updates order **inside** the sprite’s current layer.

---

### Frames (same habits as today)

```pixil
begin_frame            # clear every layer’s pixels; keep layers and visibility
                       # current set_layer → 0
                       # sprites stay visible (same as today — NOT hidden)
begin_frame(true)      # clear nothing; current set_layer → 0
end_frame              # composite everything, show, pace

clear                  # clear all layer pixels, current layer → 0
                       # same extras as today: clear surface, hide backgrounds,
                       # clear burnout; does NOT dispose sprites or delete layer slots
```

**Between scripts** (playlist / library advance): today’s `script_reset` already runs `clear` + `dispose_all_sprites`. It must also **dispose every drawing layer** and reset current layer to **0**, so the next script never sees the previous script’s retained buffers. Same rule for any path that fully resets the consumer between scripts (e.g. blackout/shutdown cleanup if it already tears down sprites).

Typical pattern for retained scenery (motion via sprites / backgrounds, not layer offsets):

```pixil
set_background(sky, 0)
set_background(hills, 1)

set_layer(10)
call draw_static_trees

set_layer(15)
show_sprite(deer, 20, 40)              # deer on 15, between trees and HUD

while true then
    begin_frame(true)
        nudge_background(1, 0, 1)       # hills scroll
        move_sprite(deer, v_x, 40)
        set_layer(100)
        clear_layer(100)
        draw_text(1, 1, "" & v_score, tiny64_font, 5, white, 60)
    end_frame
endwhile
```

---

## Worked example (who is in front?)

```pixil
set_background(sky, 0)                 # behind everything

set_layer(1)
# draw 1 and draw 2 both land on layer 1 (same buffer)
call draw_1
call draw_2

show_sprite(rabbit, 8, 40, 0, 10, 0)   # layer 1 (current), z_index 10
show_sprite(pig, 8, 40)                 # layer 1, z_index 0 (default)
show_sprite(rabbit, 8, 40, 1, 10, 0)   # layer 1, second rabbit, z_index 10
show_sprite(turtle, 8, 40, 0, 10, 0)   # layer 1, z_index 10

call draw_3                             # still layer 1

set_layer(2)
call draw_4                             # layer 2 only
```

**Back → front:**

1. Background sky  
2. **Layer 1 drawing** — draw 1 + 2 + 3 (one buffer; later pixels overwrite earlier if they overlap)  
3. **Layer 1 sprites** by `z_index`, then creation:  
   - `pig` (z 0)  
   - then the three with z 10 in show order: rabbit#0, rabbit#1, turtle  
4. **Layer 2 drawing** — draw 4 (in front of all of layer 1)

---

## Compatibility (old scripts)

Scripts that never call `set_layer`:

- All drawing → layer 0  
- All sprites → layer 0 (inherited)  
- On layer 0, sprites still paint after drawing → **sprites stay on top of drawing**, same as today  
- Backgrounds still under everything  
- `begin_frame` still does **not** hide sprites  

They should look the same.

---

## Limits

| Limit | Value |
|---|---|
| Layer number | Integer **≥ 0**, up to **65535** (sparse numbers OK; wire uses uint16) |
| How many layers alive at once | **No fixed cap** — memory is the limit (~12 KB per layer on a 64×64 buffer). Scripts should still prefer a handful of bands. |
| Negative layer | Runtime error with line number (no silent clamp) |

---

## Implementation notes (for whoever builds this)

Keep this section short; the script API above is the source of truth for authors.

### Performance (hard requirement)

Layers must **not** slow Pixil down for existing shows.

- Scripts that never call `set_layer` (everything on layer 0) must stay at today’s frame cost — same path or equivalent. No extra per-layer walk tax when only layer 0 is alive.
- Typical multi-layer scripts use a **handful** of bands. Composite cost may grow with live layers; that is fine. Do not add speculative caches or “skip dirty layer” machinery in this ship.
- Prefer one composite path that stays cheap for the single-layer case over a dual “old path / new path” forever. After layered present + fingerprint parity pass, drop the old preserve dual-write / `preserve_pixels_` path so we are not paying for two writers.
- Validate with real scripts (fps / paced frame timing on the Pi), not only unit tests — include preserve, background, and burnout-heavy shows.

### Consumer composite (each present)

1. Background viewport (or black)  
2. For each **live** layer number ascending:  
   - paint that layer’s pixels (if visible)  
   - then paint sprites assigned to that layer, sorted by `z_index` then creation order  
3. Swap / pace as today  

Every present rebuilds from these buffers. Fast-path the single live layer (0) so the common catalog does not pay for empty bands.

### Emitter (important)

Today one frame sends: one big draw batch → sprite batch → then text (`draw_text`, etc.). That would break `set_layer` + `draw_text` interleaving.

**Required:** flush open draw/sprite batches before layer-sensitive extras, in **script order**, so `set_layer(30)` then `draw_text` really hits layer 30. Flush must stay cheap (only when layer actually changes or extras need a boundary).

### Wire

| Piece | Change |
|---|---|
| Draw batch | `OP_SET_LAYER = 8` (layer uint16), `OP_CLEAR_LAYER = 9` (layer uint16) |
| Text commands | `delete_layer`, `hide_layer`, `show_layer`, `reorder_layer`, `set_sprite_layer`, `set_sprite_z` |
| `show_sprite` batch | Pack z_index on `OP_SHOW` (today in-frame show forces z=0) |
| Goldens / dialect | Update `docs/COMMAND_DIALECT.md`, `tests/golden/draw_batch.jsonl` |

### Burnout

Burnout already stacks **within one drawing surface**: a newer plot that overlays a pixel that is almost ready to burn out owns that cell; older burnout underneath does not jump ahead. That **intra-layer** ownership stays as today — it is scoped to the `set_layer` the pixels were drawn on. Layers do not invent a second burnout z-order across bands; composite order between layers handles that.

Rules:

- Each burnout object remembers the **layer** that was current when it was created; fade / expire / clear write that layer’s buffer.
- `clear_layer(n)` clears burnout on layer `n` (same layer’s pixels are gone).
- `delete_layer(n)` / overwrite via `reorder_layer` onto an existing `to`: burnouts on the discarded layer end quietly.
- `reorder_layer(from, to)`: burnouts on `from` **follow** to `to` (buffer + sprites + burnout ownership move together). Overlay order among those burnouts is unchanged.
- Scripts that never use layers keep today’s single-buffer burnout behavior on layer 0.

Burnout should continue to work with the existing cell / pixel engine without a special parallel system. **Test it** — overlay-while-fading, Instant vs Fade, `clear_layer` mid-fade, `reorder_layer` mid-fade, and at least one burnout-heavy catalog script under fingerprint / visual parity.

### Script reset

`script_reset` (IPC `R`, between playlist entries): `clear` + `dispose_all_sprites` + **dispose all layers** + current layer → 0. Leaving layers alive would leak one show’s scenery into the next.

### Compatibility testing

Do **not** use `--dry-run` for visual parity (it never runs the consumer).

Today `--fingerprint` only hashes the drawing buffer (no sprites/backgrounds). Layers need a **composited** per-present hash (headless), then baseline ~248 `scripts/main` scripts with `--max-frames` / `--seed`, including preserve, background, and burnout scripts. Confirm no fps regression on layer-0-only shows.

---

## Build phases

| Phase | Ship |
|---|---|
| **1** | Layer stack, `set_layer`, `clear_layer`, composite with sprites-per-layer, frame rules, emitter flush, burnout-by-layer, pack show z, performance bar (layer-0 parity) |
| **2** | `delete_layer` (hides that layer’s sprites), hide/show, `reorder_layer` (moves sprites + burnout), `set_sprite_layer`, `set_sprite_z` |

Out of scope for this feature: dirty-layer caches, merging backgrounds into layers, alpha blend between planes.

Then sprite transforms (`Pixil_Sprite_Transforms_Spec.md`).

---

## Script ideas this unlocks

Ideas that need **drawing layers + sprites on layers**. Grouped by feel: ambient / high-fidelity, fast-paced, and pseudo-3D.

### Ambient / high-fidelity (rich look, little redraw)

| # | Script | Layer tricks | Pitch |
|---|---|---|---|
| 1 | **Reef Aquarium** | Retained rock/coral; fish between bands; only seaweed `clear_layer` | Fish pass behind one plant and in front of the next. |
| 2 | **Parallax Meadow** | Background scroll; retained trees; deer/fox on a middle band | Side-scroll without redrawing the meadow. |
| 3 | **Ambient Clock** | Face retained; digits `clear_layer` each second; colon hide/show | Always-on, almost no per-frame drawing. |
| 4 | **Museum Diorama** | Glass on front; animals between terrain shelves; `reorder_layer` focus | Boxed scene with real occlusion. |
| 5 | **Window Rain** | City glow retained; rain mid-layer; droplet sprites in front | Soft city + depth rain without full city redraw. |
| 6 | **Puppet Theatre** | Backdrop/flats/curtain layers; puppet sprites between; hide/show + `reorder_layer` | Curtain up; cast between painted scenery. |

### Fast-paced / arcade

| # | Script | Layer tricks | Pitch |
|---|---|---|---|
| 7 | **Asteroid Dogfight Layers** | Stars retained; rocks mid; ships/shots `set_sprite_layer` by altitude; HUD top clear | Craft duck *behind* asteroids and pop out. |
| 8 | **Tube Runner** | Tunnel rings on distance-bucket layers; ship/shots on near bands; only near rings redraw | High-speed corridor with real occlusion. |
| 9 | **Pinball Depth** | Table art retained; ball/flipper sprites; ramps as higher layers | Ball goes under wireforms then over — classic pinball depth. |
| 10 | **Neon Drifter** | City parallax backgrounds; road retained; rivals by lane depth; sparks on FX layer | Chase; cars weave under overpasses on front layers. |
| 11 | **Bullet Orchard** | Trunks on staggered layers; player + pattern bullets `set_sprite_layer` | Danmaku where trees are real cover. |
| 12 | **Isometric Raid** | Building rows by depth; troops `set_sprite_layer` by tile row; explosions on short FX layer | Fast raid with wall cover and frantic FX. |

### Pseudo-3D / depth camera

| # | Script | Layer tricks | Pitch |
|---|---|---|---|
| 13 | **Doom Corridor** | Wall columns on layers from distance; enemy/prop sprites between; gun/HUD on top | Billboards hide behind nearer wall edges. |
| 14 | **Mode-7 Plains** | Ground stripes far→near layer buckets; ship climbs layers with altitude; mountains retained | SNES-style flyover with pass-behind hills. |
| 15 | **Voxel Stack** | Each Z-slice of a voxel object on its own layer; particles weave between slices | Chunky 3D logo/object with depth. |
| 16 | **Cockpit Jump** | Stars/planet retained; canopy frame on front; fighters between; reticle HUD clear | First-person space duel under a solid canopy. |
| 17 | **Starfield Dogfight 3D** | Backdrops retained; fighters `set_sprite_layer` from camera depth; laser bolts on near/far bands | Head-on jousts with ships crossing in depth. |
| 18 | **Layered Fireworks** | Sky retained; shells climb bands; spark layer cleared per burst | Bursts occlude trails by depth. |

**Lighter use:** DNA helix can stay on one layer with `move_sprites` `z_index` for near/far beads; add retained backdrop/HUD on other bands when you want more.

---

## Decisions locked in this revision

| Topic | Decision |
|---|---|
| Command name | `set_layer` (not `set_depth`) |
| Layer number | = stack order; ≥ 0; sparse OK (`10`, `20`, `300`, …) |
| Default | Layer **0** until `set_layer` is used |
| Backgrounds | Stay; always under layer 0 |
| Sprite → layer | Inherited from current `set_layer` at `show_sprite` |
| Sprite `z_index` | Order **inside** that layer only |
| Same layer | Drawing, then sprites |
| `begin_frame` | Does **not** hide sprites (unchanged). Resets current `set_layer` to 0. Clears layer pixels unless `begin_frame(true)`. |
| `delete_layer` | Removes buffer; **hides** sprites on that layer; they **stay hidden** until `show_sprite` again |
| Reorder without redraw | `reorder_layer(from, to)` reassigns the buffer, its sprites, and its burnouts to a new layer number; existing `to` is overwritten |
| Burnout | Intra-layer ownership stays as today (newer overlay wins). Burnouts live on the layer they were drawn on; `reorder_layer` moves them; deleted/overwritten layer ends them quietly. Test with cell engine. |
| Performance | Must not slow Pixil: layer-0-only shows stay at today’s cost; no speculative Phase-3 caches in this ship |
| No layer screen offsets | Sliding on screen is sprites / backgrounds — not part of the layer API |
| Live layer count | **No cap** (memory-limited); prefer a handful of bands |
| Between scripts | `script_reset` disposes **all layers** (plus today’s clear + dispose sprites) |
| Phases 3–4 | **Dropped** (dirty caches / merge backgrounds / alpha) — out of scope |

---

## Locked (follow-ups)

| Topic | Decision |
|---|---|
| `clear()` | Wipes every layer’s **pixels**; keeps layer slots. `delete_layer` / `script_reset` remove slots. |
| `set_sprite_layer` to missing layer | **Auto-create** an empty layer. |
| Layer number range | **0..65535** (uint16 on the wire). |
| `dispose_all_sprites` mid-script | Layers **stay**; only sprites go away. |
| `grid_step` / `field_render` | Draw to **current** `set_layer` (same batch path as `plot`). |

### Native draw kernels → current `set_layer` (locked)

`grid_step` and `field_render` **are in the C++ producer** (`src/runtime/grid.cpp`, dispatched from `machine.cpp`). They are first-class commands: each runs a named `grid_program` / `field_program` and queues normal draw pixels (`mplot` / small `draw_rectangle`) into the **same draw batch** as ordinary plots. They are not a separate compositor.

**Rule:** they paint onto whatever layer `set_layer` last selected — same as `plot`.

Particle **library** calls (`particle.integrate`, etc.) only update arrays; they do not draw. Scripts that `plot` afterward already follow `set_layer`. Other drawers that emit pixels (`chladni.step`, `ink.step`, …) also use the **current layer**.
