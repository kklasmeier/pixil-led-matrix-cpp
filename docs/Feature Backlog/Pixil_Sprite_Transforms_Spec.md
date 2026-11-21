# Pixil Spec: Sprite Transforms (Scale, Rotation, Tilt, Flip)

**Status:** S1–S3 implemented (transforms + cache). S4 authored cels optional polish.  
**Repo:** kklasmeier/pixil-led-matrix-cpp  
**Depends on:** Drawing layers (`Pixil_Layers_Spec.md`) — sprites already live on a layer; `z_index` only orders sprites on the same layer  
**Code areas:** interpreter command types, sprite batch emit/decode, sprite blit, docs, tests

---

## 1. What this is for

Today every sprite draws at its native size, upright, on whole-pixel positions. This feature lets each sprite instance:

- **Scale** — grow or shrink (0.1× to 8×), or stretch tall/wide
- **Rotate** — spin flat on the panel, any amount, either way (degrees)
- **Tilt** — tip like a card or coin in 3D (narrows, goes edge-on, shows the reverse)
- **Flip** — mirror left/right and/or up/down

Together with layers, scripts can build simple depth scenes: things that approach and grow, sit between background bands, and turn to face a heading.

### Goals

- One model covers scale, spin, tilt, and flip
- Small pixel art still looks readable when shrunk or turned (see §5)
- Existing scripts look identical (no transform = today’s draw path)
- A sprite that spins every frame stays cheap on the Pi once results are cached

### Not in scope

- True perspective (near edge bigger than far edge) — optional later phase S5
- Skew, or transforming scrolling backgrounds
- Soft blending into the scene — sprites stay opaque-or-transparent pixels

---

## 2. How it works today (before this feature)

| Piece | Today |
|---|---|
| Instance | Whole-pixel `x,y` (top-left), layer, `z_index`, which frame (cel), visible |
| Drawing | Copy pixels 1:1, skip clear pixels, apply each pixel’s brightness, clip to the panel |
| Messages | Show / move / hide batch records (or text commands outside a frame) |
| Script | `show_sprite` / `move_sprite` take **integer** `x,y` |

---

## 3. Script commands

Transform state is per **instance** (rock 0 and rock 1 can spin differently). Prefer the small helpers when changing one thing; use the full transform when setting several at once.

### 3.1 Commands

| Command | What it does |
|---|---|
| `set_sprite_scale(name, scale, [inst], [scale_y])` | Size. `1.0` = native. Optional `scale_y` for squash/stretch (horizontal = `scale`, vertical = `scale_y`). |
| `set_sprite_angle(name, angle, [inst])` | Flat spin in **degrees**. Positive = clockwise, negative = the other way. Any size is fine (`450` = one full turn + 90°). |
| `set_sprite_tilt(name, tilt_x, tilt_y, [inst])` | Tip in degrees (§3.5). |
| `set_sprite_flip(name, flip, [inst])` | `0` none, `1` horizontal, `2` vertical, `3` both. |
| `set_sprite_anchor(name, mode, [inst])` | `0` = top-left (default), `1` = centre. Centre is opt-in — nothing switches just because you scaled or spun. |
| `set_sprite_transform(name, scale, angle, [inst], [tilt_x], [tilt_y], [flip])` | Set several fields at once. Persists until changed. |
| `reset_sprite_transform(name, [inst])` | Back to scale 1, angle 0, no tilt, no flip. **Does not change the anchor.** |
| `sprite_back_cel(name, cel)` | Optional. After `endsprite`: which still frame to show when the reverse faces the viewer (cards, coins). |

**Argument types (match existing Pixil style):**

- Scale, angle, tilt, and sprite `x,y` → **Float**
- Instance id, cel, z, flip, anchor mode → **Int**
- There is no separate `Num` type

**Scale limits:** clamp to **0.1 … 8**. Negative scale is not allowed — use flip or tilt to mirror.

**Optional-arg tip:** instance id comes before tilt/flip (same pattern as `show_sprite`). If you use tilt or flip on `set_sprite_transform`, **always pass the instance id** so a tilt value is not mistaken for an instance. For everyday use, prefer `set_sprite_tilt` / `set_sprite_angle` / etc.

