# Pixil Spec: Retained Drawing Layers

**Status:** Proposal, ready for implementation
**Repo:** kklasmeier/pixil-led-matrix-cpp
**Areas touched:** interpreter (`src/runtime`, `src/emit`), consumer (`src/consumer`), draw-batch protocol, docs, tests

---

## 1. Summary

Today the consumer (rgb_matrix_lib) has **one drawing buffer** (`MatrixApi::drawing_`). Each frame it composites three things in a fixed order:

1. background planes
2. the drawing buffer
3. sprites, always on top

This spec replaces the single buffer with a **stack of 1..n retained drawing layers**, addressed by an integer `z`. Sprites move onto the **same z axis**, so drawing can now cover a sprite and a sprite can pass behind scenery. Layers keep their contents between frames until a script clears them. A script can therefore update one layer, such as a moving character or a clock's digits, and the consumer rebuilds the whole frame from the layers it already holds. Static content is sent over IPC once instead of every frame.

Sprite scaling and rotation, which make depth useful for 3D-style scripts, are in the companion spec `Pixil_Sprite_Transforms_Spec.md`.

### Goals
- Drawing and sprites share one depth order.
- Layers are retained, so redrawing is incremental.
- A layer can be cleared, deleted, hidden, shown and offset.
- **Existing scripts render identically** with no edits.
- The script-side API stays small: a handful of plain commands, and existing commands keep their meaning.

### Non-goals (for now)
- Per-pixel alpha blending between layers (left room for it in Phase 4).
- Rotation of layers (sprite transforms are in the companion spec).
- Replacing the background-plane system. It stays as is; unifying it with layers is an optional Phase 4 item.

---

## 2. Current architecture (relevant parts)

| Piece | Where | Today |
|---|---|---|
| Drawing buffer | `MatrixApi::drawing_` (`Buffer`, 64×64 `Rgb`, transparent sentinel) | One buffer. Cleared by `begin_frame` unless preserve is set. |
| Backgrounds | `BackgroundManager` (`background.h/.cpp`) | Tiled sprite-cel planes with offsets, composited under the drawing buffer. |
| Sprites | `SpriteManager`, `SpriteInstance::z_index` | Sorted among themselves by `z_index`, then creation order. Always blitted after drawing (`blit_sprites_onto_surface`). |
| Frame present | `MatrixApi::end_frame`, `refresh_display` | Composites background viewport + drawing + sprites onto the surface, then swaps. Preserve mode reseeds the back buffer. |
| Burnout | `BurnoutManager` | Instant and Fade burnouts write into and clear from `drawing_`. |
| Draw batch | `src/emit/draw_batch.*`, `consumer/batch_decode.*` | Binary records `OP_PLOT`..`OP_ARC` (1–7). Text, sprite and background commands travel as text or sprite batches. |

Note: the C++ consumer already rebuilds the full composite each present, so there is **no save/restore of the pixels under a sprite** left to remove. The layer stack slots into the existing composite path.

Audit of `scripts/`: every existing `show_sprite` uses `z_index` ≥ 0 (values seen: 0, 1, 5, 10). No script uses a negative z. The compatibility rules in §4.5 rely on this.

---

## 3. Script API

All new commands are plain statements in the same style as existing commands. Arguments are numeric expressions. `z` is an integer from **0 to 255**.

### 3.1 Drawing layers

| Command | Meaning |
|---|---|
| `set_depth(z)` | All following drawing commands draw into layer `z`. This covers plot, mplot, lines, rectangles, circles, polygons, ellipses, arcs and `draw_text`. The layer is created automatically the first time it is used. **Higher z is nearer the viewer and draws on top**, the same convention as sprite `z_index`. |
| `clear_layer(z)` | Make every pixel of layer `z` transparent. The layer keeps its visibility and offset. |
| `delete_layer(z)` | Remove layer `z` and free its memory. It is recreated empty the next time `set_depth(z)` is used. If `z` is the current layer, the current layer reverts to 0. |
| `hide_layer(z)` / `show_layer(z)` | Exclude or include the layer in compositing. Contents are untouched. |
| `set_layer_offset(z, x, y)` | Draw the whole layer shifted by (x, y) pixels when compositing. There is no wrap: pixels shifted off the panel are not shown, and the vacated area is transparent. The layer's stored contents never change. |
| `nudge_layer(z, dx, dy)` | Add (dx, dy) to the layer's current offset. |

`z` for layer commands may be a variable expression, such as `set_depth(int(255 - v_dist * 6))`. Values outside 0–255 are a **runtime error** with the line number. Do not clamp silently, because silent clamps hide bugs in depth formulas.

### 3.2 Frame semantics (unchanged commands, defined for layers)

