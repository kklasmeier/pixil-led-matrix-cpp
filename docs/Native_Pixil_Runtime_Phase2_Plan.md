# Native Pixil Runtime — Phase 2 Plan (Consumer)

**Status:** C0–C5 done (owner panel switch still required for C6). Phase 1 producer is the daily show; Python `rgb_matrix_lib` still draws the panel by default.  
**Date:** 2026-11-17  
**Charter predecessor:** `docs/Native_Pixil_Runtime_Phase1_Plan.md` (especially M7 Phase 2 note)  
**Audience:** Implementation sessions in this repo  

---

## 0. Decisions already made

| Decision | Choice |
|----------|--------|
| Product goal | Replace Python **`rgb_matrix_lib`** with a **C++ consumer** that draws the panel |
| Replacement shape | **Full** port of consumer responsibilities — not a wrapper that still calls Python for drawing |
| Process model | Consumer stays a **separate process** (same as today’s bridge) |
| IPC | Keep **ADR-001** pipes (fds 3/4/5), framed records, reply semantics |
| Inbox | Keep the **5000-command** bounded inbox (backpressure via the data pipe) |
| Threads | **Reader** fills the inbox; **executor** parses and draws; matrix **swap only on the executor**; library refresh thread stays separate; **no multi-writer canvas** |
| Producer | **Do not change** language / emit / playlist behavior. Only a temporary spawn switch |
| Wire dialect (first cut) | Keep today’s **text commands** + base64 `draw_batch` / `plot_batch` / `sprite_batch`. Binary frames later if needed |
| `throttle` | **Gone** from the consumer (already driven as `throttle(0)` in Phase 1) |
| Legacy tree | **Read-only.** Python consumer stays installed as **oracle + fallback** until panel cutover |
| Live show | **Do not restart** unless the owner asks |
| Cutover switch | Temporary `--consumer python\|native` (default `python`). **Removed after cutover** so only the native consumer remains |
| Build topology | Edit on Pi 5 (NFS); **compile and run on Pi 4** only |

### Why a full C++ consumer

Phase 1 made the producer fast enough that the show is often **consumer-bound**. Gravity Plexus (`fps(60)`, `throttle(0)`, no `rest`) fills the 5000 inbox and holds near **~36 fps** at the Python draw rate. A native consumer frees the GIL-bound draw loop and uses idle Pi 4 cores.

### Cutover switch (temporary)

During development and A/B:

```text
pixil --consumer python …     # default: today’s bridge + rgb_matrix_lib
pixil --consumer native …     # new C++ binary behind the same ADR-001 pipes
```

After the owner declares the panel switched:

1. Default (and only) path is the native consumer.
2. Remove `--consumer` / the Python spawn path from the host.
3. Keep the legacy tree installed as a **manual fallback** (`~/Lightshow/git`) until the owner retires it — but this repo no longer launches it.

---

## 1. Problem statement

The native producer emits the same command stream the Python consumer understands. Once the inbox is full, panel FPS is limited by Python `rgb_matrix_lib` (parse + NumPy/PIL blit + GIL).

**Phase 2 success:** a C++ consumer process, spawned like today’s bridge, executes that dialect, drives `rpi-rgb-led-matrix`, matches Python visual semantics closely enough for the show library, and raises the consumer ceiling on scripts that already fill the inbox.

Phase 2 does **not** require changing `.pix` scripts or the producer’s command packing.

---

## 2. Architecture

```text
.pix scripts
    → Producer (DONE: native pixil in this repo)
         → ADR-001 pipes (unchanged)
    → Consumer (TODAY: Python bridge → rgb_matrix_lib
                PHASE 2: pixil-consumer C++ binary)
         → rpi-rgb-led-matrix → 64×64 panel
```

```text
[pixil-consumer]
  reader thread   → inbox (5000 framed items)
  control thread  → D / Q / X  (drain, queue sample, force shutdown)
  executor thread → dequeue → parse → draw → SwapOnVSync / rest / pump_fade
  (library)       → LED refresh thread (GPIO/PWM only; does not own drawing_buffer)
```

**Frozen:** producer command dialect for the first cut; ADR-001 framing; hardware options used by the show today.  
**Replaced:** everything inside the consumer process that today is `CommandExecutor` + `RGB_Api` + sprites / backgrounds / text / burnout.

### Hardware options (match Python)

```text
rows=64  cols=64  hardware_mapping=adafruit-hat
gpio_slowdown=3  scan_mode=1  pwm_bits=11  brightness=100
limit_refresh_rate_hz=0
```

### Dev topology (unchanged)

| Machine | Role |
|---------|------|
| **Pi 5** (Cursor) | Edit only — NFS client |
| **Pi 4** `192.168.1.58` | Disk + **build** + **run** (matrix) |

