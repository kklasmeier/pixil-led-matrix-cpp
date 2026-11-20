# Pixil Spec: Sprite Transforms (Scale, Rotation, Tilt, Flip)

**Status:** Proposal, ready for implementation after Layers Phase 1
**Repo:** kklasmeier/pixil-led-matrix-cpp
**Depends on:** `Pixil_Layers_Spec.md` Phase 1, which provides the shared z axis for sprites and drawing
**Areas touched:** interpreter (`src/runtime/machine.cpp` command table), `src/emit/sprite_batch.*`, consumer (`sprite.h/.cpp`, `api.cpp` sprite blit, `batch_decode.cpp`, `parse.cpp`), docs, tests

---

## 1. Summary

Sprites today draw 1:1 at integer positions, always at native size and orientation. This spec adds a per-instance **transform**:

- **Scale:** uniform from 0.1× to 8×, with an optional separate vertical scale for stretch effects.
- **Rotation:** in-plane spin around the z axis, any angle in degrees. Positive is clockwise and negative is counter-clockwise.
- **Tilt:** around the x axis (tipping forward/back) and the y axis (turning left/right), any angle, either direction. The sprite foreshortens like a card turning in 3D, goes edge-on at 90°, and shows its back face past 90°.
- **Flip:** horizontal and/or vertical.

All of these are applied in one inverse-mapping pass, with a quality strategy suited to a 64×64 panel and a cache so animated transforms stay cheap on the Pi.

Combined with layers, this lets a script build pseudo-3D scenes from pixel art: objects that approach and grow, sit at the right depth, and turn to face any heading.

### Goals
- One transform model covers scale, rotation and flip.
- Pixel art stays legible when shrunk or rotated (§5).
- Existing scripts are unchanged. The default transform is the identity, which uses the current 1:1 blit path.
- An animated transform, such as a sprite spinning every frame, costs close to a plain blit once it is cached.

### Non-goals
- True perspective on tilts, where the near edge draws larger than the far edge (a trapezoid). Tilt here is orthographic foreshortening, which at 10–20 px sprite sizes differs from true perspective by a pixel or two. §10 lists perspective as optional phase S5.
- Skew, or transforming background planes.
- Blending or alpha between sprite and scene. Sprites keep the opaque-or-transparent pixel model.

---

## 2. Current state

| Piece | Today |
|---|---|
| `SpriteInstance` | `x`, `y` (int, top-left), `z_index`, `current_cel`, `visible` |
| Blit | `MatrixApi::blit_sprites_onto_surface`: a 1:1 copy of `cel.pixels`, skipping transparent pixels, scaled by per-pixel intensity, clipped to the panel |
| Wire | `sprite_batch` records: `OP_SHOW`/`OP_MOVE` (x, y int16; instance, cel uint8), `OP_MOVE_Z` (+ z), `OP_HIDE`. In a frame they are batched; outside a frame they are sent as text commands. |
| Script | `show_sprite(name, x, y, [inst], [z], [cel])`, `move_sprite(name, x, y, [inst], [cel])`, `move_sprites(...)`, `hide_sprite`, `dispose_sprite`. Arguments are typed `Int`. |

---

## 3. Script API

### 3.1 New commands