| Command | With layers |
|---|---|
| `begin_frame` | Clears the **contents** of every layer. Keeps the existing layers, their visibility and their offsets. Resets the current layer to **0**. |
| `begin_frame(true)` | Clears nothing. Resets the current layer to **0**. The script clears whichever layers it wants with `clear_layer`. |
| `end_frame` | Composite (§4.2), swap and pace. |
| `clear` | Clears all layers' contents and resets the current layer to 0. Sprites and backgrounds behave as they do today. |
| Immediate mode (no frames) | Commands draw into the current layer. The present after each command composites the full stack, as `refresh_display` does now. |

Resetting the current layer to 0 at each `begin_frame` is deliberate. A script that forgets to reset its depth after drawing a HUD cannot leak drawing onto the HUD layer next frame.

### 3.3 Sprites

- `show_sprite(name, x, y, [inst], [z], [cel])` already has `z_index`. That value is now the sprite's position on the **shared z axis**. The meaning is unchanged for existing scripts (§4.5).
- At equal z, **sprites draw after the drawing layer of that z**. A sprite is never hidden by drawing on its own layer.
- New: `set_sprite_depth(name, z, [inst])` changes a live instance's z without re-showing it. This is needed when a sprite moves nearer or farther every frame. The existing `OP_MOVE_Z` sprite-batch record can carry it.

### 3.4 Example

```pixil
# Layer 0: sky and mountains, drawn ONCE
set_depth(0)
call draw_sky
set_depth(10)
call draw_far_hills
set_depth(20)
call draw_near_trees

show_sprite(runner, 20, 44, 0, 15, 0)     # z 15: in front of hills, behind trees

while true then
    begin_frame(true)                     # keep everything
        nudge_layer(10, -1, 0)            # far hills drift slowly
        nudge_layer(20, -2, 0)            # trees drift faster (parallax)
        clear_layer(30)
        set_depth(30)                     # HUD on top
        draw_text(1, 1, "" & v_score, tiny64_font, 5, white, 60)
        move_sprite(runner, 20, 44, 0)
    end_frame
endwhile
```

(A real parallax script would redraw a strip at the edge or use backgrounds for wrapping. This example shows the mechanics.)

---

## 4. Consumer design (`src/consumer`)

### 4.1 Data structures

```cpp
struct DrawLayer {
    Buffer pixels;            // 64x64 Rgb, transparent sentinel = empty
    bool visible = true;
    int offset_x = 0, offset_y = 0;
    bool dirty = true;        // contents/offset/visibility changed since last composite
};

class LayerStack {
public:
    DrawLayer& get_or_create(int z);       // enforces kMaxLayers
    DrawLayer* find(int z);
    void clear(int z);
    void remove(int z);
    void clear_all_contents();
    template <class F> void for_each_ascending(F&&);   // std::map<int, DrawLayer> order
    int current = 0;
private:
    std::map<int, DrawLayer> layers_;      // sparse; usually 1–10 entries
};
```

- `MatrixApi::drawing_` becomes `layers_.get_or_create(layers_.current).pixels`. Every drawing path writes through one accessor, `current_buffer()`, so shapes, text, plots and burnout need no per-shape changes.
- **Transparency:** reuse the existing transparent sentinel (`kTransparentR/G/B`, `is_transparent`). No new mask is needed.
- **Limits:** `kMaxLayers = 64` live layers, which is about 12 KB each and 768 KB in the worst case. Creating a 65th layer is an error naming the limit.

### 4.2 Compositing order (each present)

```
1. Background viewport (unchanged; black if none)
2. Walk z ascending over the union of {layer z's} and {sprite z's}:
       for this z:  a) drawing layer z (if exists && visible), honouring its offset
                    b) sprites with z_index == z, in existing creation order
3. Swap
```

The merge is one pass over a sorted list of layers and a sorted list of sprite instances. `iter_draw_order()` already provides sprites sorted by `(z, creation)`.

### 4.3 Preserve mode and the back buffer

Because every present now composites from the layer stack, the preserve path no longer needs the back buffer to carry state:

- `end_frame` always composites from layers. The "reseed back buffer for preserve" branch and the `preserve_pixels_` replay can be removed **once compatibility tests pass** (§7.2).
- `begin_frame(true)` simply doesn't clear layers.

This should simplify `end_frame`. Keep the existing pacing and `note_present` behaviour exactly as it is.

### 4.4 Burnout

`BurnoutManager` records the layer z active when each burnout object was created. Its clear and fade steps act on **that** layer, even if the script has since changed `set_depth`. If the layer has been deleted in the meantime, the burnout silently ends.

### 4.5 Backward compatibility (must hold)