```text
Pi 4:  /home/pi/Lightshow
Pi 5:  /home/pi/Programming/piMatrix/Lightshow
```

```text
cd ~/Lightshow/pixil-led-matrix-cpp && nice -n 19 make -j2
```

`--headless` and `--dry-run` must not touch GPIO. Hardware runs need `sudo -n`.

---

## 3. Legacy reference map (read-only oracle)

| Area | Legacy location |
|------|-----------------|
| Consumer API | `rgb_matrix_lib/api.py` (`RGB_Api`) |
| Command parse / dispatch | `rgb_matrix_lib/commands.py` (`CommandExecutor`) |
| Shapes / burnout | `rgb_matrix_lib/drawing_objects.py`, `utils.py` |
| Sprites | `rgb_matrix_lib/sprite.py` |
| Backgrounds | `rgb_matrix_lib/background.py` |
| Text | `rgb_matrix_lib/text_renderer.py`, `bitmap_font.py`, `fonts.py`, `text_effects.py` |
| Fingerprint | `rgb_matrix_lib/test_inspect.py` |
| Emergency blackout | `rgb_matrix_lib/emergency_blackout.py` |
| Batch wire layouts | `shared/{draw_batch,mplot,sprite_batch}_protocol.py` |
| Today’s spawn target | `bridge/pixil_consumer_bridge.py` (this repo) |
| Dialect contract | `docs/COMMAND_DIALECT.md` |
| IPC contract | `docs/ADR-001-ipc.md` |

Python wins on visual/behavior conflicts unless the owner overrides; log intentional differences in `DEVIATIONS.md`.

---

## 4. Scope

### In scope

1. **C++ consumer binary** (`pixil-consumer`) that inherits ADR-001 fds and speaks the same replies
2. **Full draw surface** for the 31 consumer verbs in `COMMAND_DIALECT.md` §6
3. **Frame model:** `begin_frame(false|true)`, batches, sprites/text, `end_frame`, transparent `(0,0,1)` → black on present
4. **Pacing:** `fps`, `rest` (interruptible by drain/shutdown); no per-command throttle delay
5. **Sprites, backgrounds, text** parity needed by `scripts/main`
6. **Burnout** (instant + fade), pumped on the **executor** only
7. **Control plane:** fence, drain, script reset, snapshot fingerprint, graceful/force shutdown, blackout on producer death
8. **Headless stub** (no GPIO) for CI/smokes and fingerprint compare
9. **Temporary spawn switch** in `ConsumerLink` / CLI; removed after cutover
10. **ADR-002** documenting the consumer replacement and cutover

### Explicitly out of scope

- Changing the Pixil language or producer emit/packers (except the temporary spawn switch)
- Editing the legacy tree
- Binary IPC frames (defer; revisit only if text parse remains a measured bottleneck)
- New primitive verbs or `.pix` rewrites
- Restarting or switching the live daily show without owner request
- Deleting the legacy repo

### Purity / performance policy

- Prefer correct panel semantics first; speed comes from removing Python/GIL and drawing in C++
- Keep batch decode hot paths tight (`draw_batch` / `plot_batch` / `sprite_batch`)
- Do not invent a second command dialect in the first cut

---

## 5. Contracts

### 5.1 IPC (ADR-001 — unchanged)

Same frames as Phase 1. Ready `H` JSON should report `headless`, size, and that the consumer is native (e.g. `"impl":"native"`). Control `Q` still returns inbox depth/capacity.

### 5.2 Command dialect (unchanged wire)

Producer continues to send the Phase 1 dialect. Consumer must execute the verbs in `COMMAND_DIALECT.md` §6, including fast-path batch payloads and named/spectral colors.

Semantic anchors from M7 / show reality:

- `begin_frame(false)` → fresh frame buffer; `begin_frame(true)` → keep panel contents
- Filled **black** rectangle **replaces** pixels
- Black at any intensity is still black
- Transparent sentinel in the drawing buffer is `(0,0,1)` until present converts it to LED black

### 5.3 Host / ops

- Default remains Python until cutover
- `--consumer native` runs the C++ binary with the same fd handoff
- `--headless` never opens GPIO
- `--dry-run` does not start a consumer
- After cutover: remove the switch; update `OPS.md` / `CUTOVER.md`

---

## 6. Design

### 6.1 Suggested layout

```text
src/consumer/
  main.cpp           # argv: --headless, --record; require fds 3/4/5
  loop.cpp           # port of bridge loop (inbox + control + replies)
  inbox.hpp          # bounded queue, capacity 5000
  parse.cpp          # text commands + batch decode
  api.cpp / api.h    # RGB_Api equivalent
  matrix.cpp         # real rpi-rgb-led-matrix vs headless stub
  draw.cpp           # plot/line/rect/circle/… into drawing_buffer
  sprite.cpp
  background.cpp
  text.cpp
  burnout.cpp        # executor-pumped fades/expirations
  fingerprint.cpp
docs/ADR-002-native-consumer.md
```