| Command | Meaning |
|---|---|
| `set_sprite_transform(name, scale, angle, [inst], [tilt_x], [tilt_y], [flip])` | Set the instance's full transform. It persists until changed. `scale` is a float (1.0 = native). `angle` is the in-plane rotation in degrees: **positive = clockwise, negative = counter-clockwise**, any magnitude (normalised mod 360). `tilt_x` / `tilt_y` are degrees, either sign (§3.5). `flip` is 0 = none, 1 = horizontal, 2 = vertical, 3 = both. |
| `set_sprite_scale(name, scale, [inst], [scale_y])` | Change only the scale. If `scale_y` is given, `scale` applies horizontally and `scale_y` vertically, for squash-and-stretch. |
| `set_sprite_angle(name, angle, [inst])` | Change only the in-plane rotation (clockwise positive, counter-clockwise negative). |
| `set_sprite_tilt(name, tilt_x, tilt_y, [inst])` | Change only the tilts (§3.5). |
| `sprite_back_cel(name, cel)` | Template-level, declared after `endsprite`: the cel to show when a tilt turns the sprite's back toward the viewer. Without it, the back face is the mirror image of the front (§3.5). |
| `set_sprite_flip(name, flip, [inst])` | Change only the flip. |
| `set_sprite_anchor(name, mode, [inst])` | `0` = top-left (default, today's behaviour), `1` = centre. Centre anchoring is what 3D and rotating scripts want: x, y become the point the sprite rotates and scales around. |
| `reset_sprite_transform(name, [inst])` | Back to the identity: scale 1, angle 0, no flip, top-left anchor. |

Transform state belongs to the **instance**, not the template, so instances 0..n of `asteroid` can each spin at their own rate.

### 3.2 Anchor rules

- **Top-left anchor (default):** x, y is the top-left of the untransformed sprite. Scale and rotation pivot around the sprite's **centre** and the result is drawn so that centre stays where it would be at 1:1. An existing script that later adds rotation doesn't jump.
- **Centre anchor:** x, y is the centre pivot directly. This is the recommended mode for 3D projection (`move_sprite(enemy, v_sx, v_sy)` with `v_sx, v_sy` the projected point).

### 3.3 Sub-pixel positions

When an instance's transform is not the identity, `move_sprite`/`show_sprite` x and y are accepted as **floats** and kept at 1/16-pixel precision. Slow-moving scaled sprites otherwise step visibly a pixel at a time. Identity-transform sprites keep integer behaviour for compatibility.

Implementation: the command table types for x and y change from `Int` to `Num`. The emitter rounds to the nearest integer unless the instance has a non-identity transform, in which case it sends fixed-point (§6).

### 3.4 Rotation and tilt: what each does

| Control | Axis | Viewer sees | Direction |
|---|---|---|---|
| `angle` | z (out of the screen) | Sprite spins flat, like a clock hand or steering wheel | + clockwise, − counter-clockwise |
| `tilt_y` | y (vertical) | Sprite turns left/right like a door or a coin: it narrows, goes edge-on at ±90°, shows its back face beyond | + turns its right edge away from the viewer, − its left edge |
| `tilt_x` | x (horizontal) | Sprite tips forward/back like a falling sign: it gets shorter, goes edge-on at ±90°, shows its back face beyond | + tips its top edge away from the viewer, − its bottom edge |

All three can be combined, and each can be animated continuously in either direction (for example `v_tilt = v_tilt - 4` every frame spins a coin the other way).

### 3.5 Tilt rules

- **Foreshortening:** the effective scales are `sx = scale · cos(tilt_y)` and `sy = scale_y · cos(tilt_x)`. These are **signed**, so when a cosine goes negative the sprite is automatically mirrored on that axis, which is exactly how the back of a flat card looks.
- **Back face:** the sprite is showing its back when `cos(tilt_x) · cos(tilt_y) < 0`. If the template has a `sprite_back_cel`, that cel is drawn instead, using `|sx|` and `|sy|` (it is authored as seen from behind, so it is not mirrored again). Otherwise the mirrored front is drawn.
- **Edge-on:** when `|sx|` or `|sy|` × the sprite's size is under 1 px, the sprite is drawn as a 1-px **sliver** along its long axis in the cel's average opaque colour, so a spinning coin flashes a thin edge rather than vanishing (§5).
- **Order of operations:** flip → scale and tilt → in-plane rotation → position. So a sprite tilted with `tilt_y` and then spun with `angle` turns around its own axes, and its tilt axis spins with it.
- **Perspective:** none in S1–S4 (orthographic). Optional `set_sprite_perspective(name, amount, [inst])` in S5 would make the near edge larger.

### 3.6 Example

```pixil
define_sprite(rock, 16, 16)
    # ... pixel art ...
endsprite

for v_i in (0, 5, 1)
    show_sprite(rock, 0, 0, v_i, 10, 0)
    set_sprite_anchor(rock, 1, v_i)
endfor v_i

while true then
    begin_frame
        for v_i in (0, 5, 1)
            v_z[v_i] = v_z[v_i] - 0.3
            # project to screen, scale by distance, tumble
            v_s = 40 / v_z[v_i]
            set_sprite_transform(rock, v_s * 0.25, v_t * 3 + v_i * 60, v_i)
            set_sprite_depth(rock, int(255 - v_z[v_i] * 4), v_i)
            move_sprite(rock, 32 + v_x[v_i] * v_s, 24 + v_y[v_i] * v_s, v_i)
        endfor v_i
    end_frame
endwhile
```

### 3.7 Example: card flip and coin spin with tilt

```pixil
# card: cel 0 = front face, cel 1 = back pattern
define_sprite(card, 12, 16)
    sprite_cel(0)
        # ... front face art ...
    sprite_cel(1)
        # ... back pattern ...
endsprite
sprite_back_cel(card, 1)

show_sprite(card, 32, 32, 0, 10, 0)
set_sprite_anchor(card, 1, 0)
show_sprite(coin, 16, 20, 0, 10, 0)
set_sprite_anchor(coin, 1, 0)

while true then
    v_t = v_t + 1 / v_pace
    begin_frame
        # card flips over one way, pauses, flips back the other way
        v_flip = min(max(sin(v_t * 0.03) * 1.6, -1), 1) * 90 + 90
        set_sprite_tilt(card, 0, v_flip, 0)
        # coin spins on y (negative = the other direction) while wobbling on x
        set_sprite_tilt(coin, sin(v_t * 0.05) * 20, 0 - v_t * 6, 0)
        # and rocks clockwise then counter-clockwise in-plane
        set_sprite_angle(coin, sin(v_t * 0.02) * 45, 0)
    end_frame
endwhile
```

---

## 4. Rendering: inverse mapping

For each instance with a non-identity transform:

1. Build the forward 2D matrix **M** = translate(anchor) · rotate(angle) · scale(sx, sy) · flip · translate(−cel_centre), where `sx = scale · cos(tilt_y)` and `sy = scale_y · cos(tilt_x)` are signed (§3.5). Pick the front cel or the back cel. Compute the inverse **M⁻¹**. If |sx| or |sy| is too small to invert reliably, use the edge-on sliver rule instead (§5).
2. Transform the cel's four corners with **M** to get the destination bounding box, then clip it to the panel.
3. For each destination pixel (dx, dy) in the box, sample the cel at the source point(s) given by **M⁻¹**, using the quality rules in §5.
4. Transparent results are skipped. Intensity is applied exactly as in the current blit.

Use `float` maths. The box is at most 64×64, so cost per sprite is bounded at 4,096 destination pixels × samples per pixel, and is usually far smaller.

Flip-only, scale = 1, angle = 0 is an **exact fast path**: mirror the index math in the existing 1:1 loop, with no sampling.

---

## 5. Quality rules (the important part on 64×64)

A naive nearest-pixel lookup makes shrunk sprites lose features and rotated sprites shimmer from frame to frame. The rules:

| Case | Method |
|---|---|
| Identity | Existing 1:1 blit (byte-identical output) |
| Flip only | Exact mirrored copy |
| Rotation of exactly 90° / 180° / 270°, scale 1 | Exact index permutation (no resampling) |
| Upscale (s ≥ 1), any angle | **4-sample supersampling** at the 4 sub-pixel points (¼, ¾) of each destination pixel. Each sample takes the nearest source pixel. Result: the **majority opaque colour**. If ≥ 3 of 4 samples are transparent, the pixel is transparent. Keeps pixel-art edges crisp while smoothing rotation stair-steps. |
| Downscale (\|sx\| or \|sy\| < 1, from scale or tilt), any angle | **Area sampling**: sample a grid of ceil(1/\|sx\|) × ceil(1/\|sy\|) points covering the destination pixel's footprint in source space (capped at 16 samples). Colour is the **average of opaque samples**. The pixel is opaque if at least 40% of samples are opaque. The averaging keeps detail visible as blended colour, and the threshold keeps thin features from vanishing. |
| Edge-on (one axis < 1 px, the other ≥ 2 px, e.g. a tilt near ±90°) | Draw a 1-px sliver along the long axis (rotated with `angle`) in the cel's average opaque colour. |
| Tiny result (destination box < 2×2) | Draw one pixel with the cel's average opaque colour if ≥ 25% of the cel is opaque. Small distant objects become a dot instead of disappearing. |

The thresholds (40%, 25%) are constants in one place so they can be tuned on the real panel.

### 5.1 Hand-drawn angle and size cels (optional, recommended for hero sprites)

Arbitrary rotation of a 10-pixel sprite never looks as good as hand-drawn frames. Two opt-in hooks let authors supply their own frames:

- `sprite_angle_cels(name, first_cel, count)`: cels `first_cel .. first_cel+count-1` are hand-drawn headings evenly spaced around 360°. When set, the engine picks the cel nearest the requested angle and rotates **only by the remainder**. With 8 or 16 cels that remainder is ≤ 22.5° or ≤ 11.25°, so artefacts stay small. With `count` = 16 or more, the engine skips residual rotation entirely, which is the classic arcade look.
- `sprite_size_cel(name, cel, below_scale)`: use this hand-drawn small version whenever scale < `below_scale`, like a mipmap.

These are declared once, after `endsprite`. They interact with animation cels as follows: angle cels define a heading set, and `move_sprite`'s auto-advance is disabled for instances using them.

---

## 6. Wire format

New sprite-batch record, also accepted as a text command outside frames:

| Op | Payload | Notes |
|---|---|---|
| `OP_TRANSFORM = 5` | name, instance(uint8), scale_x(uint16, 8.8 fixed → 0.0–255.99), scale_y(uint16, 8.8), angle(uint16), tilt_x(uint16), tilt_y(uint16), flags(uint8: bit0 flipH, bit1 flipV, bit2 centre-anchor), reserved(uint8) | Angles travel in 1/100° after normalising to 0–359.99 (so −30° is sent as 330.00). Sent only when a transform changes; the emitter tracks the last-sent transform per instance. The reserved byte leaves room for S5 perspective. |
| `OP_MOVE_SUB = 6` | like `OP_MOVE` but x, y are int16 in **1/16 px** (±2048 px range) | Used only for instances with non-identity transforms (§3.3). |

`sprite_back_cel` is a template definition command sent as text, like `define_sprite`. Update `shared/sprite_batch_protocol.py`, `emit/sprite_batch.*` and `consumer/batch_decode.cpp`, and add golden records.

The `set_sprite_*` helper commands are interpreter-side conveniences. They update the interpreter's copy of the instance transform and emit one `OP_TRANSFORM`.

---

## 7. Transform cache

Spinning or approaching sprites change their transform every frame, so cache rendered results:

- **Key:** (template, cel, quantised effective sx, quantised effective sy, quantised angle, flags). The effective scales already include tilt, so tilt needs no separate key field. Scales are quantised to 1/32 steps in log space (sign kept), angle to **2°** steps.
- **Value:** a small `Buffer` (the rendered bounding box) plus its offset from the anchor.
- **Policy:** LRU, capped at **2 MB** total (thousands of small entries). Invalidate entries for a template when it is redefined.
- **Effect:** after the first rotation through 360°, a spinning 16×16 sprite costs only a cached blit per frame.

Positions stay exact even with the cache: sub-pixel x and y are applied when the cached box is blitted, and only scale and angle are quantised. The 2° angle step is invisible at this resolution, and a constant can tighten it.

---

## 8. Interaction with layers

- A transformed sprite composites at its `z_index` in the layer stack exactly like an untransformed one (Layers spec §4.2).
- `set_sprite_depth` (from the Layers spec) plus transforms is the full 3D billboard recipe: depth from distance, scale from distance.
- With the per-column depths a raycaster uses, a billboard is correctly clipped behind wall edges, because each wall column is its own layer z. (This needs depth-bucketed raycaster drawing; see Layers spec §10.2.)

---

## 9. Backward compatibility

| Rule | Guarantee |
|---|---|
| No `set_sprite_*` calls → identity transform | Uses the existing blit code path, so output is byte-identical |
| `move_sprite` with integer arguments | Same wire records as today (`OP_MOVE`) |
| `show_sprite` z, cel semantics | Unchanged |
| `dispose_sprite` / `dispose_all_sprites` | Also drop the instance's transform and its cache entries when the template is disposed |

Gate: the same per-frame hash suite as the Layers spec, run across all scripts in `scripts/main`.

---

## 10. Phase plan

| Phase | Scope |
|---|---|
| **S1. Scale, stretch + flip** | Transform state per instance, full `OP_TRANSFORM` record (rotation and tilt fields carried but rendered in S2), `set_sprite_scale` incl. `scale_y`, `set_sprite_flip`, `set_sprite_anchor`, `reset_sprite_transform`, flip fast path, upscale and downscale quality rules, sub-pixel moves |
| **S2. Rotation + tilt** | In-plane rotation in both directions, exact 90° multiples, supersampled rotation, `tilt_x`/`tilt_y` foreshortening with signed mirroring, back-face detection, `sprite_back_cel`, edge-on sliver, `set_sprite_angle`, `set_sprite_tilt`, full `set_sprite_transform` |
| **S3. Cache** | Transform cache and benchmarks on the Pi 4 |
| **S4. Authored cels** | `sprite_angle_cels`, `sprite_size_cel` |
| **S5. Perspective (optional)** | `set_sprite_perspective`: keystone a tilted sprite so its near edge is larger. Needs a projective inverse map; only worth doing if S2 tilts look flat on the panel. |

---

## 11. Test plan

### Unit
- Identity equals the existing blit, byte for byte.
- Flip H, V and HV against hand-computed expected buffers.
- 90°, 180° and 270° rotations are exact permutations.
- Scale 2.0 of a 4×4 checker gives exact 2×2 blocks.
- Scale 0.5 of a 1-pixel-wide line stays visible (the area-sampling threshold).
- Tiny-result rule: a 16×16 sprite at scale 0.05 produces one pixel in its average colour.
- Centre anchor: rotating about the pivot keeps the pivot pixel fixed.
- Angle sign: −90° equals 270°, and +90° followed by −90° returns the identity buffer.
- Tilt: `tilt_y` 60° halves the width; `tilt_y` 180° equals flip H; `tilt_x` 180° equals flip V; both at 180° equals a 180° rotation.
- Back face: with `sprite_back_cel` set, `tilt_y` 120° draws the back cel unmirrored at half width; without it, the mirrored front.
- Edge-on: `tilt_y` 90° gives a 1-px vertical sliver in the average colour; adding `angle` 90° makes it horizontal.
- Clipping: a partly off-panel rotated sprite is clipped without crashes, including negative coordinates.
- The cache returns identical pixels to an uncached render, and entries are evicted at the cap.
- Wire round-trip for `OP_TRANSFORM` and `OP_MOVE_SUB`, plus goldens.

### Visual
`scripts/test/sprite_transform_*.pix`: a scale ramp from 0.1 to 4, a full 360° spin at several sizes, flips, and a grid of instances with mixed transforms. Review on the panel to tune the §5 thresholds.

### Performance (Pi 4, fps 60)
40 instances of a 16×16 sprite, each spinning and scaling every frame. Target: consumer sprite work < 3 ms per frame with the cache warm.

---

## 12. Script ideas this unlocks

| Script | Uses | Idea |
|---|---|---|
| **Asteroid Field** | rotation, scale, depth | A ship turns to any heading and fires at tumbling rocks of several sizes. Rocks split into smaller spinning fragments when hit. |
| **Top-Down Rally** | rotation, angle cels | Cars with 16 hand-drawn headings race a winding track, drifting through corners. Dust puffs scale up and fade. |
| **Nova_Runner v2** | scale, depth, rotation | Pixel-art enemy ships grow from the horizon and roll as they weave. Alien trees and pillars are scaled sprites that pass behind each other. |
| **Doom_Tour with inhabitants** | scale, centre anchor, layers | Billboard creatures, barrels and lamps scaled by distance and hidden behind wall edges. |
| **Windmill Farm** | rotation, scale | Rolling hills with windmills at different distances, blades spinning at different speeds, clouds drifting over. |
| **Analog Clock Face** | rotation, centre anchor | Hour, minute and second hands as rotating sprites over a retained dial layer. Very little drawing per frame. |
| **Coin Shower** | tilt, scale, back cel | Gold coins tumble on both tilt axes, flashing their edges, with heads on the front and tails on a back cel. They bounce toward the viewer and scale up. |
| **Card Magic** | tilt, back cel, rotation | A deck fans out, cards flip face-up one by one with `tilt_y`, spin into a pile clockwise and counter-clockwise, then flip back. |
| **Falling Leaves** | both tilts, rotation, depth | Autumn leaves flutter down, tipping and turning on both axes while spinning, at several depths between tree layers. |
| **Departure Board** | tilt_x, back cel | Split-flap display: each tile tips forward on `tilt_x` to reveal the next letter on its back cel. |
| **Aquarium with depth** | scale, flip, layers | Fish flip to face their swim direction and scale with depth as they swim toward and away from the glass. |
| **Radar Station** | rotation | A rotating sweep sprite over a retained radar layer. Contacts light up as the sweep passes. |
| **Space Dogfight** | rotation, scale | Two ships circle and chase in a top-down arena, with scaled explosions and rotating debris. |

---

## 13. Open decisions

1. **Angle units:** degrees (proposed; friendlier for scripts) or radians to match `sin()`/`cos()`. Scripts already convert in other places, so degrees is fine; add `deg(r)` / `rad(d)` helpers if they don't exist.
2. **Default anchor:** keep top-left for compatibility (proposed), or make centre the default for transformed sprites only. The latter is convenient but surprising.
3. **Cache angle step:** 2° (proposed) vs 1°. Tune on the panel.
4. **Downscale threshold:** 40% (proposed). Tune with the visual test scripts.
5. **Back face default:** mirrored front (proposed; physically correct for a flat card), or hide the sprite when no back cel is set (like back-face culling).
6. **Tilt sign convention:** as in §3.4 (+ moves the right or top edge away). Confirm on the panel that it reads naturally.
7. **Perspective (S5):** decide after seeing S2 tilts on the real matrix.