| Old behaviour | Why it is preserved |
|---|---|
| All drawing goes to one buffer | The current layer defaults to 0 and resets to 0 every frame. Scripts that never call `set_depth` only use layer 0. |
| Sprites are always above drawing | Existing sprites use z ≥ 0 (audited), and at equal z sprites draw after the drawing layer. Layer 0 is therefore always below every existing sprite. |
| Backgrounds are under everything | Backgrounds stay step 1 of the composite. |
| `begin_frame` / `begin_frame(true)` / `clear` | Same visible results with a single layer. |

### 4.6 Performance

- **Base cost:** 4,096 pixels × (visible layers + sprites). With 5 layers that is about 20k pixel operations per frame, trivial in C++ on a Pi 4.
- **Prefix cache (Phase 3):** keep a cached composite of the backgrounds plus the run of lowest layers that are all clean, with no sprite inside that range. Recompose only from the first dirty layer or sprite upward. Static skies then cost a single 64×64 copy.
- **The main win is upstream:** static layers are sent once, so the interpreter skips evaluating that drawing and the IPC pipe carries far less per frame.

---

## 5. Protocol and interpreter changes

### 5.1 Draw batch (ordering matters)

Layer switches and clears must stay ordered with the drawing records around them, so they belong **in the draw batch**:

| Op | Record | Notes |
|---|---|---|
| `OP_SET_LAYER = 8` | op(1) + z(uint8) | Emitted when `set_depth` changes the current layer. Optimisation: skip it when z is unchanged. |
| `OP_CLEAR_LAYER = 9` | op(1) + z(uint8) | |

Update both sides: `src/emit/draw_batch.*`, `consumer/batch_decode.*`, and the record-size table. Also update the Python reference `shared/draw_batch_protocol.py` and regenerate `tests/golden/draw_batch.jsonl` with `tools/gen_draw_batch_goldens.py`.

`draw_text` travels outside the batch, so the **consumer** must own the "current layer" state, and text simply uses `current_buffer()`. Before emitting any non-batch command, the emitter must flush the open batch, as it already does today, so the layer switch is applied first.

### 5.2 Text commands (low-frequency)

`delete_layer`, `hide_layer`, `show_layer`, `set_layer_offset`, `nudge_layer` and `set_sprite_depth` travel as normal text commands parsed in `consumer/parse.cpp`. Use the same flush-before-send rule.

### 5.3 Interpreter

- Register the commands, argument counts and types in the command table and the syntax checker.
- Range-check z (0–255) in the interpreter so the error carries the script line number.
- `--dry-run --trace` emits the new ops and commands. Update the trace pretty-printer if one exists.

---

## 6. Phase plan

| Phase | Scope | Done when |
|---|---|---|
| **1. Layer stack** | `LayerStack`, `set_depth`, `clear_layer`, the batch ops, the composite merge with sprites, frame semantics, burnout targeting | All of §7.1–7.3 pass; old scripts render byte-identically |
| **2. Layer controls** | `delete_layer`, `hide_layer`/`show_layer`, `set_layer_offset`/`nudge_layer`, `set_sprite_depth` | Unit tests plus the demo scripts |
| **3. Performance** | Prefix composite cache, skip unchanged `OP_SET_LAYER` | Benchmark shows a static-heavy script using less consumer CPU |
| **4. Optional extras** | Background unification, alpha blending | Separate review |

---

## 7. Test plan

### 7.1 Unit (`tests/unit`)
- Layer order: a red rectangle on layer 5 covers a blue one on layer 2, whichever is submitted first.
- Transparency: pixels a layer never drew show what is underneath.
- Sprite interleave: a sprite at z 3 sits between layer 2 and layer 5. At z 5, the sprite is above layer-5 drawing.
- `clear_layer`, `delete_layer` (recreated empty), hide/show, offset clipping, negative offsets.
- `begin_frame` clears contents but keeps offsets and visibility. `begin_frame(true)` keeps everything. The current layer resets to 0.
- A burnout started on layer 4 clears on layer 4 after `set_depth(9)`.
- Errors: z = 256, z = -1, the 65th layer.
- Batch codec round-trip for ops 8 and 9, plus the regenerated goldens.

### 7.2 Backward compatibility (the key gate)
Before any change, record a **per-frame hash** of the composited surface for every script in `scripts/main` (236 scripts). Use `--dry-run --max-frames 300 --seed 1` with the consumer's surface hashed per present (`sha256.cpp` exists). Re-run after each phase. **Every hash must match.** Include the 24 scripts that use `set_background` and every script that uses preserve mode or burnout.

### 7.3 New feature scripts
Add `scripts/test/layers_*.pix` covering each command, plus the showcase scripts from §9 as they are written.