### 3.2 Anchor (where x,y points)

- **Top-left (default):** `x,y` is the top-left of the unscaled sprite, like today. Spin and scale still pivot around the sprite’s centre, and that centre stays where it would at 1:1 — so adding rotation later doesn’t jump the sprite.
- **Centre (opt-in):** call `set_sprite_anchor(name, 1)`. Then `x,y` is the pivot — best for spinning clocks, projected 3D positions, etc.

### 3.3 Positions with fractions

`show_sprite` / `move_sprite` / `move_sprites` accept float `x,y`.

- **No transform** (plain sprite): round to a whole pixel — same look as today
- **Has a transform:** keep fractions (about 1/16 pixel) so slow motion doesn’t stutter

### 3.4 Spin vs tilt (what you see)

| Control | What it looks like | Direction |
|---|---|---|
| `angle` | Spins flat, like a clock hand | + clockwise, − the other way |
| `tilt_y` | Turns left/right like a door or coin — narrows, edge-on at ±90°, reverse beyond | + right edge away, − left edge away |
| `tilt_x` | Tips forward/back like a falling sign — shortens, edge-on at ±90°, reverse beyond | + top edge away, − bottom edge away |

You can combine all three. Angles wrap every 360° (`450` looks like `90`). Animating past 360 keeps spinning forever.

### 3.5 Tilt details

**Shrinking as it tips:** width and height shrink by the cosine of the tilt (tip toward edge-on → thinner). Past 90°, that math goes negative — that means “show the reverse” (mirrored).

**What you see on the reverse:**

1. **Default:** keep playing the front animation, but **flipped** (mirrored). Animation does not pause.
2. **Optional `sprite_back_cel`:** when the reverse faces the viewer, show that **one still frame** instead (e.g. card back, coin tails). Front animation is only for the front. The back cel is not an animated strip in v1.

**Edge-on:** when the sprite would be thinner than one pixel, draw a 1-pixel sliver in the art’s average colour so a spinning coin flashes an edge instead of vanishing.

**Order applied:** flip → scale & tilt → flat spin → position. So a tilted sprite that you also spin turns around its own axes.

**Perspective:** none until optional S5 (near edge larger). Early phases keep simple foreshortening only.

### 3.6 Example: rocks in depth

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
            v_s = 40 / v_z[v_i]
            set_sprite_transform(rock, v_s * 0.25, v_t * 3 + v_i * 60, v_i)
            set_sprite_layer(rock, int(255 - v_z[v_i] * 4), v_i)
            move_sprite(rock, 32 + v_x[v_i] * v_s, 24 + v_y[v_i] * v_s, v_i)
        endfor v_i
    end_frame
endwhile
```

### 3.7 Example: card flip and coin spin

```pixil
# cel 0 = front, cel 1 = back pattern (optional reverse art)
define_sprite(card, 12, 16)
    sprite_cel(0)
        # ... front ...
    sprite_cel(1)
        # ... back ...
endsprite
sprite_back_cel(card, 1)

show_sprite(card, 32, 32, 0, 10, 0)
set_sprite_anchor(card, 1, 0)
show_sprite(coin, 16, 20, 0, 10, 0)
set_sprite_anchor(coin, 1, 0)

while true then
    v_t = v_t + 1 / v_pace
    begin_frame
        v_flip = min(max(sin(v_t * 0.03) * 1.6, -1), 1) * 90 + 90
        set_sprite_tilt(card, 0, v_flip, 0)
        set_sprite_tilt(coin, sin(v_t * 0.05) * 20, 0 - v_t * 6, 0)
        set_sprite_angle(coin, sin(v_t * 0.02) * 45, 0)
    end_frame
