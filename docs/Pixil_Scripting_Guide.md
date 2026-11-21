# PIXIL SCRIPTING GUIDE

This is the scripting guide for the native producer in this repository.
The legacy guide remains `docs/Pixil_Scripting_Guide.txt` in the old tree and
describes that producer, including the builtin particle, agent, ink, and
chladni commands. Those commands are not part of this language.


Pixil is a scripting language for creating animations on a 64x64 RGB LED matrix 
display. Scripts run on a Raspberry Pi 4, which has limited CPU power, so 
efficient scripting practices matter for smooth animations.

## HARDWARE CONTEXT
- Display: 64x64 RGB LED matrix (4,096 pixels)
- Controller: Raspberry Pi 4
- Coordinates: x and y range from 0-63, origin (0,0) is top-left
- Performance: CPU-limited; complex math and large loops affect frame rate

## TABLE OF CONTENTS

<a id="table-of-contents"></a>

1. [Quick Reference](#1-quick-reference)
2. [How Pixil Runs (Producer / Consumer)](#2-how-pixil-runs-producer--consumer)
3. [Drawing Commands](#3-drawing-commands)
4. [Colors](#4-colors)
5. [Burnout System](#5-burnout-system)
6. [Variables and Expressions](#6-variables-and-expressions)
7. [Control Structures](#7-control-structures)
8. [Arrays](#8-arrays)
9. [Math Functions](#9-math-functions)
10. [Procedures and Imports](#10-procedures-and-imports)
11. [Frame Control](#11-frame-control)
12. [Batch Plotting](#12-batch-plotting-mplot--mflush)
13. [Text Commands](#13-text-commands)
14. [Sprites](#14-sprites)
15. [Drawing Layers](#15-drawing-layers)
16. [Background Layers](#16-background-layers)
17. [Performance Commands](#17-performance-commands)
18. [Debugging](#18-debugging)
19. [Common Errors and Troubleshooting](#19-common-errors-and-troubleshooting)
20. [Performance Tips](#20-performance-tips)
21. [Native Grid and Field Programs](#21-native-grid-and-field-programs)
22. [Particle library](#22-particle-library)
23. [Agent, ink, chladni, and points libraries](#23-agent-ink-chladni-and-points-libraries)
24. [Script Examples](#24-script-examples)

To run a script on the matrix Pi:

```text
ssh pi@192.168.1.58
cd ~/Lightshow/pixil-led-matrix-cpp
./run start -- main/<filename> -t 5:00
```

`./run` with no arguments plays every script in `scripts/main`. See `docs/OPS.md`.

# 1. QUICK REFERENCE

[↑ Table of Contents](#table-of-contents)

Key Rules:
- All commands must be on a single line (no line wrapping)
- Variables must be prefixed with v_ (e.g., v_x, v_color)
- Variables are case sensitive
- Color names and sprite names are NOT wrapped in quotes in commands
- String values in variable assignments ARE wrapped in quotes

Basic Example:
```text
# Draw a red circle that fades out over 2 seconds
draw_circle(32, 32, 10, red, 100, true, 2000, fade)

# Animation loop
for v_i in (0, 63, 1)
    plot(v_i, 32, blue, 100, 500, fade)
    rest(0.05)
endfor v_i
```

Native grid and field programs stay built into the language. See section 21.

```text
grid_program life { size 32 cell 2 fields v_grid steps 1 boundary clamp
    step v_grid { v_n = neighbors(v_grid)
        v_grid_next = where(v_n == 3, 1, where(at(v_grid)==1 and v_n==2, 1, 0)) }
    draw { fill_field v_grid fill_color cyan fill_intensity 90 } }
while true then
    begin_frame
        grid_step(life)
    end_frame
endwhile
```

Particle, agent, ink, chladni, and points behavior lives in Pixil libraries
under `scripts/lib/`. A script imports one and calls its procedures. The
procedures mutate arrays in place and do not draw, except `chladni.step` and
`ink.step`, which plot. See sections 22 and 23.

```text
import particle

create_array(v_x, 16, numeric)
create_array(v_y, 16, numeric)
create_array(v_vx, 16, numeric)
create_array(v_vy, 16, numeric)
create_array(v_active, 16, numeric)
call particle.integrate(v_x, v_y, v_vx, v_vy, v_active, 0, 0.15)
call particle.collide_bounds(v_x, v_y, v_vx, v_vy, v_active, 2, 61, 2, 61, 0.96, 1.5)
```

```text
import agent

call agent.chemotaxis(v_x, v_y, v_dir, v_phero, v_size, 5, 1, 0.55, v_n, 1)
grid_step(slime)
```

# 2. HOW PIXIL RUNS (PRODUCER / CONSUMER)

[↑ Table of Contents](#table-of-contents)

Your `.pix` script does not paint the LEDs directly. Two programs share the work:

```text
  .pix script
       │
       ▼
   pixil  (producer)     parses the script, runs loops/math, queues draw commands
       │
       │  command queue (inbox)
       ▼
   pixil-consumer        owns the panel (or a headless buffer) and draws
       │
       ▼
   LED matrix
```

**Producer (`pixil`)** — executes your script: variables, `if`/`for`/`while`,
procedure calls, `print`. When you `plot`, `draw_*`, `show_sprite`,
`end_frame`, and so on, it **enqueues** work for the consumer. It can run
ahead of what is on screen.

**Consumer (`pixil-consumer`)** — pulls from the queue and updates pixels.
Burnouts and presents happen here. If the inbox fills up, the consumer is
the bottleneck (see `./run` with `-q` in [docs/OPS.md](OPS.md)).

### Why this matters when you write scripts

| What you notice | Why |
|---|---|
| `print` seems “early” | Prints run in the producer immediately; draws may still be queued. Use `sync_queue` before a print that must match the panel ([§19](#19-common-errors-and-troubleshooting)). |
| Clocks / `get_datetime` look frozen | Without `fps` or `rest`, the producer can queue many frames in one wall-clock second. Pace with `fps` ([§17](#17-performance-commands)). |
| `begin_frame` / `end_frame` | The producer batches draws; `end_frame` sends them as one unit so the consumer presents a coherent frame. |
| `rest(1)` actually waits ~1s | The producer sleeps too, so script time and wall time stay aligned. |

You do **not** need to configure this pair yourself: `./run` starts the
producer, which spawns the consumer. For build/run details see
[docs/OPS.md](OPS.md); for the IPC design see
[ADR-001](ADR-001-ipc.md) and [ADR-002](ADR-002-native-consumer.md).

# 3. DRAWING COMMANDS

[↑ Table of Contents](#table-of-contents)

All drawing commands use this pattern:
```text
command(position, size, color, [intensity], [options], [burnout], [burnout_mode])
```

Brackets indicate optional parameters.

## PLOT (Single Pixel)
```text
plot(x, y, color, [intensity], [burnout], [burnout_mode])
```

Examples:
```text
plot(32, 32, red, 100)                    # Permanent red pixel
plot(32, 32, blue, 75, 2000)              # Blue at 75%, disappears after 2s
plot(32, 32, green, 100, 1000, fade)      # Green that fades over 1s
```

## LINE
```text
draw_line(x1, y1, x2, y2, color, [intensity], [burnout], [burnout_mode])
```

Examples:
```text
draw_line(0, 0, 63, 63, yellow, 100)              # Diagonal line
draw_line(10, 10, 50, 10, white, 80, 2000, fade)  # Fading horizontal line
```

## RECTANGLE
```text
draw_rectangle(x, y, width, height, color, [intensity], [filled], [burnout], [burnout_mode])
```

Examples:
```text
draw_rectangle(10, 10, 20, 15, green, 100, true)           # Filled rectangle
draw_rectangle(10, 10, 20, 15, blue, 100, false)           # Outline only
draw_rectangle(10, 10, 20, 15, red, 100, true, 1500)       # Disappears after 1.5s
```

## CIRCLE
```text
draw_circle(x, y, radius, color, [intensity], [filled], [burnout], [burnout_mode])
```

Examples:
```text
draw_circle(32, 32, 10, blue, 100, true)              # Filled blue circle
draw_circle(32, 32, 15, red, 100, false, 3000, fade)  # Outline that fades
```

## POLYGON
```text
draw_polygon(x, y, radius, sides, color, [intensity], [rotation], [filled], [burnout], [burnout_mode])
```

Parameters:
- sides: Number of sides (3=triangle, 4=square, 5=pentagon, 6=hexagon, etc.)
- rotation: Degrees clockwise rotation

Examples:
```text
draw_polygon(32, 32, 15, 6, purple, 100, 0, true)     # Filled hexagon
draw_polygon(32, 32, 20, 3, yellow, 100, 30, false)   # Rotated triangle outline
```

## ELLIPSE
```text
draw_ellipse(x_center, y_center, x_radius, y_radius, color, [intensity], [filled], [rotation], [burnout], [burnout_mode])
```

Examples:
```text
draw_ellipse(32, 32, 20, 10, cyan, 100, true, 0)           # Horizontal ellipse
draw_ellipse(32, 32, 10, 20, orange, 100, false, 45, 2000) # Rotated outline
```

## ARC
```text
draw_arc(x1, y1, x2, y2, bulge, color, [intensity], [filled], [burnout], [burnout_mode])
```

Bulge Parameter:
- Positive bulge: Arc curves left (port side) when traveling from start to end
- Negative bulge: Arc curves right (starboard side)
- Zero bulge: Straight line
- Larger values: More pronounced curve

Examples:
```text
draw_arc(10, 32, 54, 32, 15, red)                     # Arc curving up
draw_arc(10, 32, 54, 32, -15, blue)                   # Arc curving down
draw_arc(10, 32, 54, 32, 12, cyan, 75, true)          # Filled arc
```

## CLEAR
```text
clear()
```

Clears every drawing layer’s pixels, hides backgrounds, clears burnout, and
resets the current layer to 0. Sprite templates and instances survive; layer
slots stay allocated (empty). Between playlist scripts the consumer also
disposes all drawing layers so the next show starts clean.

# 4. COLORS

[↑ Table of Contents](#table-of-contents)

## Named Colors (exactly 44)

These are the **only** valid named colours. Names are case-insensitive.
There is no `dark_green`, `dark_red`, `light_pink`, `cream`, or similar
aliases — use a listed name or a spectral number 0–99.

```text
transparent    black         white         gray          light_gray
dark_gray      silver        red           crimson       maroon
rose           pink          salmon        coral         brown
standard_brown dark_brown    wood_brown    tan           orange
gold           peach         bronze        yellow        lime
green          olive         spring_green  forest_green  mint
teal           turquoise     cyan          sky_blue      azure
blue           navy          royal_blue    ocean_blue    indigo
purple         violet        magenta       lavender
```

(`forest_green` and `olive` are the dark greens; `maroon` / `crimson` for
dark reds; `peach` / `salmon` / `rose` for light pinks.)

## Using Colors
```text
# Direct color name
plot(10, 20, red, 100)

# Numeric color (0-99)
plot(10, 20, 45, 100)

# Color stored in variable (must use quotes when assigning)
v_color = "blue"
plot(10, 20, v_color, 100)

# Numeric color in variable
v_color = 45
plot(10, 20, v_color, 100)
```

## Intensity
- Intended range: **1–100** (100 is full brightness)
- Optional; omit it to get full brightness (100)

**Pitfall:** for normal drawing commands (`plot`, `draw_*`, …) inside a
frame, intensity is truncated toward zero, and a packed value of **0 is
treated as 100** (legacy `intensity or 100`). So `0`, `0.5`, or any value
in `0 … <1` can appear at **full brightness**, not dark. Use at least `1`
when you mean “barely lit.” (`mplot` keeps byte `0` as true zero.)

Examples:
```text
plot(10, 20, red, 100)         # Full brightness
plot(10, 20, red, 50)          # Half brightness
plot(10, 20, red, v_intensity) # Variable intensity — keep >= 1 if dimming
```

## Transparent Color
The transparent color erases pixels, allowing background layers to show through.

```text
plot(10, 20, transparent)
```

# 5. BURNOUT SYSTEM

[↑ Table of Contents](#table-of-contents)

Burnout automatically removes drawn objects after a specified time. This is 
essential for animations as it eliminates manual tracking of pixels.

Burnout lives on the **drawing layer that was current** when the pixel was
drawn (`set_layer`). Overlay ownership among fading pixels stays within that
layer; a burnout on layer 10 never jumps in front of layer 20. See section 15.

## Parameters
- burnout: Duration in milliseconds before removal (-1 for permanent)
- burnout_mode: "instant" (default) or "fade"

## Modes
- instant: Object disappears immediately after duration
- fade: Object gradually fades to black over the duration

## Examples
```text
# Permanent (no burnout)
plot(32, 32, red, 100)

# Disappears after 2 seconds
plot(32, 32, red, 100, 2000)

# Fades out over 2 seconds
plot(32, 32, red, 100, 2000, fade)

# Explicit instant mode
plot(32, 32, red, 100, 2000, instant)

# Permanent with explicit -1
plot(32, 32, red, 100, -1)
```

## Shape Examples
```text
# Circle that fades
draw_circle(32, 32, 10, blue, 100, true, 3000, fade)

# Rectangle that disappears instantly
draw_rectangle(10, 10, 20, 15, green, 100, true, 1500, instant)

# Line that fades
draw_line(0, 0, 63, 63, yellow, 100, 2000, fade)

# Polygon that fades
draw_polygon(32, 32, 15, 6, purple, 100, 0, true, 4000, fade)
```

## Practical Animation Examples
```text
# Trailing effect - particles that fade behind a moving object
for v_i in (0, 50, 1)
    v_x = 10 + v_i
    plot(v_x, 32, orange, 100, 500, fade)
    rest(0.05)
endfor v_i

# Explosion effect - expanding ring that fades
for v_radius in (1, 20, 1)
    draw_circle(32, 32, v_radius, red, 100, false, 800, fade)
    rest(0.03)
endfor v_radius

# Comet trail
for v_frame in (0, 60, 1)
    v_x = v_frame
    plot(v_x, 32, white, 100, 100, instant)      # Bright head
    plot(v_x - 1, 32, yellow, 80, 400, fade)     # Trail
    plot(v_x - 2, 32, orange, 60, 400, fade)
    plot(v_x - 3, 32, red, 40, 400, fade)
    rest(0.05)
endfor v_frame
```

# 6. VARIABLES AND EXPRESSIONS

[↑ Table of Contents](#table-of-contents)

## Variable management (read this)

Pixil has **one rule with one exception**:

| Where you are | Scalars (`v_x = …`) | Arrays (`v_a[i] = …`) |
|---|---|---|
| Main script, loops, `if`/`while` | **Global** — one shared name for the whole script | **Global** — one array object |
| Procedure with **no** parameters `def foo { }` | **Global** — same names as the script; changes stick | **Global** — same |
| Procedure **with** parameters `def foo(v_a, …) { }` | **Local** for parameters and any name that procedure assigns — restored when the call returns | Argument arrays are **shared** (not copied). Cell writes stick in the caller’s array |

There is no `local` keyword. You only get locals by declaring parameters on
`def`. A zero-argument procedure never gets a private scope.

**Why arrays are shared:** Pixil has no `return` value. Libraries pass
results back by writing into arrays you hand them
(`call particle.integrate(v_x, …)`). Copying whole arrays on every call
would also be expensive on a Pi. Scalars are cheap to copy/restore; array
*contents* are updated in place on purpose.

Full examples: [§10 Procedures — Scope](#scope-this-matters) and
[Arrays are shared](#arrays-are-shared-how-results-come-back).

## Variable Naming
All variables must be prefixed with v_ and are case sensitive.

```text
v_x = 10            # Numeric
v_color = "red"     # String (must use quotes)
v_delay = 2.5       # Decimal
v_text = "Hello"    # String
```

## Variable Assignment
Each assignment must be on its own line.

```text
v_x = 10
v_y = 20
v_result = v_x + v_y
```

## Math in Assignments
```text
v_x = v_x + 10
v_y = v_size * 2
v_z = abs(v_x - 25)
v_angle = sin(v_time) * 30
```

## Inline Math in Commands
Math expressions can be used directly in command parameters.

```text
plot(v_x + 5, v_y * 2, red, 100)
draw_circle(v_x - 10, v_y / 2, v_size + 1, blue, 100, true)
draw_line(v_x, v_y, v_x + 10, v_y + 10, yellow, 100)
```

## Examples with Variables
```text
v_x = 32
v_y = 32
v_radius = 10
v_color = "blue"
v_intensity = 75

draw_circle(v_x, v_y, v_radius, v_color, v_intensity, true)
draw_circle(v_x + cos(v_angle) * 20, v_y + sin(v_angle) * 20, 5, red, 100, true)
```

# 7. CONTROL STRUCTURES

[↑ Table of Contents](#table-of-contents)

## IF/THEN/ELSE

Basic syntax:
```text
if <condition> then
    # commands
endif
```

With else:
```text
if <condition> then
    # commands if true
else
    # commands if false
endif
```

With elseif:
```text
if <condition> then
    # commands
elseif <condition> then
    # commands
else
    # commands
endif
```

Comparison Operators:
==    Equal to
!=    Not equal to
>     Greater than
<     Less than
>=    Greater than or equal
<=    Less than or equal

Logical Operators:
and   Both conditions must be true
or    At least one condition must be true
not   Negates the following condition

Examples:
```text
if v_x > 10 then
    plot(v_x, v_y, red, 100)
endif

if v_count < 5 then
    draw_circle(v_x, v_y, 5, red, 100, true)
else
    draw_circle(v_x, v_y, 10, blue, 100, true)
endif

if v_value < 10 then
    draw_text(10, 20, "Low", tiny64_font, 12, red, 100)
elseif v_value < 20 then
    draw_text(10, 20, "Medium", tiny64_font, 12, yellow, 100)
else
    draw_text(10, 20, "High", tiny64_font, 12, green, 100)
endif
```

Compound Conditions:
```text
if v_x > 5 and v_y < 20 then
    draw_circle(v_x, v_y, 5, red, 100, true)
endif

if v_x < 5 or v_y > 30 then
    draw_rectangle(v_x, v_y, 10, 10, blue, 100, true)
endif
```

Operator Precedence:
- AND has higher precedence than OR
- Use parentheses to override default precedence

Parentheses Support:
Parentheses can be used to group conditions and override operator precedence.
Multiple non-nested parentheses groups are supported.

Examples with Parentheses:
```text
# Override precedence - evaluate OR first, then AND with result
if (v_x > 5 or v_y < 10) and v_z == 15 then
    # Executes if: (v_x > 5 OR v_y < 10) is true AND v_z == 15
endif

# Multiple parenthesized groups
if (v_x > 5 and v_y < 10) or (v_z == 15) then
    # Executes if: either group is true
endif

# Mixed: simple condition with parenthesized group
if v_a == 1 and (v_x > 5 or v_y > 100) then
    # Executes if: v_a == 1 AND (v_x > 5 OR v_y > 100)
endif

# Three groups
if (v_x > 5) or (v_y > 100) or (v_z == 15) then
    # Any group being true makes the whole condition true
endif

# Complex compound conditions inside parentheses
if (v_x > 5 and v_y < 10) or (v_z > 10 and v_a == 1) then
    # Each group is evaluated separately, then combined with OR
endif
```

Note: Nested parentheses are NOT supported. For example:
# NOT supported: ((v_x > 5 or v_y < 10) and v_z == 15) or v_a == 1
# Use multiple if statements for complex nested logic instead

NOT Operator:
The 'not' operator negates a condition. It has higher precedence than 'and' and 'or'.

Examples with NOT:
```text
# Negate a boolean variable
if not v_flag then
    # Executes if v_flag is false (0 or falsy)
endif

# Negate a comparison
if not v_x > 5 then
    # Same as: v_x <= 5
endif

# Negate a parenthesized group
if not (v_x > 5 and v_y < 10) then
    # Executes if the group is false
endif

# Combined with AND
if v_a == 1 and not v_flag then
    # v_a must be 1 AND v_flag must be false
endif

# Combined with OR
if v_x > 100 or not v_active then
    # True if v_x > 100 OR v_active is false
endif

# Multiple NOT operators
if not v_flag and not v_disabled then
    # Both flags must be false
endif

# NOT with parenthesized group in compound
if (v_x > 5) and not (v_y < 10 or v_z == 0) then
    # v_x > 5 AND the parenthesized group must be false
endif
```

## FOR LOOPS

Syntax:
```text
for <variable> in (<start>, <end>, <step>)
    # commands
endfor <variable>
```

Parameters support numbers, variables, and expressions.

Examples:
```text
for v_x in (0, 63, 1)
    plot(v_x, 32, red, 100)
endfor v_x

for v_i in (0, 10, 2)
    draw_circle(v_i * 5, 30, 3, blue, 100, true)
endfor v_i

# With expressions
for v_i in (v_start, v_end, v_step)
    plot(v_i, v_y, green, 100)
endfor v_i
```

Use break to exit the innermost for or while loop early (compiled and interpreted).
```text
break inside if still exits the enclosing loop; break outside any loop is ignored.
```

## WHILE LOOPS

Syntax:
```text
while <condition> then
    # commands
endwhile
```

While loops support all condition features: compound conditions, parentheses, and NOT.

Basic Examples:
```text
v_x = 0
while v_x < 64 then
    plot(v_x, 32, red, 100)
    v_x = v_x + 1
endwhile

v_running = 1
while v_running then
    draw_circle(v_x, v_y, 5, blue, 100, true)
    v_x = v_x + 1
    if v_x > 63 then
        v_running = 0
    endif
endwhile
```

Compound Conditions:
```text
# With AND - both conditions must be true
while v_x < 64 and v_active == 1 then
    plot(v_x, 32, green, 100)
    v_x = v_x + 1
endwhile

# With OR - either condition can be true
while v_x < 32 or v_y < 32 then
    v_x = v_x + 1
    v_y = v_y + 1
endwhile
```

With NOT Operator:
```text
# Loop while v_done is false (0)
while not v_done then
    # do work
    if v_count >= 10 then
        v_done = 1
    endif
    v_count = v_count + 1
endwhile

# Combined with AND
while v_x < 64 and not v_stopped then
    plot(v_x, 32, blue, 100)
    v_x = v_x + 1
endwhile
```

With Parentheses:
```text
# Group conditions for proper precedence
while (v_x < 32 or v_y < 32) and v_active then
    v_x = v_x + 1
    v_y = v_y + 1
endwhile

# NOT with parenthesized group
while not (v_x > 60 and v_y > 60) then
    v_x = v_x + 1
    v_y = v_y + 1
endwhile
```

Nesting:
All control structures can be nested within each other.

```text
for v_y in (0, 63, 8)
    for v_x in (0, 63, 8)
        if v_x + v_y < 64 then
            plot(v_x, v_y, red, 100)
        else
            plot(v_x, v_y, blue, 100)
        endif
    endfor v_x
endfor v_y
```

# 8. ARRAYS

[↑ Table of Contents](#table-of-contents)

Arrays store multiple values of the same type (numeric or string).

## Creation
```text
create_array(v_array, size)                # Numeric array (default)
create_array(v_array, size, numeric)       # Explicit numeric
create_array(v_array, size, string)        # String array
```

Arrays are 0-indexed. A size of 10 creates indices 0-9.

## Assignment
```text
# Numeric arrays
v_array[0] = 10
v_array[v_i] = 20
v_array[v_i + 1] = 30

# String arrays
v_words[0] = "Hello"
v_words[1] = v_message
```

## Access
```text
v_x = v_array[0]
v_text = v_words[0]
v_result = v_array[0] + v_array[1]

# String concatenation
v_message = v_words[0] & " " & v_words[1]
```

## Using in Commands
```text
plot(v_array[0], v_array[1], red, 100)
draw_circle(v_points[0] + 5, v_points[1] * 2, 5, blue, 100, true)
draw_text(10, 10, v_words[0], tiny64_font, 12, white, 100)
```

## Notes
- Arrays are zero-based (first element is index 0)
- Array size is fixed at creation
- Index must be a number or expression that evaluates to a number
- Cannot mix types: numeric arrays store numbers, string arrays store text
- Be careful with array bounds in loops to avoid exceeding the index
- **Do not reuse an array’s name as a scalar.** `v_x = 5` after
  `create_array(v_x, 10)` replaces the array with a number; later
  `v_x[i]` fails. Use a different name (`v_x_pos`, `v_xi`, …).

# 9. MATH FUNCTIONS

[↑ Table of Contents](#table-of-contents)

## Basic Math
cos(x)      Cosine (radians)
sin(x)      Sine (radians)
tan(x)      Tangent (radians)
abs(x)      Absolute value
pow(x,y)    Power (x raised to y)
sqrt(x)     Square root
exp(x)      Exponential (e^x)
log(x)      Natural logarithm
log10(x)    Base-10 logarithm

## Rounding
round(x)    Round to nearest integer
floor(x)    Round down
ceil(x)     Round up
trunc(x)    Truncate decimal portion
int(x)      Convert to integer

## Trigonometry
asin(x)     Arc sine
acos(x)     Arc cosine
atan(x)     Arc tangent
atan2(y,x)  Arc tangent with correct quadrant
degrees(x)  Convert radians to degrees
radians(x)  Convert degrees to radians

## Min/Max
min(x,y)    Minimum of two values
max(x,y)    Maximum of two values

## Constants
pi          3.14159...
e           2.71828...
tau         6.28318... (2π)

## Random Numbers
```text
random(min, max, precision)
```

Parameters:
- min: Minimum value
- max: Maximum value
- precision: Number of decimal places (0 = integer)

Examples:
```text
v_x = random(0, 63, 0)      # Random integer 0-63
v_speed = random(1, 5, 1)   # Random decimal like 2.3, 4.1
v_factor = random(-1, 1, 2) # Random like -0.45, 0.78
```

## Advanced
copysign(x,y)    Copy sign of y to x
fabs(x)          Floating point absolute value
remainder(x,y)   IEEE 754 remainder
fmod(x,y)        Floating point modulo

## Date/Time Functions
```text
get_datetime(format)
```

Returns current date/time values. Single format codes return numeric values,
compound formats return strings.

TIME CODES (numeric):
  H     Hour 24-hour format (0-23)
  h     Hour 12-hour format (1-12)
  mi    Minute (0-59)
  s     Second (0-59)
  ms    Millisecond (0-999)

TIME CODES (string):
  a     AM or PM

DATE CODES (numeric):
  d     Day of month (1-31)
  m     Month (1-12)
  yyyy  Full year (2026)
  yy    Short year (26)
  w     Weekday number (0=Sunday, 6=Saturday)

DATE CODES (string):
  ddd   Day abbreviated (Sun, Mon, Tue, ...)
  dddd  Day full (Sunday, Monday, ...)
  mmm   Month abbreviated (Jan, Feb, Mar, ...)
  mmmm  Month full (January, February, ...)

EXTENDED CODES (numeric):
  ts        Unix timestamp (seconds since 1970)
  tz        Timezone offset hours (-12 to +14)
  doy       Day of year (1-366)
  woy       Week of year (1-53)
  diy       Days remaining in year
  q         Quarter (1-4)
  ispm      Is PM (0 or 1)
  isweekend Is Saturday or Sunday (0 or 1)
  isleap    Is leap year (0 or 1)
  dim       Days in current month (28-31)

EXAMPLES - Single codes (return numbers):
```text
  v_hour = get_datetime("H")               # v_hour = 14
  v_minute = get_datetime("mi")            # v_minute = 30
  v_day_of_year = get_datetime("doy")      # v_day_of_year = 67
```

EXAMPLES - String codes:
```text
  v_day_name = get_datetime("dddd")        # v_day_name = "Sunday"
  v_ampm = get_datetime("a")               # v_ampm = "PM"
```

EXAMPLES - Compound formats (return strings):
  get_datetime("H:mi:s")         -> "14:30:45"
  get_datetime("m/d/yyyy")       -> "3/8/2026"
  get_datetime("dddd, mmmm d")   -> "Sunday, March 8"
  get_datetime("h:mi a")         -> "2:30 PM"

EXAMPLES - Using in commands:
```text
  # Display current time
  draw_text(10, 20, get_datetime("H:mi:s"), tiny64_font, 12, green, 100)
  
  # Calculate hour hand angle
  v_angle = get_datetime("H") * 30
  
  # Conditional based on time
  if get_datetime("ispm") == 1 then
      # afternoon/evening colors
  endif
  
  # Weekend special animation
  if get_datetime("isweekend") == 1 then
      # party mode
  endif
```

Note: Format strings must be passed as literals (in quotes), not variables.

## System Functions
```text
get_system(metric)
```

Returns system metrics. Currently available metrics:

METRICS:
  runtime   Milliseconds since script started (integer)

EXAMPLES:
```text
  v_runtime = get_system("runtime")        # v_runtime = 45237
  
  # Convert to other units
  v_seconds = get_system("runtime") / 1000
  v_minutes = get_system("runtime") / 60000
  
  # Timed phase changes
  if get_system("runtime") > 30000 then
      # After 30 seconds, do something different
  endif
  
  # Display runtime
  v_secs = int(get_system("runtime") / 1000)
  draw_text(2, 2, v_secs, tiny64_font, 5, white, 100)
```

Note: Metric names must be passed as literals (in quotes), not variables.

# 10. PROCEDURES AND IMPORTS

[↑ Table of Contents](#table-of-contents)

Procedures are reusable blocks of commands. They **can take parameters**.
Libraries under `scripts/lib/` are just files of procedures you `import`.

## Definition

No parameters (legacy style — shares the script’s variables):

```text
def draw_target {
    draw_circle(32, 32, 20, red, 75, true)
    draw_circle(32, 32, 10, white, 75, true)
}
```

With parameters (preferred for reusable logic):

```text
def draw_target(v_x, v_y, v_r) {
    draw_circle(v_x, v_y, v_r, red, 75, true)
    draw_circle(v_x, v_y, v_r / 2, white, 75, true)
}
```

## Calling

```text
call draw_target
call draw_target(32, 32, 20)
call particle.integrate(v_x, v_y, v_vx, v_vy, v_active, 0, 0.15)
```

Argument count must match the definition. There is no `return` value —
results go out through **shared arrays** (see below) or by writing into
caller-owned globals when the procedure has no parameters.

## Scope (this matters)

Also summarized under [§6 Variable management](#variable-management-read-this).

**Not every procedure has locals.** Only a `def` that lists parameters gets
a temporary scope. Zero-arg procedures share the script’s globals.

| Kind of procedure | Scalars | Arrays passed as arguments |
|---|---|---|
| **No parameters** `def foo { … }` | **Global.** `v_x = …` changes the script’s `v_x`. | N/A (no args) — use globals directly |
| **Has parameters** `def foo(v_a, v_b) { … }` | **Local** for parameters and every name the body assigns. Saved on entry, restored on exit. | **Shared reference** — same array object as the caller. `v_a[i] = …` updates the caller’s data |

**Reads** of a name the procedure never assigns still see the script global
(handy for constants like `v_width`). Nested `call`s and **recursion** work.

### Zero-arg — everything global (changes stick)

```text
v_x = 10

def bump {
    v_x = v_x + 1        # script’s v_x
}
call bump
print(f"{v_x}")          # 11
```

### With parameters — scalar locals (restored after call)

```text
v_x = 10
v_n = 99

def bump(v_n) {
    v_n = v_n + 1        # local copy of the argument
    v_x = v_n            # local v_x for this call only
    print(f"inside {v_x}")
}
call bump(5)
print(f"after {v_x} {v_n}")   # after 10 99 — caller’s scalars unchanged
```

```text
def countdown(v_n) {
    if v_n > 0 then
        print(f"{v_n}")
        call countdown(v_n - 1)
    endif
}
call countdown(3)
```

## Arrays are shared (how results come back)

An array argument is **not** a private copy. The parameter points at the
caller’s array. Cell writes survive the call; rebinding the parameter name
itself does not replace the caller’s array variable.

**Why:** there is no `return`, so this is how procedures (and `scripts/lib`)
hand results back. Sharing also avoids copying large arrays on every call.

```text
create_array(v_out, 8, numeric)

def fill_row(v_arr, v_n) {
    for v_i in (0, v_n - 1, 1)
        v_arr[v_i] = v_i * 10     # mutates caller’s v_out
    endfor v_i
}

call fill_row(v_out, 8)    # v_out[0]..[7] are set
```

Contrast with scalars: `v_n` above is a local number; assigning `v_n = …`
inside does not change the caller’s `v_n` (if they had one).

## Example

```text
def draw_target(v_x, v_y) {
    draw_circle(v_x, v_y, 20, red, 75, true)
    draw_circle(v_x, v_y, 10, white, 75, true)
}

call draw_target(32, 32)

for v_i in (0, 3, 1)
    call draw_target(10 + v_i * 15, 32)
endfor v_i

begin_frame
    call draw_target(32, 32)
    draw_rectangle(0, 0, 64, 5, blue, 75, true)
end_frame
```

## Imports

```text
import particle
import agent
```

Rules:

- `import name` loads `scripts/lib/name.pix` (default library directory).
  A path works the same way: `import arcade/ai` → `scripts/lib/arcade/ai.pix`.
- Imported procedures are **prefixed** with the last path segment:
  `import particle` then `call particle.integrate(...)`.
- A library file may contain **only** `def` procedures (no top-level drawing
  or loops).
- Importing the same path twice is ignored. A second library that wants the
  **same prefix** is an error.
- Inside a library, sibling calls must use the prefixed name
  (`call particle.collide_bounds(...)`), not the bare name.

Full APIs: [§22 Particle library](#22-particle-library) and
[§23 Agent, ink, chladni, and points](#23-agent-ink-chladni-and-points-libraries).

## Library catalogue (`scripts/lib`)

| Import | Prefix | What it provides |
|---|---|---|
| `import particle` | `particle.*` | Motion kernels: `integrate`, `verlet`, `flock`, attractors/springs, collide bounds / circles / static circles. Mutates arrays; does not draw. |
| `import agent` | `agent.*` | `chemotaxis` — slime-mold style agents on a scent field. |
| `import points` | `points.*` | `lerp`, `project3d` — point-cloud helpers. |
| `import ink` | `ink.*` | `step` — ink-in-water particle step; reads script globals and **plots** (call inside a frame). |
| `import chladni` | `chladni.*` | `step` — Chladni particle step; uses script globals and **plots** (call inside a frame). |

`ink.step` and `chladni.step` take **no parameters** on purpose so they share
the script’s particle arrays and open-frame `mplot` context.


# 11. FRAME CONTROL

[↑ Table of Contents](#table-of-contents)

Frame buffering allows multiple drawing commands to appear simultaneously.

## Commands
```text
begin_frame           Start buffering (clears every drawing layer’s pixels)
begin_frame(true)     Start buffering (preserves layer pixels)
end_frame             Composite and display everything at once
```

Both forms reset the **current drawing layer** back to 0 so a HUD
`set_layer` cannot leak into the next frame. Sprites keep their layer
assignments. See section 15 for drawing layers.

## Standard mode (`begin_frame` or `begin_frame(false)`)

- Clears every drawing layer’s pixels before buffering (layer slots stay)
- All commands are buffered until `end_frame`
- Only new content is visible after `end_frame`
- Do not use `clear()` inside a frame. It causes flicker

```text
begin_frame
    draw_circle(32, 32, 20, red, 100, true)
    draw_line(10, 10, 54, 54, yellow, 100)
    draw_rectangle(22, 22, 20, 20, green, 100, false)
end_frame
```

## Preserve mode (`begin_frame(true)`)

- Keeps existing layer pixels
- New commands overlay on the current layer
- Useful for retained scenery with moving sprites / scrolling backgrounds

```text
draw_circle(32, 32, 20, red, 100, true)
rest(1)

begin_frame(true)
    draw_circle(16, 16, 10, blue, 100, true)
    draw_circle(48, 48, 10, green, 100, true)
end_frame
```

## Animation example

```text
for v_frame in (0, 100, 1)
    begin_frame
        v_x = 32 + cos(v_frame * 0.1) * 20
        v_y = 32 + sin(v_frame * 0.1) * 20
        draw_circle(v_x, v_y, 5, red, 100, true)
    end_frame
    rest(0.033)
endfor v_frame
```

## What a frame sends

`end_frame` sends the frame's pixels as one `draw_batch`. Shape commands in
that frame (`draw_line`, `draw_circle`, `draw_rectangle`, `draw_polygon`,
`draw_ellipse`, `draw_arc`) go in the same batch. Commands that are not part
of the batch (`draw_text`, sprites, backgrounds) are sent after it, in order.

For a lot of pixels, use `mplot`, inside the frame or outside it. Section 11.

```text
begin_frame
    for v_i in (0, 500, 1)
        mplot(random(0, 63, 0), random(0, 63, 0), white, 80)
    endfor v_i
    draw_circle(32, 32, 10, red, 100, true)
end_frame
```

# 12. BATCH PLOTTING (mplot / mflush)

[↑ Table of Contents](#table-of-contents)

`mplot` is how a script sends many pixels as one command. The consumer has no
`mplot` command. The producer holds the pixels and sends one `draw_batch`.

- Inside `begin_frame` / `end_frame`, `end_frame` sends the batch. `mflush` is not required.
- Outside a frame, `mflush` sends the batch. Without `mflush`, the pixels stay buffered.

`plot` outside a frame is one consumer command per pixel. A loop of a thousand
`plot` calls is a thousand commands. The same loop written with `mplot`, then
one `mflush`, is one command. Keep `mplot` when the loop is inside a frame
too. Opening a frame is not a reason to rewrite it as `plot`.

## Commands

```text
mplot(x, y, color, [intensity], [burnout], [burnout_mode])
mflush()
```

## Outside a frame

```text
for v_i in (0, 100, 1)
    mplot(random(0, 63, 0), random(0, 63, 0), red, 100)
endfor v_i
mflush()
```

## With burnout

```text
mplot(10, 10, red, 100, 2000, fade)
mplot(11, 10, red, 100, 2000, fade)
mplot(12, 10, red, 100, 2000, fade)
mflush()
```

## Inside a frame

`end_frame` sends the `mplot` pixels in the frame's `draw_batch`. A point
whose coordinates fall outside 0..63 is dropped.

```text
begin_frame
    draw_circle(32, 32, 15, blue, 100, true)
    for v_i in (0, 100, 1)
        mplot(random(20, 44, 0), random(20, 44, 0), white, 80)
    endfor v_i
end_frame
```

## Notes

- Call `mflush()` after `mplot` when no frame is open.
- Inside a frame, `end_frame` sends the batch. `mflush()` there is optional.

# 13. TEXT COMMANDS

[↑ Table of Contents](#table-of-contents)

## DRAW_TEXT
```text
draw_text(x, y, text, font_name, font_size, color, intensity, [effect], [modifier])
```

Parameters:
- x, y: Position (0-63)
- text: String to display (in quotes)
- font_name: Font to use (e.g., `tiny64_font`)
- font_size: Size in pixels (accepted; native tiny64 path does not scale by this yet)
- color: Color name or variable
- intensity: Brightness (1–100; see [Intensity](#intensity))
- effect: Optional — see below
- modifier: Optional — see below

### What works on native (this repo)

Only the **NORMAL** path is implemented. Alignment modifiers apply:

| effect | modifier | Result |
|---|---|---|
| `NORMAL` (or omit) | omit / `LEFT` | Draw at `x,y` (left edge) |
| `NORMAL` | `CENTER` | Centre the string on `x` |
| `NORMAL` | `RIGHT` | Right-align so the end is at `x` |

### Fancy effects (not animated here)

`TYPE`, `SCAN`, `SLIDE`, `DISSOLVE`, and `WIPE` are accepted for script
compatibility with the Python edition, but on native they **draw as NORMAL**
(no typewriter / slide / wipe animation). Prefer `NORMAL` + alignment in new
scripts.

## Examples
```text
# Basic text
draw_text(10, 20, "Hello", tiny64_font, 12, white, 100)

# Centered text
draw_text(32, 20, "SCORE", tiny64_font, 12, yellow, 100, NORMAL, CENTER)

# Right-aligned
draw_text(60, 20, "100", tiny64_font, 12, green, 100, NORMAL, RIGHT)
```

## CLEAR_TEXT
```text
clear_text(x, y)
```

Clears text at the specified coordinates.

```text
clear_text(10, 20)
```

# 14. SPRITES

[↑ Table of Contents](#table-of-contents)

Sprites are reusable graphical objects that can be animated and instanced.
Each instance can also be **scaled, flipped, spun, or tilted** (see
[Sprite Transforms](#sprite-transforms) below). With no transform set, a
sprite draws exactly as it always has — native size, upright, whole pixels.

## DEFINING SPRITES

Single-Cel Sprite:
```text
define_sprite(name, width, height)
    # drawing commands
endsprite
```

Example:
```text
define_sprite(ball, 10, 10)
    draw_circle(5, 5, 4, red, 100, true)
endsprite
```

Multi-Cel Animated Sprite:
```text
define_sprite(name, width, height)
    sprite_cel(0)
        # frame 0 drawing
    sprite_cel(1)
        # frame 1 drawing
endsprite
```

Example:
```text
define_sprite(pacman, 12, 12)
    sprite_cel(0)
        draw_circle(6, 6, 5, yellow, 100, true)
    sprite_cel(1)
        draw_circle(6, 6, 5, yellow, 100, true)
        draw_polygon(11, 6, 4, 3, black, 100, 0, true)
endsprite
```

Auto-indexing (omit cel number):
```text
define_sprite(blinker, 8, 8)
    sprite_cel()
        draw_circle(4, 4, 3, green, 100, true)
    sprite_cel()
        draw_circle(4, 4, 3, red, 100, true)
endsprite
```

Drawing Commands in Sprites:
- plot
- draw_line
- draw_rectangle
- draw_circle
- draw_polygon
- draw_ellipse
- draw_arc

Note: draw_text and burnout are NOT supported inside sprites.

## SPRITE COMMANDS
```text
show_sprite(name, x, y, [instance_id], [z_index], [cel_idx])
hide_sprite(name, [instance_id])
move_sprite(name, x, y, [instance_id], [cel_idx])
dispose_sprite(name, [instance_id])
dispose_all_sprites()

# Transforms (per instance — see Sprite Transforms below)
set_sprite_scale(name, scale, [instance_id], [scale_y])
set_sprite_angle(name, angle, [instance_id])
set_sprite_tilt(name, tilt_x, tilt_y, [instance_id])
set_sprite_flip(name, flip, [instance_id])
set_sprite_anchor(name, mode, [instance_id])
set_sprite_transform(name, scale, angle, [instance_id], [tilt_x], [tilt_y], [flip])
reset_sprite_transform(name, [instance_id])
sprite_back_cel(name, cel)
```

Show Sprite:
```text
show_sprite(ball, 10, 20)                    # Default instance 0
show_sprite(ball, 10, 20, 1)                 # Instance 1
show_sprite(ball, 10, 20, 1, 0, 2)           # Instance 1, z_index 0, cel 2
show_sprite(ball, 10.5, 20.25)               # Float x,y ok (see Positions)
```

Hide Sprite:
```text
hide_sprite(ball)                            # Hide instance 0
hide_sprite(ball, 1)                         # Hide instance 1
```

Move Sprite:
```text
move_sprite(ball, 30, 40)                    # Auto-advances cel
move_sprite(ball, 30, 40, 1)                 # Move instance 1
move_sprite(ball, 30, 40, 0, 2)              # Jump to cel 2 (no auto-advance)
move_sprite(ball, 30.25, 40.5, 0)            # Sub-pixel move when transformed
```

**Pitfall:** omitting `cel_idx` always advances to the next cel (wraps to 0).
If you only want to change position, pass the cel explicitly.

## CEL ANIMATION BEHAVIOR
- Auto-advance: move_sprite without cel_idx advances to next cel (wraps to 0)
- Explicit cel: Provide cel_idx to jump to specific frame
- Cel preservation: hide_sprite preserves current cel
- Independent tracking: Each instance tracks its own cel

Animation Example:
```text
define_sprite(walker, 8, 8)
    sprite_cel(0)
        draw_rectangle(2, 0, 4, 4, cyan, 100, true)
        draw_line(3, 4, 3, 7, cyan, 100)
    sprite_cel(1)
        draw_rectangle(2, 0, 4, 4, cyan, 100, true)
        draw_line(2, 4, 1, 7, cyan, 100)
    sprite_cel(2)
        draw_rectangle(2, 0, 4, 4, cyan, 100, true)
        draw_line(3, 4, 3, 7, cyan, 100)
    sprite_cel(3)
        draw_rectangle(2, 0, 4, 4, cyan, 100, true)
        draw_line(4, 4, 5, 7, cyan, 100)
endsprite

show_sprite(walker, 0, 28, 0, 0, 0)
for v_x in (0, 55, 1)
    move_sprite(walker, v_x, 28, 0)
    rest(0.08)
endfor v_x
```

## SPRITE INSTANCING
Create multiple instances of the same sprite:

```text
show_sprite(enemy, 10, 10, 0)    # Instance 0
show_sprite(enemy, 30, 10, 1)    # Instance 1
show_sprite(enemy, 50, 10, 2)    # Instance 2

# Move each independently
move_sprite(enemy, 12, 10, 0)
move_sprite(enemy, 32, 10, 1)
move_sprite(enemy, 52, 10, 2)
```

Transforms are per instance too — enemy 0 can spin while enemy 1 stays still.

## SPRITE TRANSFORMS

Use these when you want a sprite to grow/shrink, mirror, spin flat on the
panel, or tip like a card/coin. State sticks on the instance until you change
it or call `reset_sprite_transform`.

**Prefer the small helpers** (`set_sprite_scale`, `set_sprite_angle`, …) when
changing one thing. Use `set_sprite_transform` when setting several at once.

### What each command does

| Command | Effect |
|---|---|
| `set_sprite_scale(name, scale, [inst], [scale_y])` | Size. `1.0` = native. Clamp **0.1 … 8**. Optional `scale_y` stretches tall/wide (`scale` = horizontal, `scale_y` = vertical). No negative scale — use flip. |
| `set_sprite_angle(name, angle, [inst])` | Flat spin in **degrees**. **+** = clockwise, **−** = the other way. Any size is fine (`450` = one turn + 90°). |
| `set_sprite_tilt(name, tilt_x, tilt_y, [inst])` | Tip in degrees (see Spin vs tilt). |
| `set_sprite_flip(name, flip, [inst])` | `0` none, `1` horizontal, `2` vertical, `3` both. |
| `set_sprite_anchor(name, mode, [inst])` | Where `x,y` points: `0` top-left (default), `1` centre. Opt-in — scaling/spinning alone does **not** switch the anchor. |
| `set_sprite_transform(name, scale, angle, [inst], [tilt_x], [tilt_y], [flip])` | Set several fields at once. |
| `reset_sprite_transform(name, [inst])` | Scale 1, angle 0, no tilt, no flip. **Keeps the anchor.** |
| `sprite_back_cel(name, cel)` | After `endsprite`: which still cel to show when the reverse faces the viewer (cards, coins). |

**Optional-arg tip:** instance id comes before tilt/flip (same pattern as
`show_sprite`). If you pass tilt or flip on `set_sprite_transform`, **always
pass the instance id** so a tilt value is not read as an instance.

### Anchor (where x,y points)

- **Top-left (`0`, default):** `x,y` is the top-left of the *unscaled* sprite,
  same as classic Pixil. Spin and scale still pivot around the sprite’s
  centre, and that centre stays where it would at 1× — adding rotation later
  does not jump the sprite.
- **Centre (`1`):** call `set_sprite_anchor(name, 1)`. Then `x,y` is the
  pivot — best for spinning clocks, orbiting objects, projected depth.

### Positions (float x, y)

`show_sprite` / `move_sprite` accept float coordinates.

- **No transform** (plain sprite): position is rounded to a whole pixel —
  same look as before.
- **Has a transform:** fractions are kept (~1/16 pixel) so slow drifts do
  not stutter.

### Spin vs tilt (what you see)

| Control | Looks like | Direction |
|---|---|---|
| `angle` | Spins flat, like a clock hand | + clockwise, − other way |
| `tilt_y` | Turns left/right like a door or coin — narrows, edge-on at ±90°, reverse beyond | + right edge away, − left edge away |
| `tilt_x` | Tips forward/back like a falling sign — shortens, edge-on at ±90°, reverse beyond | + top edge away, − bottom edge away |

You can combine all three. Past ±90° the reverse faces you:

1. **Default:** keep the front animation, but mirrored.
2. **With `sprite_back_cel`:** show that one still frame instead (card back,
   coin tails). Front cels keep animating only while the front faces you.

Edge-on (thinner than one pixel) draws a 1-pixel sliver in the art’s average
colour so a spinning coin flashes an edge instead of vanishing.

### Quick examples

Scale and spin in place (centre anchor):

```text
show_sprite(arrow, 32, 32, 0, 0, 0)
set_sprite_anchor(arrow, 1, 0)

while true then
    begin_frame
        set_sprite_scale(arrow, 1.0 + abs(sin(v_t * 0.08)) * 2.0, 0)
        set_sprite_angle(arrow, v_t * 4, 0)
        move_sprite(arrow, 32, 32, 0)
    end_frame
    v_t = v_t + 1
endwhile
```

Card flip with a dedicated back cel:

```text
define_sprite(card, 10, 14)
    sprite_cel(0)
        # front art ...
    sprite_cel(1)
        # back art ...
endsprite
sprite_back_cel(card, 1)

show_sprite(card, 32, 32)
set_sprite_anchor(card, 1)
set_sprite_tilt(card, 0, v_degrees)   # animate v_degrees past 90 to show back
```

Squash / stretch:

```text
set_sprite_scale(mark, 1.8, 0, 0.6)   # wide and short
```

Reset keeps centre anchor:

```text
set_sprite_anchor(arrow, 1)
set_sprite_transform(arrow, 2.0, 45, 0, 0, 30, 1)
reset_sprite_transform(arrow)         # back to 1×, 0°, no tilt/flip
# anchor is still centre
```

Live walkthrough on the panel: `scripts/tests/Sprite_Transforms_Demo.pix`.

## SPRITE NOTES
- Coordinates inside sprites are relative to top-left (0,0)
- Sprite names don't use quotes
- Instance IDs are optional (defaults to 0)
- At `show_sprite`, the instance joins the **current drawing layer**
  (`set_layer`). Among sprites on that same layer, `z_index` decides order.
  Drawing layers (section 15) put sprites *between* scenery bands — they are
  no longer always on top of every plot.
- Background buffer is preserved when sprites move or hide
- Transforms are per instance and persist until changed or reset
- `reset_sprite_transform` does **not** change the anchor
- Scale is clamped to 0.1…8; use flip (or tilt past 90°) to mirror

# 15. DRAWING LAYERS

[↑ Table of Contents](#table-of-contents)

Drawing layers let scenery and sprites stack in depth. A fox can walk
**between** far trees and near trees instead of always sitting on top of
everything you draw.

Without `set_layer`, everything stays on **layer 0** — same behavior as
before this feature.

## STACK (back → front)

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

Rules:

1. **Layer number = stack position.** Higher is in front. Gaps are fine
   (`10`, `20`, `100`). Layer must be **≥ 0**.
2. **On one layer:** all drawing paints first, then that layer’s sprites.
   Sprites win over drawing on the **same** layer.
3. **Among sprites on the same layer:** higher `z_index` is in front.
4. **Sprites remember their layer** from `show_sprite` (or
   `set_sprite_layer`). Moving them does not change layer unless you say so.
5. **Burnout stays inside its layer.** Overlay/fade ownership does not
   cross layers. `reorder_layer` moves that layer’s burnouts with the buffer.
6. **Layers stack; they do not slide.** To move art on screen, use sprites
   or scrolling backgrounds — not layer numbers.

## COMMANDS

```text
set_layer(layer)
```
All following drawing goes onto that layer (`plot`, `mplot`, shapes,
`draw_text`, …). Creates the layer empty the first time you use it.
`begin_frame` / `begin_frame(true)` reset the current layer back to **0**
(sprites keep their assignments).

```text
clear_layer(layer)
```
Erase every pixel on that layer (transparent). The layer still exists;
visibility and sprites on it are unchanged. Clears burnout on that layer.
Use this to redraw only a HUD or FX band.

```text
delete_layer(layer)
```
Remove the layer buffer. Sprites that were on it are **hidden** (not
disposed) and stay hidden until you `show_sprite` them again on a living
layer. If you delete the current drawing layer, current becomes 0.

```text
hide_layer(layer)
show_layer(layer)
```
Skip or include a layer when compositing. Pixels stay in memory. Sprites
on a hidden layer are not drawn while it is hidden.

```text
reorder_layer(from_layer, to_layer)
```
Move the buffer, its sprites, and its burnouts to a new layer number
without redrawing. `from_layer` is vacated. If `to_layer` already exists,
it is **overwritten** (old pixels/burnouts discarded; old sprites on that
number hidden).

```text
set_sprite_layer(name, layer, [instance_id])
set_sprite_z(name, z_index, [instance_id])
```
Change a sprite’s layer or same-layer `z_index` without depending on the
current `set_layer`. `set_sprite_layer` auto-creates an empty layer if
needed. Prefer `set_sprite_z` over a bare `move_sprite` when you only want
order (bare `move_sprite` advances cel).

## SPRITES ON LAYERS

```text
set_layer(10)
call draw_forest
show_sprite(rabbit, 8, 40, 0, 0, 0)    # layer 10, z_index 0
show_sprite(fox, 22, 42, 0, 1, 0)      # layer 10, z_index 1 → in front of rabbit

set_layer(15)
show_sprite(deer, 30, 38)              # layer 15 (between 10 and 20)

set_layer(20)
call draw_near_trees                   # covers deer where they overlap

# later: duck behind the near trees
set_sprite_layer(deer, 12)
```

`z_index` only orders sprites **on the same layer**. Depth between bands
comes from the layer number.

## FRAMES AND RETAINED SCENERY

```text
begin_frame            # clear every layer’s pixels; current set_layer → 0
begin_frame(true)      # keep pixels; current set_layer → 0
end_frame              # composite backgrounds + layers + sprites, present

clear                  # clear all layer pixels; current → 0; hide backgrounds;
                       # clear burnout; does not dispose sprites or layer slots
```

Typical pattern: draw static bands once, then preserve and only redraw HUD/FX.

```text
set_background(sky, 0)

begin_frame
    set_layer(10)
    call draw_far_trees
    set_layer(18)
    show_sprite(fox, 8, 40, 0, 0, 0)
    set_layer(25)
    call draw_near_trees
end_frame

while true then
    begin_frame(true)
        nudge_background(1, 0, 0)
        move_sprite(fox, v_x, 40)
        set_layer(40)
        clear_layer(40)
        call draw_fireflies
    end_frame
endwhile
```

Draw setup **inside** a frame (or you will present after every shape —
startup flicker). See `scripts/main/Layer_Walk.pix`.

## BURNOUT WITH LAYERS

- Each burnout remembers the layer that was current when it was created.
- `clear_layer(n)` clears burnout on layer `n`.
- `delete_layer` / overwrite via `reorder_layer` ends burnouts on the
  discarded layer quietly.
- `reorder_layer(from, to)` moves burnouts with the buffer.
- Scripts that never call `set_layer` keep today’s layer-0 burnout behavior.

# 16. BACKGROUND LAYERS

[↑ Table of Contents](#table-of-contents)

Background layers are scrollable, animated planes rendered **behind every
drawing layer** (always under layer 0).

## KEY CONCEPTS
- Render stack: Backgrounds → Drawing Layers (each: pixels, then sprites) → front
- Multiple background planes supported (higher background numbers above lower)
- Backgrounds tile/wrap seamlessly
- Can be any size (tiles if smaller than 64x64, scrolls if larger)
- For occlusion between drawn scenery and sprites, use drawing layers
  (section 15), not background planes

## COMMANDS
```text
set_background(sprite_name, [layer], [cel_index])
```
    Activates a sprite as a background layer.
    - layer defaults to 0
    - cel_index defaults to 0

hide_background([layer])
    Hides background layer (omit layer to hide all).
    Sprite template is preserved for reactivation.

nudge_background(dx, dy, [layer], [cel_index])
    Shifts background by (dx, dy) relative to current position.
    - Omit cel_index to auto-advance cel
    - Specify cel_index to hold on that frame

set_background_offset(x, y, [layer], [cel_index])
    Sets absolute background position.
    Same cel behavior as nudge_background.

## TRANSPARENCY
- Use transparent color in background sprites for see-through areas
- Lower layers bleed through higher layers where transparent
- Backgrounds bleed through into drawing layers where those pixels are transparent

## EXAMPLE
```text
# Define scrolling starfield
define_sprite(stars, 128, 128)
    for v_i in (0, 50, 1)
        plot(random(0, 127, 0), random(0, 127, 0), white, random(50, 100, 0))
    endfor v_i
endsprite

# Activate background
set_background(stars)

# Scroll animation
for v_i in (0, 200, 1)
    begin_frame(true)
```
        nudge_background(1, 0)
```text
        draw_circle(32, 32, 5, red, 100, true)
    end_frame
    rest(0.033)
endfor v_i
```

# Cleanup
hide_background

## CLEANUP BEHAVIOR
- clear(): Hides backgrounds, clears every drawing layer’s pixels, sprite
  templates survive (layer slots stay allocated)
- dispose_all_sprites(): Destroys all sprite templates including backgrounds
- Between playlist scripts the consumer also disposes drawing layers

# 17. PERFORMANCE COMMANDS

[↑ Table of Contents](#table-of-contents)

## FPS (RECOMMENDED FOR ANIMATION SPEED)
```text
fps(rate)
```

Caps how fast the **producer** advances after each frame. On this native
runtime, `pixil` sleeps to hit the target so wall-clock helpers like
`get_datetime` stay in step with the show (otherwise a fast producer can
queue hundreds of frames with the same clock second). The consumer still
presents as commands arrive.

- `fps(60)` — aim for at most 60 frames per second (~0.0167s between frames)
- `fps(30)` — at most 30 FPS
- `fps(0)`  — unlimited (default; producer pacing off)

Default is off. Each script starts with `fps(0)`; set `fps` when you want a
cap. You may change it during the show. Disabled under `--dry-run`.

If work finishes before the frame budget, the producer waits the remainder.
If work is slower than the budget, it does not add extra delay (no catch-up
burst).

Example (framed animation — preferred):

```text
fps(30)
while true then
    begin_frame(true)
    # ... simulation and draws ...
    end_frame
endwhile
```

Example (immediate draws — still paced by the producer when `fps` is set):

```text
fps(60)
for v_i in (0, 63, 1)
    plot(v_i, v_i, green, 100)
endfor v_i
```

## REST
```text
rest(seconds)
```

Pauses for a fixed wall-clock duration. The producer sleeps (so time-based
script logic advances correctly) and a `rest` command is also sent so the
consumer holds. Use for holds and timed pauses — not for steady animation
speed (`fps` is better for that). A `rest` breaks the current `fps`
cadence; the next `end_frame` starts a fresh interval.

```text
rest(1)       # Pause 1 second
rest(0.05)    # Pause 50 milliseconds
rest(v_delay) # Variable delay
```

## SYNC_QUEUE
```text
sync_queue
```

Waits for all queued commands to complete before continuing. Use when you need
to ensure the display has caught up before proceeding.

```text
for v_i in (0, 5, 1)
    draw_circle(v_i * 10, 30, 3, red, 100, true)
    rest(0.4)
endfor v_i
sync_queue
print("All circles drawn")
```

## THROTTLE
```text
throttle(factor)
```

**Legacy.** On the native runtime, `throttle(...)` is accepted and **ignored**
(treated like always-fast queue pacing). Prefer `fps()` for animation rate.
Old scripts may still call `throttle(0)`; that is fine and does nothing here.

```text
throttle(0)             # no-op on native (harmless)
throttle(0.3)           # also ignored on native
```

# 18. DEBUGGING

[↑ Table of Contents](#table-of-contents)

## PRINT
```text
print("message")
print(f"value is: {v_variable}")
```

Outputs text to the console for debugging.

**To show a variable’s value you must use the `f"..."` form.** A plain
string only prints that text — `print("v_x")` prints the characters
`v_x`, not the number in `v_x`.

Examples:
```text
print("Starting animation")           # literal text only
print(f"Position: {v_x}, {v_y}")      # evaluates braces
print(f"Array value: {v_data[v_i]}")
```

Notes:
- Use `f"..."` (or `f'...'`) with curly braces for variables and expressions
- Plain `print("...")` does not interpolate

## IMPORTANT: SYNC_QUEUE WITH PRINT
Pixil uses a command queue - drawing commands are sent to the queue and 
executed by the display asynchronously. This means print statements execute
immediately while drawing commands may still be queued.

To ensure a print statement reflects what's actually displayed, add sync_queue
BEFORE the print:

```text
# WRONG - print happens before circles are actually drawn
for v_i in (0, 5, 1)
    draw_circle(v_i * 10, 30, 3, red, 100, true)
    rest(0.4)
endfor v_i
print("All circles drawn")  # This prints while circles are still being drawn!

# CORRECT - sync_queue waits for display to catch up
for v_i in (0, 5, 1)
    draw_circle(v_i * 10, 30, 3, red, 100, true)
    rest(0.4)
endfor v_i
sync_queue                   # Wait for all queued commands to complete
print("All circles drawn")   # Now this accurately reflects the display state
```

Use sync_queue before print when:
- Confirming an animation sequence completed
- Debugging timing issues
- Writing regression tests
- Any time the print should reflect what's on screen

# 19. COMMON ERRORS AND TROUBLESHOOTING

[↑ Table of Contents](#table-of-contents)

## SCRIPTING PITFALLS (read these first)

These regularly cost debugging time:

1. **Intensity below 1 looks full-bright.** For `plot` / `draw_*` in a frame,
   intensity is truncated and `0` is treated as `100`. Keep dim values at
   **≥ 1**. See [Intensity](#intensity).
2. **`move_sprite` without a cel number advances the cel.** Omit `cel_idx`
   and multi-cel sprites walk their animation every move. Pass an explicit
   cel (often the current one) when you only want to move.
3. **A scalar can shadow an array of the same name.** After
   `create_array(v_x, n)`, never assign `v_x = …` — that destroys the array.
4. **`print` needs `f"..."` to show values.** `print("v_x")` prints the
   literal text `v_x`. Use `print(f"{v_x}")`.
5. **Named colours are a fixed list of 44.** No `dark_green` / `cream` /
   etc. See [Named Colors](#named-colors-exactly-44).
6. **Variable scope:** globals everywhere except parameterized procedures
   (scalar locals). Array arguments are shared on purpose. See
   [§6 Variable management](#variable-management-read-this) and
   [§10 Scope](#scope-this-matters).
7. **Producer runs ahead of the panel.** `print` and script logic can race
   draws — see [§2 How Pixil Runs](#2-how-pixil-runs-producer--consumer).

Pixil also provides helpful error messages when conditions are invalid:

## VARIABLE NOT FOUND
Error: Variable 'v_xx' not found
  Condition: v_xx > 5
  Hint: Did you mean: v_x?

Cause: The variable name has a typo or hasn't been initialized.
Fix: Check spelling, or initialize the variable before using it:
```text
  v_x = 10
  if v_x > 5 then ...
```

## USING = INSTEAD OF ==
Error: Invalid condition syntax
  Condition: v_x = 10
  Hint: Did you mean '==' instead of '='? Use '==' for comparison.

Cause: Single = is for assignment, == is for comparison.
Fix: Use == in conditions:
```text
  if v_x == 10 then  ; Correct (comparison)
  v_x = 10           ; This is assignment, not comparison
```

## INCOMPLETE CONDITION
Error: Incomplete compound condition
  Condition: v_x > 5 and
  Hint: Add a condition after 'and'

Cause: Missing condition after 'and' or 'or' operator.
Fix: Add the complete condition:
```text
  if v_x > 5 and v_y < 10 then
```

## UNBALANCED PARENTHESES
Error: Unbalanced parentheses in condition
  Condition: (v_x > 5 and v_y < 10
  Hint: You have 1 unclosed '(' - make sure each '(' has a matching ')'.

Cause: Missing opening or closing parenthesis.
Fix: Match all parentheses:
```text
  if (v_x > 5 and v_y < 10) then  ; Correct
```

## UNTERMINATED STRING
Error: Unterminated string in condition
  Condition: v_color == "red
  Hint: Make sure all strings have matching opening and closing quotes.

Cause: Missing closing quote on a string.
Fix: Add the closing quote:
```text
  if v_color == "red" then  ; Correct
```

## STRING COMPARISON WITH > OR <
Error: Operator '>' not supported for string comparison
  Condition: v_name > "bob"
  Hint: Use '==' or '!=' for string comparison. Strings cannot be compared with >.

Cause: Attempting to use >, <, >=, <= with strings.
Fix: Use == or != for strings:
```text
  if v_name == "bob" then  ; Correct
  if v_name != "bob" then  ; Correct
```

## ARRAY NOT FOUND
Error: Array 'v_data' not found
  Hint: Make sure the array is created with create_array() before use.

Cause: Trying to use an array before creating it.
Fix: Create the array first:
```text
  create_array(v_data, 10)
  if v_data[0] == 5 then ...
```

## ARRAY INDEX OUT OF BOUNDS
Error: Array index out of bounds: v_numbers[10]
  Hint: Array size is 3, valid indices are 0 to 2.

Cause: Trying to access an array element beyond its size.
Fix: Use an index within the valid range:
```text
  create_array(v_numbers, 3)  ; Creates indices 0, 1, 2
  if v_numbers[2] == 5 then   ; Index 2 is the last valid index
```

## TYPE MISMATCH
Error: Cannot compare str with int using '>'

Cause: Trying to compare incompatible types.
Fix: Make sure both sides of the comparison are the same type.

# 20. PERFORMANCE TIPS

[↑ Table of Contents](#table-of-contents)

The Raspberry Pi 4 has limited CPU power. Follow these tips for smooth animations:

## GENERAL TIPS
1. Use begin_frame/end_frame for animations; plot() and draw_* batch at end_frame
2. Use mplot/mflush only when NOT using frame buffering (see section 12)
3. Use burnout instead of manually clearing pixels
4. Prefer simpler shapes when possible (circles are more expensive than rectangles)
5. Use fps() to cap frame rate; throttle() is a no-op on native (legacy leftover)
6. For full-grid sims or distance fields, use grid_program / field_program (section 21)
   instead of per-cell Pixil loops

## LOOP OPTIMIZATION
```text
# Without frames — many plot() calls queue one command each (slow)
for v_i in (0, 100, 1)
    plot(v_i, 32, red, 100)
endfor v_i

# Without frames — batch with mplot/mflush
for v_i in (0, 100, 1)
    mplot(v_i, 32, red, 100)
endfor v_i
mflush()

# With frames — mplot joins the frame batch; end_frame sends one command
for v_frame in (0, 100, 1)
    begin_frame
        for v_i in (0, 100, 1)
            mplot(v_i, 32, red, 100)
        endfor v_i
    end_frame
endfor v_frame
```

## FRAME RATE TIPS
- Target 30 FPS with fps(30) and begin_frame/end_frame for smooth animation
- Complex calculations inside loops slow down frame rate
- Pre-calculate values outside loops when possible

```text
# Less efficient
for v_frame in (0, 100, 1)
    v_angle = v_frame * 0.1
    v_cos_val = cos(v_angle)
    v_sin_val = sin(v_angle)
    # ... use values
endfor v_frame

# More efficient - if angle increment is constant
v_angle = 0
for v_frame in (0, 100, 1)
    v_cos_val = cos(v_angle)
    v_sin_val = sin(v_angle)
    v_angle = v_angle + 0.1
    # ... use values
endfor v_frame
```

## SPRITE VS DRAWING
- Sprites are efficient for moving objects (only redraws what changes)
- Use sprites for game characters, moving elements
- Use direct drawing for backgrounds, static elements, particle effects

## BURNOUT FOR TRAILS
Instead of tracking and erasing pixels manually, use burnout:

```text
# Automatic cleanup with burnout
for v_i in (0, 60, 1)
    plot(v_i, 32, red, 100, 500, fade)
    rest(0.05)
endfor v_i
```

## NATIVE GRID / FIELD PROGRAMS
For full-grid simulations (cellular automata, reaction-diffusion, fire) or
distance-field effects (Voronoi, metaballs, ripples), use grid_program and
```text
field_program instead of per-cell Pixil loops. Setup and orchestration stay in
```
Pixil; the producer runs the hot step and render. See section 21.

## PARTICLE LIBRARY
For many moving circles, `import particle` and call `particle.integrate` and
the `particle.collide_*` procedures instead of a per-particle physics loop.
They mutate numeric arrays and do not draw. See section 22.

## AGENT, INK, CHLADNI, AND POINTS
`import agent`, `import ink`, `import chladni`, or `import points`, then call
the procedures in section 23. `grid_step` still diffuses and draws a field.

# 21. NATIVE GRID AND FIELD PROGRAMS

[↑ Table of Contents](#table-of-contents)

Native grid and field programs accelerate simulations and distance-field
effects on the 64x64 matrix. Pixil handles seeding, mode changes, and UI;
the producer runs the per-frame math and draws via the normal frame
batch (mplot / draw_rectangle inside grid_step / field_render).
`grid_program` and `field_program` are part of the language. They are not libraries.

## WHEN TO USE
Use grid_program for neighbor-based grid simulations:
- Cellular automata (Game of Life)
- Reaction-diffusion (Gray-Scott)
- Fire / heat propagation
- Any rule that updates every cell from neighbors

Use field_program for per-pixel formulas over moving sites:
- Voronoi diagrams
- Metaballs
- Ripple tanks / wave interference (sum_sites over sources)
- Escape-time fractals (Mandelbrot / Julia via escape_iter / escape_zoom)

Use call chladni.step for the Chladni particle effect (see below) — it is
particle-based, not a full-grid program.

Keep plain Pixil loops when logic is not expressible in the expression
language, or when only a handful of objects are updated per frame.

For many independent circles (balls, rocks, marbles), use the particle
primitives in section 22 instead of per-body Pixil physics. Those commands
update arrays; they do not render pixels (unlike grid_step / call chladni.step).

For grid-following trail agents (slime mold, ant highways), use
call agent.chemotaxis in section 23 plus grid_step for diffuse/draw.

## OVERVIEW
1. Create and seed v_ arrays in Pixil (as usual)
2. Define a program block once (grid_program or field_program)
3. Each frame: call grid_step(name) or field_render(name) inside begin_frame
4. After reseeding arrays, call grid_reset(name) so the engine reloads data

Program names are user-defined (like procedure names): conway, gray_scott,
voronoi_anim, metaballs, etc.

## GRID_PROGRAM

Definition syntax:

```text
grid_program <name> {
```
    size <scalar>
    cell <scalar>
    fields <array> [, <array> ...]
    steps <scalar>
    boundary clamp | wrap

    step <field_array> {
        <assignments>
    }

    draw {
        <draw directives>
    }
}

## Directives
size      Grid dimension (e.g. 32 for 32x32, 64 for 64x64). Array length must
          be size * size.
cell      Pixels per grid cell when drawing (1 = 1:1, 2 = each cell is 2x2).
fields    One or more flat numeric arrays (row-major: index = y * size + x).
steps     Simulation substeps per grid_step() call (usually 1).
boundary  clamp — edges use edge values; wrap — toroidal neighbors.

## Step blocks
Each step block updates one field. Assignments are expressions evaluated over
the whole grid at once. The final value for the field MUST be assigned to
<field>_next (e.g. step v_grid { ... v_grid_next = ... }).

Inside step expressions you can use:
- at(v_field)           Current cell value of a field
- lap(v_field)          4-neighbor Laplacian
- neighbors(v_field)    Count of alive neighbors (8-connected, value > 0.5)
- neighbors4(v_field)   Count of alive neighbors (4-connected)
- below_avg(v_field)    Average of cell below and its horizontal neighbors
- random_field(lo, hi)  Per-cell random values in range
- grid_x, grid_y        Per-cell coordinates (0 .. size-1)
- clamp(val, lo, hi)    Clip values
- where(cond, a, b)     Per-cell conditional (like np.where)
- min, max, abs, floor  Element-wise math
- sin, cos, sqrt, pow, atan2(y, x)   Element-wise math
- v_* scalars           Any Pixil numeric variable (v_dt, v_f, etc.)
- and, or, not, ==, !=, <, >, <=, >=

Example — Conway's Game of Life (see scripts/main/game_of_life.pix):

```text
grid_program conway {
```
    size v_size
    cell v_cell_size
    fields v_grid
    steps 1
    boundary clamp

    step v_grid {
```text
        v_n = neighbors(v_grid)
        v_alive = at(v_grid) == 1
        v_grid_next = where(v_alive and (v_n == 2 or v_n == 3), 1, where(v_n == 3, 1, 0))
```
    }

    draw {
        fill_field v_grid
        fill_color cyan
        fill_intensity 90
    }
}

## Draw block modes (grid_program)
palette — Map a field to rotating palette colors and intensity:
    palette_field v_v
    color_base v_color_base      # or literal
    color_scale 65
    intensity_base 28
    intensity_scale 72

fill — Plot solid color where field > 0.5:
    fill_field v_grid
    fill_color cyan
    fill_intensity 90

fire — Heat-based fire colors (black → red → orange → yellow):
    fire_field v_heat

expr — Custom per-cell plot with expressions:
    plot_if <expr>
    color <expr>
    opacity <expr>

## Grid commands
grid_step(name)       Run step(s), render draw block, queue pixels for frame
```text
grid_fill(v_arr, val) Fill a flat grid array with a constant (fast reseed)
```
grid_reset(name)      Discard cached engine state after you modify arrays
                      directly in Pixil (required after reseed / preset change)

Typical animation loop:

```text
while true then
    begin_frame
        grid_step(conway)
    end_frame
endwhile
```

After reseeding v_grid in Pixil:

```text
grid_reset(conway)
```

## FIELD_PROGRAM

Definition syntax:

```text
field_program <name> {
```
    size <scalar>
    sites <x_array>, <y_array>
    [weights <array>]
    [phases <array>]
    [active <array>]
    mode voronoi | metaballs | formula

    # formula mode only:
    value {
        <assignments>
    }

    # optional rendering (formula / metaballs):
    [plot_if <expr>]
    [color <expr>]
    [opacity <expr>]
    [signed true]
    [peak_color <color or v_var>]
    [trough_color <color or v_var>]
    [intensity_scale <scalar>]

    # voronoi mode only:
    [site_colors <array>]
    [edges true]
    [edge_ratio <scalar>]
    [edge_color <color>]
    [edge_opacity <scalar>]
    [fill_opacity <scalar>]
}

## Modes
voronoi   Nearest-site coloring with optional edge lines. Uses sites arrays
          only; no value block. Move sites in Pixil between field_render calls.

metaballs Built-in sum of (weight^2 / dist2) over sites. Optional weights
          array per site. Use plot_if / color / opacity for threshold coloring.

formula   Custom per-pixel value via value { } block. Use sum_sites(expr) to
          accumulate a formula over all active sites.

## Site variables inside sum_sites(expr)
dx, dy     Offset from current site to pixel
dist2      Squared distance to site
weight     Site weight (from weights array, or 1)
phase      Site phase (from phases array, or 0)
active     1 if site is active, 0 if disabled

Grid variables in value blocks: grid_x, grid_y (full 64x64 or size x size).
Pixil scalars (v_time, v_wavelength, etc.) work as in grid_program.

The last assignment in value { } is the field result (e.g. v_height = ...).

## Signed rendering (ripples / waves)
signed true with peak_color / trough_color / intensity_scale draws positive
values in peak_color and negative in trough_color. Intensity scales with
abs(value). See scripts/main/Blob.pix and scripts/main/Plasma_Tunnel.pix.

## Escape-time fractals (Mandelbrot / Julia)
For Mandelbrot and Julia sets, use mode formula with escape builtins in the
value { } block. These iterate z = z^2 + c per pixel in NumPy (no per-cell
Pixil loops). formula mode still requires a sites line — use one-element dummy
arrays if the field does not use moving sites (see Plasma_Tunnel.pix).

Builtins (field_program value / plot_if / color / opacity expressions):

  escape_iter(c_re, c_im, max_iter)
      Mandelbrot: z0 = 0, c = per-pixel (c_re, c_im).

  escape_iter(c_re, c_im, max_iter, z0_re, z0_im)
      Julia: c = constant (c_re, c_im), z0 = per-pixel (z0_re, z0_im).

  escape_zoom(anchor_re, anchor_im, dc_re, dc_im, max_iter)
      Deep-zoom Mandelbrot helper. c = anchor + dc per pixel. Uses direct
      evaluation when |dc| is large (wide view) and perturbation when |dc| is
      tiny (extreme zoom without float cancellation on anchor + offset).

  escape_perturb(anchor_re, anchor_im, dc_re, dc_im, max_iter)
      Low-level perturbation path used by escape_zoom. Scripts normally call
      escape_zoom instead.

Typical coordinate setup (map grid to complex plane, zoom with v_scale and
optional v_depth renormalization):

```text
    v_dx = grid_x - v_half
    v_dy = grid_y - v_half
    v_eff = v_scale * pow(10, v_depth)
    v_dc_re = v_dx / v_eff
    v_dc_im = v_dy / v_eff
    v_iter = escape_zoom(v_anchor_re, v_anchor_im, v_dc_re, v_dc_im, v_max_iter)
```

Render escaped pixels only (interior of the set has v_iter == max_iter):

    plot_if v_iter < v_max_iter - 1
    color clamp(sin(v_iter * 0.68 + v_stop_idx * 1.7 + v_hue_phase + grid_x * 0.1) * 47 + 52, 1, 99)
    opacity 80

plot_if, color, and opacity expressions may use grid_x, grid_y, v_iter (from the
value block result), and any v_ scalars (v_hue_phase, v_depth, v_stop_idx, etc.).

Guided tour pattern (scripts/main/Mandelbrot_Zoom.pix):
  Store landmark coordinates in v_stop_re[] / v_stop_im[] arrays. For each stop:
  reset v_scale to a wide value (new fractal shape visible), zoom in, then
  advance to the next waypoint before the view becomes all-interior (black).
  Cap max_iter as zoom deepens; increase v_hue_phase on each stop for color bloom.

See scripts/main/Mandelbrot_Zoom.pix for a full working example.

## Formula + threshold (metaballs style)
```text
field_program metaballs {
```
    size 64
    sites v_pos_x, v_pos_y
    weights v_radius
    mode formula
    value {
```text
        v_sum = sum_sites((weight * weight) / max(dist2, 1))
```
    }
    plot_if v_sum > 1
    color min(v_sum * 15, 95)
    opacity 75
}

## Field command
field_render(name)    Evaluate field and queue pixels for the current frame

Move site positions in Pixil each frame, then call field_render again. No
```text
grid_reset is needed for field programs — they read arrays fresh each render.
```

## CHLADNI LIBRARY
Chladni patterns use sand particles, not a full grid. `import chladni` and
call `chladni.step`. The procedure reads the script's particle arrays and
plots. It is not a `grid_program`.

```text
import chladni
call chladni.step()
```

Required Pixil state (same as Chladni_Patterns.pix):
- v_px, v_py          Particle position arrays (75 elements)
- v_n_scale, v_m_scale  Current mode scales (v_n * v_scale, v_m * v_scale)

Setup, mode cycling, and scatter logic stay in Pixil; each frame:

```text
begin_frame(false)
    draw_rectangle(0, 0, 64, 64, dark_gray, 40, false)
    call chladni.step()
end_frame
```

See scripts/main/Chladni_Patterns.pix.

## INK LIBRARY
Ink in Water diffuses particles. `import ink` and call `ink.step`. The
procedure reads the script's particle arrays and plots.

```text
import ink
call ink.step()
```

Required Pixil state (set before each drop's animation loop):
- v_px, v_py, v_vx, v_vy, v_intensity, v_alive, v_curve_x, v_curve_y
- v_drop_x, v_drop_y (spawn center for swirl)
- v_drift_x, v_drift_y, v_swirl_active, v_swirl_strength, v_swirl_dir
- v_fade_rate, v_color_bright, v_color_mid, v_color_dark

After each call, v_any_alive is 1 if any particle remains, else 0.
Spawn, palette, and behavior selection stay in Pixil:

```text
while v_any_alive == 1 then
    begin_frame
        call ink.step()
    end_frame
endwhile
```

See scripts/main/Ink_In_Water.pix.

## WORKFLOW PATTERN
```text
# 1. Arrays and constants in Pixil
v_size = 32
create_array(v_grid, v_size * v_size)
```
# ... seed v_grid ...

```text
# 2. Define program (once, at script load)
grid_program my_sim { ... }

# 3. Frame loop
fps(60)
while true then
    begin_frame
        grid_step(my_sim)
    end_frame
endwhile

# 4. After direct array edits
v_grid[v_i] = 1
grid_reset(my_sim)
```

## REFERENCE SCRIPTS
  game_of_life.pix           grid_program
  game_of_life_highres.pix   grid_program (64x64, cell 1)
  Reaction_Diffusion.pix     grid_program
  Fractal_Fire.pix           grid_program
  Brians_Brain.pix           grid_program (multi-state CA, expr draw)
  Wireworld.pix              grid_program (4-state CA, expr draw)
  Voronoi_Pattern.pix        field_program
  Metaballs_Effect.pix       field_program
  Blob.pix                   field_program (signed sum_sites)
  Plasma_Tunnel.pix          field_program (formula signed, dummy sites)
  Orbiting_Ripples.pix       field_program (sum_sites moving sources)
  Mandelbrot_Zoom.pix        field_program (escape_zoom fractal tour)
  Chladni_Patterns.pix       call chladni.step
  Ink_In_Water.pix           call ink.step
  Slime_Mold.pix             call agent.chemotaxis + grid_program

Test examples: scripts/testing/test_grid_program_*.pix,
scripts/testing/test_field_program_metaballs.pix
tests/pixil/test_fractal_escape.py

## COMMON PITFALLS
- Array size must equal size * size for grid_program fields
- Every step block needs an assignment to <field>_next
- Call grid_reset(name) after reseeding arrays outside grid_step
- field_program sites requires two arrays: sites v_px, v_py
- Expression identifiers must be v_ scalars, field names, or engine builtins
- grid_program / field_program blocks use brace nesting; inner { } in step/draw
  blocks are supported

# 22. PARTICLE LIBRARY

[↑ Table of Contents](#table-of-contents)

Motion that used to be a builtin `particle_*` command is a procedure in
`scripts/lib/particle.pix`. The procedures mutate numeric arrays in place and
do not draw. The script still owns spawning, scoring, wrap-versus-bounce
policy, random kicks, and rendering (`plot`, sprites, `begin_frame`).

```text
import particle
call particle.integrate(v_x, v_y, v_vx, v_vy, v_active, 0, 0.15)
```

Import rules:

- `import particle` loads `scripts/lib/particle.pix`. A path loads the same way: `import arcade/ai` loads `scripts/lib/arcade/ai.pix`.
- The prefix is the last path segment. `import particle` then `call particle.integrate(...)`.
- A second import that wants the same prefix is an error.
- Importing the same path twice is ignored.
- A library file may contain only `def` procedures. A drawing command, assignment, or loop at the top level of a library is an error.
- Inside a library, a procedure that calls a sibling must use the prefixed name (`call particle.collide_bounds(...)`). The prefix is applied after the library is resolved, so a bare `call collide_bounds(...)` does not find it.
- Arguments are evaluated before the procedure runs. Scalars the body assigns are restored when it returns. Arrays are shared, so writes into array cells persist.

There is no `particle_integrate(...)` command. A script that still uses one does not run on this producer.

Use them when many bodies share the same motion rule (balls, birds, cloth
knots). Keep a plain Pixil loop when you have only a few bodies or logic
the library procedures cannot express (open floor exit, wrapping positions
onto the panel, peg-hit random kick).

## ARRAY CONTRACT
State arrays x, y, vx, vy, and active must:
- Be numeric (create_array(v_x, n, numeric))
- Have the same length
- Use names starting with v_

active[i] != 0 means particle i is simulated; 0 is skipped.
Optional hit arrays are the same length as the state arrays. Each collide
```text
call that takes hit clears the whole hit array, then sets hit[i] = 1 for
```
contacts this call.

radius, mass, ax, ay, and restitution may be a numeric literal or a numeric
array. When they are arrays, length must match the particles (or obstacles,
```text
for obstacle_radius).
```

Optional count limits how many leading slots are processed (useful when
the array is larger than the live pool). Omit it to use the full array.
Do not pass a bare number in a slot that can be parsed as a hit array
name — if you need count after restitution/radius, pass the hit array
explicitly first.

Y grows downward (display origin is top-left). Restitution 1 is elastic;
values below 1 lose energy on bounce.

## MATERIALS (RESTITUTION)
Pass a per-particle numeric array as restitution so each body has its own
elasticity (rubber vs metal vs soft polymer). The kernels do not store
named materials — Pixil chooses values and optional colours/kicks.

Suggested starting values (used by pachinko_machine_v2.pix):
    rubber          0.95   lively bounce
    metal           0.72   firmer, less elastic
    soft polymer    0.42   absorbs most impact

How contacts combine:
- Walls and circular walls use that particle's restitution.
- Pegs / static circles currently use the particle's restitution (pegs
  are infinite-mass; there is no peg-material array yet).
- Particle-particle pairs use the lower of the two values, so the
  less-elastic material dominates energy loss.

Sideways random kicks after a peg hit stay in Pixil. Scale the kick
range with the same material if you want rubber to scatter more than
soft polymer.

Example:
```text
create_array(v_restitution, 24, numeric)
v_restitution[0] = 0.95
v_restitution[1] = 0.72
v_restitution[2] = 0.42
call particle.collide_bounds(v_x, v_y, v_vx, v_vy, v_active, 4.5, 58.5, -1000, 1000, v_restitution, 0.5)
call particle.collide_static_circles(v_x, v_y, v_vx, v_vy, v_active, v_peg_x, v_peg_y, 0.5, 1, v_restitution, v_hit)
```

Commands may be called inside or outside begin_frame; they only update
variables. Draw afterwards.

## particle.apply_attractors
```text
call particle.apply_attractors(x, y, ax, ay, active, attractor_x, attractor_y,
                          [attractor_strength], [particle_strength],
                          [falloff], [softening], [max_acceleration],
                          [count], [attractor_count], [swirl])
```

Adds each attractor's pull to the acceleration arrays. Positive strengths
pull; negative strengths push away. Both strength values may be scalar or
arrays: attractor_strength is one value per attractor and particle_strength
is one value per particle. Their values multiply, so signed arrays can model
magnetic charges.

falloff defaults to 1: pull becomes weaker as distance increases. Use 2 for
a faster inverse-square falloff, or -1 for a spring-like pull that grows with
distance. softening defaults to 0 and prevents extreme pull close to an
attractor. max_acceleration optionally caps the combined acceleration after
all pulls are added. swirl defaults to 0; a non-zero value adds sideways
spin around each attractor (positive is counterclockwise, negative is
clockwise). The command preserves existing ax/ay, so reset those arrays
in Pixil first when this is the only force.

Example — three gravity wells:
```text
call particle.apply_attractors(v_x, v_y, v_ax, v_ay, v_active, v_well_x, v_well_y, v_well_force, 1, 1, 1, 2.5, v_count, 3)
call particle.integrate(v_x, v_y, v_vx, v_vy, v_active, v_ax, v_ay, 0.998, 0, v_count)
```

## particle.flock
```text
call particle.flock(x, y, vx, vy, ax, ay, active, separation_distance,
               neighbor_distance, [separation_weight], [alignment_weight],
               [cohesion_weight], [max_speed], [max_force], [wrap_width],
               [wrap_height], [count])
```

Add three classic flock steering forces to ax/ay (Reynolds: separate,
align, cohere). Existing ax/ay values are kept, so set wander or a chase
target in Pixil first, then call this. The command does not move, wrap
positions, or draw.

Each active body looks at every other active body (O(n^2) over count).
Bodies closer than separation_distance push apart. Bodies closer than
neighbor_distance also contribute matching velocity (alignment) and a
pull toward the group's average offset (cohesion). Each of those three
steers is capped by max_force and aimed as if the body wanted to go at
max_speed, then scaled by its weight. Defaults: weights 1.5 / 1 / 1,
max_speed 2.5, max_force 0.3.

wrap_width / wrap_height default to 0 (open world: neighbor distance is
ordinary Euclidean). Pass the same world size you use when wrapping
positions in Pixil so neighbors across an edge still count. Positive
values use the shortest toroidal delta; they do not move x/y. If the
logical wrap box is larger than the 64x64 panel, pass that larger size
here and still wrap/draw in Pixil.

Scatter / panic is a show event: call again with a large separation
weight and 0 alignment/cohesion. Inactive slots (active=0) are skipped.

Example — flock, then apply steering, then move and wrap in Pixil:
```text
call particle.flock(v_x, v_y, v_vx, v_vy, v_ax, v_ay, v_active, 7, 15, 1.5, 1, 1, 2.5, 0.3, 64, 64, v_count)
call particle.integrate(v_x, v_y, v_vx, v_vy, v_active, v_ax, v_ay, 1, 0, v_count, post_move, 100, 0)
```
# Pixil: clamp speed by magnitude, wrap x/y into the world, draw

Example — scatter (separate only):
```text
call particle.flock(v_x, v_y, v_vx, v_vy, v_ax, v_ay, v_active, 7, 15, 4.5, 0, 0, 2.5, 0.3, 64, 64, v_count)
```

## particle.integrate
```text
call particle.integrate(x, y, vx, vy, active, [ax], [ay], [damping], [sleep_speed],
                   [count], [integration_mode], [max_speed], [position_scale])
```

For each active particle:
    vx += ax;  vy += ay
    x  += vx;  y  += vy
    vx *= damping;  vy *= damping
```text
    if |vx| or |vy| < sleep_speed, that component is set to 0
```

Defaults: ax=0, ay=0, damping=1, sleep_speed=0.
ax and ay may be scalars (same for every particle) or per-particle arrays.
integration_mode defaults to post_move: accelerate, move, then damp. Pass
pre_move to accelerate, damp, clamp, then move. max_speed is optional; when
given (as a scalar or per-particle array), it caps each velocity component
to +/- max_speed after damping. position_scale defaults to 1 and controls how
far velocity moves position without changing velocity itself. Both modes
apply sleep_speed after moving. Does not write a hit array.

Example — gravity, no air drag:
```text
call particle.integrate(v_x, v_y, v_vx, v_vy, v_active, 0, 0.15)
```

Example — damped, capped motion before movement:
```text
call particle.integrate(v_x, v_y, v_vx, v_vy, v_active, 0, 0.12, 0.998, 0, v_count, pre_move, 2.5)
```

## particle.verlet
```text
call particle.verlet(x, y, old_x, old_y, active, [ax], [ay],
                          [damping], [count])
```

Move a 2D point net using current and previous positions instead of explicit
velocity. These two cloth commands model fabric that lives in the screen
plane: a hanging sheet, curtain, net, or rope, with gravity (Y down) and
pinned anchors. They do not model a 3D flag blowing toward or away from
the viewer. Link constraints can move positions directly. active=0 means a
fixed point: it and its old position are unchanged. ax/ay may be scalar or
per-point arrays.

## particle.constrain_distances
```text
call particle.constrain_distances(x, y, active, link_from, link_to, rest_length,
                             [stiffness], [iterations], [tension], [count],
                             [link_count], [min_distance])
```

Correct an arbitrary list of links toward their requested lengths. Pair this
with call particle.verlet for hanging 2D cloth. link_from and link_to
hold point indexes. active=0 makes a point a fixed anchor while its links
still affect movable partners. rest_length and stiffness may be scalars or
per-link arrays. Multiple iterations make cloth firmer. Optional tension
receives each link's absolute stretch from the final pass.

## particle.apply_springs
```text
call particle.apply_springs(x, y, force_x, force_y, active, link_from, link_to,
                       rest_length, [stiffness], [tension], [count],
                       [link_count])
```

Add spring forces for an arbitrary list of links. Existing force arrays are
preserved, allowing gravity, wind, and other forces to be combined. Inactive
endpoints act as fixed anchors. Optional tension receives absolute spring
force for color or other show logic. Use call particle.integrate afterwards.

## particle.collide_bounds
```text
call particle.collide_bounds(x, y, vx, vy, active, left, right, top, bottom,
                        [restitution], [radius], [hit], [count])
```

Bounce active centres off a rectangle. radius (scalar or array) insets the
walls so a circle stays inside. Restitution is applied on the bounced axis
(default 1) and may be a scalar or per-particle array.

To leave an open floor (pachinko-style exit), set bottom far below the
display (for example 1000) so balls are never floor-bounced; deactivate
them in Pixil when y exceeds the exit line.

Wrap-around is not a kernel option — implement wrap in Pixil, or skip this
```text
command that frame.
```

Example — closed box, radius 1.65:
```text
call particle.collide_bounds(v_x, v_y, v_vx, v_vy, v_active, 5, 58, 5, 58, 0.88, 1.65, v_hit)
```

## particle.collide_circle_bounds
```text
call particle.collide_circle_bounds(x, y, vx, vy, active, center_x, center_y,
                               boundary_radius, [restitution], [radius],
                               [angular_speed], [grip], [hit], [count])
```

Keep particles inside a circular wall. Centres are projected to
boundary_radius - radius. Outward normal speed is reflected by
restitution (default 1, scalar or per-particle array). Optional angular_speed
and grip pull tangential velocity toward the rotating wall (tumbler / mixer).

Example:
```text
call particle.collide_circle_bounds(v_x, v_y, v_vx, v_vy, v_active, 32, 32, 28, 0.92, v_radius, 0.08, 0.35, v_hit)
```

## particle.collide_circles
```text
call particle.collide_circles(x, y, vx, vy, active, radius, [mass], [restitution], [hit], [count])
```

Resolve overlaps between active particles. Equal or unequal mass.
Restitution defaults to 1 and may be a per-particle material array. A pair
uses the lower value, so the less-elastic material controls energy loss.
Returns (to the runtime) the pair count; scripts typically ignore it and
read hit[] instead.

Overlapping pairs are separated even if already moving apart. Impulse is
applied only while approaching. Two fully stationary overlaps still get
positional correction so a packed rack does not stay stuck.

O(n^2) over the live prefix — fine for tens of bodies, expensive for
hundreds without a smaller count.

Example:
```text
call particle.collide_circles(v_x, v_y, v_vx, v_vy, v_active, v_radius, v_mass, 0.97, v_hit)
```

## particle.collide_static_circles
```text
call particle.collide_static_circles(x, y, vx, vy, active, obstacle_x, obstacle_y,
                                radius, obstacle_radius, [restitution],
                                [hit], [count], [obstacle_count],
                                [response])
```

Bounce particles off fixed circular obstacles. Obstacles have infinite
mass and are never moved. Overlaps are projected to contact.
Restitution default 1. Restitution may be a per-particle material array.
obstacle_count limits how many leading obstacles are used.

response defaults to impulse (omit it): velocity changes only while
approaching, and only the normal component is scaled by restitution
(tangent is kept). Pass reflect for a full-velocity bounce: every overlap
reflects the whole velocity, then scales that vector by restitution.
That dissipates speed on contact instead of letting bodies slide along
the obstacle.

Random kicks after a hit stay in Pixil: read hit[] after the call.
Pass a restitution array to give each body its own material bounce.

Full-velocity bounce example:
```text
call particle.collide_static_circles(v_ball_x, v_ball_y, v_ball_vx, v_ball_vy, v_ball_active, v_peg_x, v_peg_y, 0.5, 0.5, 0.82, v_peg_hit, 12, v_peg_count, reflect)
```

Impulse example:
```text
call particle.collide_static_circles(v_ball_x, v_ball_y, v_ball_vx, v_ball_vy, v_ball_active, v_peg_x, v_peg_y, 0.5, 1, v_ball_restitution, v_peg_hit, 24, v_num_pegs)
```

## TYPICAL FRAME
Balls / marbles:
```text
call particle.integrate(...)
call particle.collide_bounds(...)          # or wrap in Pixil; not both
call particle.collide_static_circles(...)  # optional pegs
call particle.collide_circles(...)         # optional ball-ball
```
# then Pixil: kicks, clamp speed, deactivate, draw

Wells / magnets (reset ax/ay in Pixil first if this is the only force):
```text
call particle.apply_attractors(...)
call particle.integrate(...)
```

Flock (wander/target into ax/ay first):
```text
call particle.flock(...)
call particle.integrate(...)               # apply steering; wrap x/y in Pixil
```
# then Pixil: magnitude speed cap, wrap, draw

Hanging 2D cloth / net (not a 3D wind-blown flag):
```text
call particle.verlet(...)
call particle.constrain_distances(...)     # several iterations inside the call
```
# then Pixil: draw links

Example shows:
  scripts/main/Billiard_Break.pix
  scripts/main/Pinball_Magnet_Table.pix
  scripts/main/Particle_Tumbler.pix
  scripts/main/Magnetic_Marbles.pix
  scripts/main/Ferrofluid_Iron_Filings.pix
  scripts/main/Gravity_Wells.pix
  scripts/main/Meteor_Shower.pix
  scripts/main/swarm.pix
  scripts/main/Boids_Flocking_Simulation.pix
  scripts/main/Cloth_simulation.pix
  scripts/main/Hanging_Lanterns.pix
  scripts/main/Seaweed_Forest.pix
  scripts/main/Swaying_Meadow_Grass.pix
  scripts/main/Spring_Network.pix
  scripts/main/Asteroid_Miner.pix
  scripts/main/pachinko_machine_v2.pix
  scripts/main/Galton_Board.pix
  scripts/main/Hourglass_Marbles.pix

# 23. AGENT, INK, CHLADNI, AND POINTS LIBRARIES

[↑ Table of Contents](#table-of-contents)

These used to be builtin commands. Each one is now a Pixil library under
`scripts/lib/`, loaded with `import` and invoked with `call`. The import rules
in section 10 apply to all of them.

`call agent.chemotaxis(...)` senses a flat grid field, steers, moves, and
optionally deposits into that field. It mutates arrays and does not draw.
Pair it with `grid_program` / `grid_step` for diffusion, evaporation, and
rendering (section 21). Keep food placement, phase timing, and palettes in
the script. Use the particle library (section 22) when motion is physics
forces without a grid field.

```text
import agent
call agent.chemotaxis(v_x, v_y, v_dir, v_phero, v_size, 5, 1, 0.55, v_n, 1)
```

## agent.chemotaxis
```text
call agent.chemotaxis(x, y, heading, field, size,
                 [sensor_dist], [step], [deposit], [count], [wrap])
```

Parameters:
- x, y          Numeric position arrays (grid coordinates, not necessarily pixels)
- heading       Numeric array of 8-way directions 0..7:
                  0=E  1=SE  2=S  3=SW  4=W  5=NW  6=N  7=NE
- field         Flat row-major numeric array, length size*size (pheromone / scent)
- size          Grid dimension (e.g. 32 for a 32x32 field)
- sensor_dist   How far ahead sensors look (default 5)
- step          Move distance per call (default 1)
- deposit       Amount added to field at the landing cell (default 0.55).
                Use 0 for pure followers that never reinforce the trail.
- count         Optional leading-slot limit (default: full array length)
- wrap          Non-zero (default 1): toroidal edges. 0: clamp to the field.

Each agent samples field ahead / left / right of its heading, turns toward the
strongest sample, steps forward, and deposits. Caps deposited cells at 1.5 so
hot spots stay bounded before grid_step evaporates them.

Typical frame (slime-style):
```text
# Pixil: inject food into v_phero, advance phases, etc.
call agent.chemotaxis(v_x, v_y, v_dir, v_phero, v_size, v_sensor_dist, v_step, v_deposit, v_n, 1)
begin_frame
    grid_step(diffuse)          # blur + evaporate + draw field
    for v_i in (0, v_n - 1, 1)
        plot(v_x[v_i] * v_cell, v_y[v_i] * v_cell, lime, 95)
    endfor v_i
end_frame
```

Notes:
- Coordinates are in *field* space (0 .. size-1). Scale by cell size when plotting
  on the 64x64 panel (e.g. plot(v_x[i] * 2, v_y[i] * 2, ...) for size 32 cell 2).
- grid_step syncs Pixil field edits (deposits / food) into the engine each frame —
  you do not need grid_reset after every deposit (still call grid_reset after a
  full reseed / grid_fill if you rebuild the world).
- There is no `slime_step` command. Call `agent.chemotaxis` with the arrays
  the script actually uses.

Example show:
  scripts/main/Slime_Mold.pix


## POINTS LIBRARY
`scripts/lib/points.pix` holds the old points helpers. They write into the
output arrays and do not draw.

```text
import points
call points.lerp(v_a, v_b, v_out, v_t, v_count)
call points.project3d(v_x, v_y, v_z, v_sx, v_sy, v_ax, v_ay, v_az, v_scale, v_camera, v_cx, v_cy, v_depth, v_count)
```

# 24. SCRIPT EXAMPLES

[↑ Table of Contents](#table-of-contents)

## SIMPLE BOUNCING BALL
```text
v_x = 32
v_y = 10
v_dx = 1
v_dy = 1

while 1 then
    begin_frame
        draw_circle(v_x, v_y, 3, red, 100, true)
    end_frame
    
    v_x = v_x + v_dx
    v_y = v_y + v_dy
    
    if v_x < 3 or v_x > 60 then
        v_dx = v_dx * -1
    endif
    if v_y < 3 or v_y > 60 then
        v_dy = v_dy * -1
    endif
    
    rest(0.033)
endwhile
```

## STARFIELD
```text
create_array(v_star_x, 50)
create_array(v_star_y, 50)
create_array(v_star_speed, 50)

# Initialize stars
for v_i in (0, 49, 1)
    v_star_x[v_i] = random(0, 63, 0)
    v_star_y[v_i] = random(0, 63, 0)
    v_star_speed[v_i] = random(1, 3, 0)
endfor v_i

# Animation loop
while 1 then
    begin_frame
        for v_i in (0, 49, 1)
            v_intensity = v_star_speed[v_i] * 33
            plot(v_star_x[v_i], v_star_y[v_i], white, v_intensity)
            
            v_star_x[v_i] = v_star_x[v_i] - v_star_speed[v_i]
            if v_star_x[v_i] < 0 then
                v_star_x[v_i] = 63
                v_star_y[v_i] = random(0, 63, 0)
            endif
        endfor v_i
    end_frame
    rest(0.05)
endwhile
```

## ANIMATED SPRITE EXAMPLE
```text
# Define a 2-frame animated character
define_sprite(character, 8, 8)
    sprite_cel(0)
        draw_circle(4, 2, 2, yellow, 100, true)
        draw_rectangle(2, 4, 4, 4, blue, 100, true)
    sprite_cel(1)
        draw_circle(4, 2, 2, yellow, 100, true)
        draw_rectangle(2, 4, 4, 4, green, 100, true)
endsprite

# Animate across screen
show_sprite(character, 0, 28)
for v_x in (0, 55, 1)
    move_sprite(character, v_x, 28)
    rest(0.1)
endfor v_x
hide_sprite(character)
```

# END OF SCRIPTING GUIDE

My current list of scripts are:

1. 3D Asterisk - Three-dimensional asterisk pattern animation
2. 3D Grid - 3D wireframe grid / floor-style depth grid animation
3. 3D Perspective - Dynamic 3D depth perception visualization
4. 3D Shapes - Animated three-dimensional geometric objects
5. Abstract Geometry - Abstract geometric shape compositions
6. Analog Clock - Traditional clock face with moving hands
7. Aurora Borealis - Northern lights simulation with flowing colors
8. Aurora Reactor - Dynamic energy bursts that ripple outward in vibrant, layered color waves, simulating an unstable reactor core
9. Blob - Organic amorphous shape movement
10. Boids Flocking Simulation - Simulated bird or fish group behavior
11. Bouncing Ball - Physics-based ball bounce animation
12. Bouncing Pixels - Multiple pixel particles with rebound effects
13. Breakout - Breakout game demo
14. Bridge Building - Watch various bridges being built and then see how they are stressed when trucks drive over them
15. Bubble Pop - Expanding and bursting bubble animation
16. Building Construction - Watch buildings being built in a city
17. Burnout - Different sequence patterns of pixels that fade out
18. Cafe Wall Illusion - Scrolling cafe wall illusion
19. Circles - Geometric circle patterns and transformations
20. Clock Wave - Time-based waveform visualization
21. Clockwork Gears - Interlocking gears turning together
22. Cloth Simulation - A large sheet blowing in the wind
23. Color Blend - Smooth transitioning between color palettes
24. Color Mandala - Symmetrical radial color patterns
25. Cosmic Nebula - Space gas cloud formation simulation
26. Cosmic Pulse - Rhythmic space energy visualization
27. Cracking - Ice and glass cracking effects
28. Crystal Growth - Geometric crystalline formation simulation
29. Dancing Squares - Choreographed square movement patterns
30. Digital Rain - Matrix-style falling character effect
31. Double Pendulum - Chaotic double pendulum physics simulation
32. Drone Show - Coordinated drone light show formation patterns
33. Dropped Ball - Physics-based falling object simulation
34. Earthquake Waves - Seismic wave propagation visualization
35. Elastic Collision - Physics-based collision simulation
36. Electric Arcs - Lightning-like electrical discharge effects
37. Electric Web - Interconnected electrical network visualization
38. Embers - Glowing particle effect simulation
39. Epicycles - Rotating circles creating complex patterns using Fourier series
40. Etch-a-Sketch - Classic drawing toy simulation
41. Expanding Circles - Circular growth pattern animation
42. Expanding Rings - Concentric circle growth patterns
43. Fireflies - Random illuminating light particles
44. Fireworks - Explosive particle burst patterns
45. Fish Tank - Aquatic life simulation
46. Flappy Bird - Side-scrolling obstacle avoidance game
47. Floating Geometry - Drifting geometric shapes animation
48. Fractal Fire - Burning fractal pattern simulation
49. Fractal Trees - Recursive branching structure growth
50. Fractured Mirror - Fragmented reflection patterns
51. Frost - Ice crystal formation spreading across the display
52. Game of Life - Conway's cellular automaton simulation
53. Geometric Kaleidoscope - Symmetrical geometric pattern rotation
54. Gravity Wells - Orbital motion around gravity points
55. Hex Grid Glow - Radiating hexagonal lattice pattern that pulses from the center with periodic color and intensity shifts
56. Hyperspace Jump - Star Wars-style jump to lightspeed effect
57. Hypnosis - Mesmerizing spiral pattern animation
58. Icicle Drop - Falling and forming ice crystal simulation
59. Ink in Water - Ink drops spreading out in water
60. Jellyfish - Jellyfish swimming around
61. Kaleidoscope - Symmetrical mirrored pattern animation
62. Kaleidoscope Tumbler - Kaleidoscope rotation/tumbling variant animation
63. Kinetic Rainbow - Motion-based color spectrum display
64. Laser Mirror - Laser rotates and bounces off walls/objects
65. Laser Show - Beam-based light performance simulation
66. Lightning - Electric discharge branching patterns
67. Liquid Starfield - Flowing star-like particle movement
68. Lissajous Curves - Mathematical curve pattern generation
69. Magic Carpets - Undulating textile-like pattern movement
70. Metaballs Effect - Organic blob interaction simulation
71. Missile Command - Classic arcade game simulation
72. Moire Patterns - Overlapping pattern interference effects
73. Morphing Prism - Shape-shifting color separation effect
74. Morse Code - Visual morse code transmission display
75. Motion Blindness - Optical illusion where yellow dots disappear
76. Mouse Escape - Interactive chase sequence
77. Newtons Cradle - Classic desk toy physics simulation
78. Orbital Wispfield - Smoothly drifting orbs trace elliptical paths around a central point, creating a soft ambient field
79. Oscilloscope Display - Electronic oscilloscope simulation
80. Outrun - Game demo of Outrun
81. Pachinko Machine - Simulates a pachinko machine with different boards
82. Paint Drops - Liquid color diffusion simulation
83. Parallax Meadow - Multi-layered scrolling landscape with depth effect
84. Particle Eruption - Volcanic-style particle emission
85. Particle Fountain - Upward streaming particle emission
86. Particle Tumbler - Rotating particle containment simulation
87. Pathfinder - Route-finding algorithm visualization
88. Pen Art - Multiple sequences of pens drawing art
89. Pendulum Wave - Physics-based harmonic oscillation patterns
90. Pendulums Front - Swinging line of pendulums from a front-perspective where one row appears in front and others behind
91. Perlin Noise Particle System - Organic flowing particle movement using Perlin noise
92. Phi Phenomenon - Optical illusion demonstrating apparent motion
93. Photon - Light particle simulation
94. Plasma Ball - Energy field touch-response simulation
95. Polygon Zoom - Centered polygon shapes zooming outward
96. PolyWobble - Undulating polygon deformation animation
97. Pong - Classic paddle and ball arcade game simulation
98. Prayers - Contemplative animation sequence
99. Psychedelic Tunnel - Immersive optical illusion animation
100. Pulse Matrix - Rhythmic grid pulsation effect
101. Qix - Geometric area capture game simulation
102. Quantum Particle Collision - Subatomic particle interaction simulation
103. Radar Sweep - Rotating radar beam scanning pattern
104. Rain - Falling water particle simulation
105. Rain Drops - Water droplet impact simulation
106. Rainbow Sweep - Rainbow sweeps diagonally across matrix from all corners
107. Rainbow Wave Collapse - Spectrum-based wave interference pattern
108. Raining Down - Enhanced precipitation effect
109. RGB Color Cube - Three-dimensional color space visualization
110. Ring Gate - Ring-gate style animation inspired by The Expanse
111. Rolling Cylinder - Cylindrical object rotation animation
112. Rotating Cube - Wireframe cube that rotates
113. Sand Drop - Granular material physics simulation
114. Sheet Music - Musical notation scrolling display
115. Sine Wave Color Flow - Mathematical color wave propagation
116. Ski Downhill - Downhill skiing game simulation
117. Snake - Classic growing-tail game simulation
118. Snowflake - Snowflake generation
119. Solar Bloom - Radiating sun-like bursts with pulsing center glow; each bloom may jitter or stay symmetrical
120. Solar System - Planetary orbital simulation
121. Sort Visualizer - Algorithm comparison visualization
122. Space Flight - First-person space travel simulation
123. Space Invaders - Classic arcade shooter game simulation
124. Space Shooter - Space combat shooting game
125. Spider Web - Spider web generation
126. Spiral2 - Enhanced spiral pattern generation
127. Spirograph Pen - Single-pen spirograph drawing simulation
128. Spirograph Sequence - Multiple spirograph pattern sequences
129. Spring Network - Connected spring physics simulation
130. Squares2 - Advanced square pattern generation
131. Starburst - Radial light explosion effect
132. Starfield - Space star simulation with depth
133. Steampunk Airship - Victorian-era flying vessel animation
134. Swarm - Collective movement behavior simulation
135. Tetris - Game demo of Tetris
136. The Matrix - Digital reality simulation effect
137. Tornado - Spiral vortex weather pattern simulation
138. Tron - Light cycle racing game inspired by Tron
139. Voronoi Pattern - Cellular geometric pattern generation
140. Vortex Spiral - Spinning funnel pattern animation
141. Watching Eyes - Cartoon animated eyes looking around and blinking
142. Wave Grid - Undulating coordinate mesh animation
143. Wormhole - Space-time tunnel visualization
144. Asteroid Miner - Drifting rocks that collide, split, and get mined by corner lasers
145. Pachinko Machine V2 - Same 24 boards as Pachinko Machine; particle primitives plus rubber/metal/soft-polymer balls (cyan/white/orange)
146. Galton Board - Bean-machine histogram with 16 balls in flight (queue-stable max); pegs use response=reflect
147. Hourglass Marbles - 24 mixed-material marbles collide, bounce through a sealed glass neck, pile up, and rotate back to the top