### 7.4 On-device
Run a static-heavy script, such as the parallax demo, on the Pi 4 at fps 60 and compare consumer CPU against an equivalent redraw-everything version.

---

## 8. Sprite transforms (separate spec)

Sprite scaling, rotation and flipping are specified in **`Pixil_Sprite_Transforms_Spec.md`**. That work depends only on **Phase 1** of this spec (the shared z axis). Transformed sprites composite at their `z_index` exactly as described in §4.2.

## 8.1 Recommended build order across both specs

| Step | Work | Why this order |
|---|---|---|
| 1 | **Layers Phase 1** (layer stack, `set_depth`, `clear_layer`, composite merge) | Rewrites the compositor that everything else plugs into. The compatibility hash gate is established here. |
| 2 | **Layers Phase 2** (delete, hide/show, offsets, `set_sprite_depth`) | Small, and finishes the layer API scripts will use. `set_sprite_depth` is needed by every 3D sprite script. |
| 3 | **Sprites S1–S2** (scale, flip, rotation, tilt) | Builds on the new compositor. Delivers the visible new capability. |
| 4 | **Sprites S3 + Layers Phase 3** (transform cache, prefix composite cache) | Performance work is easiest once the features are stable and real scripts exist to benchmark. |
| 5 | **Sprites S4** (authored angle and size cels) | Polish, done once real scripts show where hand-drawn cels are needed. |

## 9. Script ideas this unlocks

| Script | Uses | Idea |
|---|---|---|
| **Parallax Valley** | retained layers, `nudge_layer`, sprite depth | Side-scrolling landscape with 5 depth layers (sky, far peaks, hills, forest, foreground grass) drawn once and nudged at different speeds. A deer sprite walks *between* the forest and the grass. Almost no per-frame drawing. |
| **Nova_Runner v2** | scaled sprites, sprite depth | Pixel-art enemy ships and alien trees that grow from the horizon. The player's lasers and explosions sit at correct depths, and enemies pass behind pillars. |
| **Doom_Tour with inhabitants** | per-column `set_depth`, scaled sprites | Each wall column sets its depth from distance. Barrels, lamps and wandering creatures are scaled billboard sprites that are correctly hidden behind wall edges, column by column. |
| **Reef Aquarium** | retained layers, sprite depth | Rocks, coral and three bands of swaying seaweed on separate layers. Fish sprites swim at depths between them, gliding behind one plant and in front of the next. Only the seaweed layers and sprites update. |
| **Ambient Clock / Info Panel** | retained layers, `clear_layer`, hide/show | Frame, labels and a background scene drawn once. Only the digits layer is cleared and redrawn each second, and a colon layer blinks with hide/show. Suits low-power, always-on displays. |
| **Rainy Window** | layers, offsets | Blurry city lights behind glass on a back layer, two rain layers at different depths sliding with offsets, and droplets running down a front "glass" layer. |
| **Puppet Theatre** | layers, offsets, sprite depth | A stage with backdrop, painted scenery flats and curtains on the top layer. The curtains slide open with `set_layer_offset`. Animated puppet sprites act between the scenery flats, and scene changes swap pre-drawn backdrop layers with hide/show. |
| **Scene Slideshow / Transitions** | hide/show, offsets | Pre-render several scenes onto separate layers, then transition with slides (offset), cuts (hide/show) and wipes. A reusable intro for any showcase. |
| **Solitaire Autoplay** | many layers, offsets | Each moving card is its own small layer, slid into place with offsets. The z order changes as cards stack. Static tableau layers stay retained. |
| **Ocean_Sunset (optimised)** | retained layers | Stars and the far horizon move to retained layers that update only as the sky changes. The per-pixel sea shader is the only full redraw, freeing CPU for more waves. |
| **Isometric Town** | sprite depth, layers | Buildings on layers ordered by their row. Little cars and people sprites move along streets and pass behind buildings correctly by setting sprite depth from their row. |

---

## 10. Open decisions

1. **Name:** `set_depth` (as proposed) or `set_layer`. Higher z is nearer the viewer to match sprite `z_index`. A script working from distance converts with something like `int(255 - dist * k)`. `set_layer` may read more clearly given that convention.
2. **Layer limit:** 64 live layers. Is that enough for per-column raycaster depths? It is if distances are bucketed, for example `int(dist)` over a 0–40 range.
3. **Offset wrap:** layers don't wrap (backgrounds still provide tiled wrap). Add an optional wrap flag later if parallax scripts want it.
4. **Background unification (Phase 4):** backgrounds could become layers with tiled sprite contents. That would be cleaner, but all 24 background scripts would have to stay hash-identical.