endwhile
```

---

## 4. How drawing works (implementer notes)

For a sprite with any transform (not the plain 1:1 case):

1. Work out width/height after scale and tilt. If tilt would mirror, fold that into the flip switches (sizes sent on the wire stay **positive** — see §6).
2. Pick the front frame, or the optional still back frame when the reverse faces us.
3. Figure out where the four corners land on the panel; clip to 64×64.
4. For each LED in that box, look up the matching source pixel(s) using the quality rules in §5.
5. Skip clear results. **Brightness:** darken each sample by its intensity first, then average those colours (same idea GPUs use when filtering).

Plain flip with no scale/spin can stay a fast exact mirror of today’s loop.

Math is ordinary floating point. A sprite can’t cover more than the whole panel, so work per sprite is bounded.

---

## 5. Quality on a 64×64 panel

Cheap “nearest pixel” looks bad when shrinking (features vanish) or spinning (shimmer). Use these rules:

| Situation | What to do |
|---|---|
| No transform | Today’s 1:1 copy (identical bytes) |
| Flip only | Exact mirror copy |
| Exactly 90° / 180° / 270°, scale 1 | Exact pixel rearrange (no blending) |
| Grow (scale ≥ 1), any angle | Sample 4 points inside each LED; pick the majority opaque colour. If ≥ 3 of 4 are clear → clear. Keeps edges crisp. |
| Shrink (from scale or tilt), any angle | Sample a small grid over the LED’s footprint (cap 16). Average opaque samples’ colours (intensity already baked in). Light the LED if ≥ ~40% of samples were opaque. |
| Edge-on (one side &lt; 1 px, other still visible) | 1-pixel sliver in the art’s average colour, rotated with `angle` |
| Tiny (result smaller than ~2×2) | One pixel in the average colour if enough of the art was opaque (~25%). Distant dots don’t disappear. |

Tune the 40% / 25% knobs on the real panel; keep them in one place.

### 5.1 Hand-drawn headings and small versions (phase S4 — polish)

Engine rotation of a tiny sprite is never as nice as art drawn at each heading. Later optional hooks:

- `sprite_angle_cels` — several hand-drawn directions around 360°; engine picks the nearest
- `sprite_size_cel` — a hand-drawn tiny version below a given scale

Not required for S1–S3.

---

## 6. Messages between interpreter and display

New batch record (also allowed as text outside a frame):

| Op | Carries | Notes |
|---|---|---|
| `OP_TRANSFORM = 5` | name, instance, scale_x, scale_y (positive fixed-point), angle, tilt_x, tilt_y, flags (flip H/V, centre-anchor), reserved | **Sizes are always positive.** When tilt (or the author) needs a mirror, set the flip flags. Angles in hundredths of a degree, wrapped to 0–360. Send only when something changed. Reserved byte for optional S5. |
| `OP_MOVE_SUB = 6` | like move, but `x,y` in 1/16 pixel | Only for instances that have a transform |

`sprite_back_cel` is a one-shot definition command (like `define_sprite`), not a per-frame transform.

The small `set_sprite_*` helpers update the interpreter’s copy of the transform and emit one `OP_TRANSFORM`.

Update the shared protocol, emitter, decoder, and golden tests.

---

## 7. Cache (so spinning stays cheap)

Spinning every frame would redo the same work. Cache painted results:

- **Key:** template, which cel, rough scale, rough angle, flip/anchor flags (tilt is already folded into effective scale + flip)
- **Rough steps:** scale in fine steps; angle about **2°** (fine enough at this resolution; tunable)
- **Value:** small pixel buffer + offset from the anchor
- **Limit:** about **2 MB**, drop least-recent; clear a template’s entries when it is redefined

Position is applied when placing the cached picture, so motion stays smooth even though scale/angle are rounded for the cache key.

After one full spin, a looping rotate is basically a normal blit.

---

## 8. Working with layers

No conflict — transforms and layers complement each other.

- A transformed sprite still draws on its layer (layer drawing first, then that layer’s sprites by `z_index`)
- Typical depth recipe: pick layer from distance, scale from distance, use `z_index` only for ties on the same layer
- Hide / delete / reorder layer behave as today; transform settings stay on the instance (`reorder_layer` moves the instance with the layer)

---

## 9. Compatibility

| Rule | Promise |
|---|---|
| Never call transform commands | Same pixels as today (same draw path) |
| Whole-number moves on plain sprites | Same messages as today |
| `show_sprite` z / cel meaning | Unchanged |
| Dispose sprite / dispose all | Drop that instance’s transform; drop cache entries when the template goes away |
| `reset_sprite_transform` | Clears scale, angle, tilt, flip — **anchor unchanged** |

Regression gate: same per-frame hash pass as layers, across `scripts/main`.

---

## 10. Build in phases

| Phase | Deliver |
|---|---|
| **S1. Scale, stretch, flip** | Per-instance transform state; full transform message (spin/tilt fields present but unused until S2); scale (+ stretch), flip, anchor, reset; flip fast path; grow/shrink quality; fractional moves |
| **S2. Rotation + tilt** | Flat spin both ways; exact 90° steps; tilted foreshortening + reverse side; optional still back cel; edge-on sliver; angle/tilt/full transform commands |
| **S3. Cache** | Cache + Pi 4 timings |
| **S4. Hand-drawn cels (polish)** | Angle headings + small-size art hooks |
| **S5. Perspective (optional)** | Near edge larger when tilted — only if S2 looks too flat on the panel |

---

## 11. Tests

### Unit

- Plain sprite matches today’s blit byte-for-byte
- Flip H / V / both match hand-checked buffers
- 90° / 180° / 270° are exact
- Scale 2 on a checkerboard → clean 2×2 blocks
- Scale 0.5 on a 1-pixel line still visible
- Very small scale → single average-colour pixel when appropriate
- Centre anchor: pivot pixel stays put while spinning
- `−90°` equals `270°`; +90 then −90 returns to start
- Tilt 60° ≈ half width; 180° matches a flip; both axes 180° matches a 180° spin
- With back cel: reverse shows that still art; without: flipped front (animation continues)
- Edge-on → 1-pixel sliver; spin turns its direction
- Partly off-panel spin clips safely
- Cache matches live render; eviction at the size cap
- Transform and sub-pixel move messages round-trip (goldens)

### On the panel

Test scripts: scale ramp, full spin at several sizes, flips, mixed grid. Tune quality thresholds by eye.

### Performance (Pi 4, 60 fps)

40× 16×16 sprites, each spinning and scaling every frame. Target: under ~3 ms sprite work per frame with a warm cache.

---

## 12. Shows this unlocks

| Idea | Uses |
|---|---|
| Asteroid field — tumble, split, ship heading | spin, scale, layers |
| Top-down rally — cars (hand-drawn headings in S4) | spin, scale |
| Nova Runner v2 — ships grow from the horizon | scale, spin, layers |
| Doom Tour props — barrels/lamps by distance | scale, centre anchor, layers |
| Windmill farm — blades and depth | spin, scale |
| Analog clock — hands on a retained dial | spin, centre anchor |
| Coin shower — tumble, edge flash, heads/tails | tilt, scale, back cel |
| Card magic — flip, fan, spin | tilt, back cel, spin |
| Falling leaves between tree layers | both tilts, spin, layers |
| Split-flap departure board | tilt_x, back cel |
| Aquarium — face swim direction, size by depth | scale, flip, layers |
| Radar sweep | spin |
| Space dogfight — chase, debris | spin, scale |

---

## 13. Decisions (locked) and leftover knobs

### Locked in review

1. **Degrees** for angle and tilt (`450` = full turn + 90°)
2. **Default anchor = top-left**; centre only via `set_sprite_anchor(..., 1)`
3. **Wire sizes always positive**; mirroring via flip flags (including tilt-induced reverse)
4. **`x,y` are Float**; round when untransformed, keep fractions when transformed; same for `move_sprites`
5. **Intensity:** apply to colour, then average samples
6. **Scale** clamped to 0.1…8; no negative scale
7. **Instance id** stays before optional tilt/flip; always pass it when using those; prefer small helpers
8. **Reset** clears scale/angle/tilt/flip, not anchor
9. **Back cel** optional still frame on reverse only; default reverse = flipped front animation
10. **Hand-drawn angle/size cels** deferred to S4

### Tune on the panel later

- Cache angle step (start at 2°)
- Downscale opacity threshold (start at 40%) and tiny-sprite threshold (25%)
- Whether tilt directions feel natural (+ right/top edge away)
- Whether S5 perspective is worth it after seeing S2