Link `librgbmatrix` from the Pi’s `rpi-rgb-led-matrix` install. Reuse `src/emit/` record layouts / color ids for decode where practical.

### 6.2 Threading rules

| Thread | May do | Must not |
|--------|--------|----------|
| Reader | Read data pipe; `inbox.put` (block at 5000) | Touch drawing_buffer / canvas |
| Control | Set drain/shutdown; answer `Q` | Touch drawing_buffer / canvas |
| Executor | Parse, draw, composite, `SwapOnVSync`, `rest`, pump burnout | Share canvas writes with other app threads |
| Library refresh | PWM/GPIO refresh | Own application drawing state |

**Burnout difference from Python (intentional):** Python’s `ThreadedBurnoutManager` mutates buffers from another thread. Native burnout bookkeeping may wake on a timer, but **all buffer/canvas writes run on the executor** (idle `pump_fade`, `rest`, and presents). Document as a deviation if timing differs slightly; visual fade/instant behavior is the acceptance bar.

### 6.3 Control-plane behavior (match bridge)

Port `bridge/pixil_consumer_bridge.py` loop semantics:

- Drain: swallow `C` until `K`; interrupt `rest`
- Force `X`: immediate blackout + reply `X`
- Graceful data `X`: finish policy matching today’s bridge, blackout, reply `X`
- Data EOF: blackout and exit
- `R`: reset_fps, end open frame, clear, dispose_all_sprites
- `T`: drawing-buffer fingerprint (headless-comparable)
- Fatal command error: reply `E`, blackout, exit

### 6.4 Producer spawn (temporary)

`ConsumerLink` gains a consumer kind:

- `python` → today’s `python3 -u bridge/pixil_consumer_bridge.py …`
- `native` → `build/…/pixil-consumer --headless? …` with the same fd mapping

No other producer behavior changes. After cutover, delete the Python branch and the flag.

### 6.5 What not to do

- Do not embed the consumer in the producer process
- Do not attach to Python `multiprocessing.Queue`
- Do not keep calling into `rgb_matrix_lib` from the native binary
- Do not transliterate Python line-by-line without fixing the burnout threading model
- Do not compile on the Pi 5 against the NFS tree

---

## 7. Build plan (milestones)

Lean gates. Each milestone leaves a runnable binary or a stronger contract. Prefer vertical slices.

### C0 — Scaffold + ADR + spawn switch

- [x] `src/consumer/` skeleton; Makefile target `pixil-consumer`
- [x] Link/detect `librgbmatrix` on the Pi 4 (headless build must still work without opening GPIO)
- [x] ADR-002: native consumer + temporary `--consumer` switch
- [x] `ConsumerLink` / CLI: `--consumer python|native` (default **python**)
- [x] Native process: inherit fds, send ready `H`, answer `Q`, exit cleanly on data EOF with stub blackout

**Gate:** `pixil --consumer native --hello --headless` gets a ready frame (drawing may still be stubbed). Daily show default unchanged.

### C1 — Hello path (first milestone)

- [x] Executor + reader + 5000 inbox
- [x] Commands: `clear`, `sync_queue`, `rest`, `fps`, `begin_frame` / `end_frame`, `draw_rectangle`, `draw_batch`, `plot` as needed for `--hello`
- [x] Drawing buffer + present (transparent → black); headless stub canvas
- [x] Drain / fence / script reset / graceful + force shutdown / blackout
- [x] Optional `--record` JSONL of executed commands

**Gate:** `pixil --hello --headless --consumer native` matches the Phase 1 hello control-plane drills. On owner request only: short `sudo` hardware hello **without** touching the daily show.

### C2 — Core draw dialect

- [x] Remaining shape verbs: line, circle, ellipse, polygon, arc, `plot_batch`
- [x] Color / intensity / burnout fields decoded (burnout may no-op until C4 if needed, but must not crash)
- [x] Unit tests for batch decode vs existing goldens / emit layouts

**Gate:** Headless run of a small pure-draw script set under `--consumer native` completes without consumer errors.

### C3 — Sprites, backgrounds, text

- [x] Sprite define/cel/draw/show/hide/move/dispose + `sprite_batch`
- [x] Background set/hide/nudge/offset
- [x] `draw_text` / `clear_text` (fonts from the same sources the show uses)
- [x] Frame composite order matches Python (`COMMAND_DIALECT.md` + `api.end_frame`)

**Gate:** Representative sprite/text scripts run headless under native consumer; fingerprints stable across runs with a fixed seed/stream.

### C4 — Burnout + idle pump

- [x] Instant and fade burnout, executor-pumped
- [x] `pump_fade` while inbox idle; `rest` keeps fades alive and stays abortable

**Gate:** A burnout/fade smoke (headless record or fingerprint) behaves like the Python oracle within documented tolerance.

### C5 — Parity hardening

- [x] Stream → fingerprint compare: Python consumer vs native on agreed captures
- [x] Document deviations (`DEVIATIONS.md`)
- [x] Inbox/`-q` behavior unchanged from the producer’s point of view
- [x] Emergency blackout path verified for native (and Python fallback still works while the switch exists)

**How:** `pixil --fingerprint` (ADR-001 `T` before script reset);  
`make consumer-parity` → `tools/consumer_parity_compare.sh`;  
`make c5-ops` → `tools/verify_c5_ops.sh`. Phase 2 deltas: D-010…D-016.  
C5 fixes: `draw_batch` intensity `0→100` (D-015); burnout point dedupe for fade
clear (D-016). `Abstract_Geometry` left out of hard MATCH gate (D-010 race).

**Gate:** Owner-approved parity on the compare set; native is eligible for a deliberate panel switch.

### C6 — Panel cutover

- [ ] Owner-authorized A/B on hardware (not the unattended daily show until signed off)
- [ ] Switch daily show to `--consumer native` (or make native the default)
- [ ] Watch consumer-bound scripts (e.g. Gravity Plexus): inbox pressure and achievable fps
- [ ] **Remove the switch:** native only; update `CUTOVER.md` / `OPS.md` / README
- [ ] Leave legacy tree installed as manual fallback until owner retires it

**Gate:** Daily show runs from this repo with the C++ consumer; Python consumer is fallback only, not launched by default.

---

## 8. Testing strategy

Parity against the **Python consumer** (still the oracle), not against rewriting scripts.

| Tier | What | When |
|------|------|------|
| A | Unit: batch decode, color, clipping helpers | C1–C2 |
| B | Headless control-plane: hello, drain, reset, snapshot, blackout | C1 |
| C | Command-stream replay → fingerprint vs Python bridge | C3–C5 |
| D | Short hardware smokes (owner-requested only) | C1 optional, C6 |
| E | Consumer-bound fps / inbox observation on Pi 4 | C6 |

Prefer semantic compare (fingerprints, structural traces) over brittle frame dumps. `--headless` is the default development path so the live show can keep running on Python.

---

## 9. Risks

| Risk | Mitigation |
|------|------------|
| Visual drift (AA, text, sprite z-order, preserve frames) | Fingerprint + owner eye on a short list; DEVIATIONS |
| Burnout timing vs Python’s background thread | Executor pump; document tolerance |
| Accidentally stealing the panel from the daily show | Default `--consumer python`; no unsolicited `./run` restart |
| Linking / root / GPIO differences | Match options; headless CI path; `sudo -n` only for hardware |
| Scope creep into binary IPC | Deferred until text parse is measured as the limit |
| Dual-maintenance of spawn paths | Switch is temporary; C6 deletes it |

---

## 10. Checklist — ready to code C0/C1

- [ ] Phase 1 daily show still healthy on Python consumer
- [ ] This document accepted as the Phase 2 charter
- [ ] Cutover switch shape confirmed: temporary `--consumer`, removed after cutover
- [ ] First milestone confirmed: **C1 hello path** (frames + `draw_batch`)
- [ ] Pi 4 has `rpi-rgb-led-matrix` headers/libs for the native link
- [ ] Agree: no live-show restart until owner asks

---

## 11. Suggested first prompt for the next session

> Implement **Phase 2 C0 + C1** per `docs/Native_Pixil_Runtime_Phase2_Plan.md`.  
> Full C++ replacement of `rgb_matrix_lib` behind ADR-001 — not a Python wrapper.  
> Add temporary `--consumer python|native` (default python).  
> Deliver headless hello: inbox, frames, `draw_batch`, drain/reset/shutdown/blackout.  
> Do not edit the legacy tree. Do not restart the live show. Build only on the Pi 4.

---

## 12. One-page summary

- Phase 2 = **C++ `rgb_matrix_lib`**, separate process, same pipes, same commands.  
- Reader → 5000 inbox; executor draws and swaps; no multi-writer canvas.  
- Producer dialect frozen; temporary `--consumer` switch; **delete the switch after cutover**.  
- Milestone order: scaffold → **hello/frames/`draw_batch`** → shapes → sprites/text → burnout → parity → panel switch.  
- Python consumer remains oracle/fallback until the owner switches the panel on purpose.
