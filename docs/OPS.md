# Operating the show

`./run` is the launcher. `pixil` is the producer; it spawns `pixil-consumer`
to draw the panel.

Build on the Pi 4, from this repo:

```text
nice -n 19 make -j2
```

In the foreground, **space** skips to the next script and **r** reloads the
current script from disk (re-parse, same playlist position). `./run start` is
not a terminal, so those keys do nothing there; use `./run next`.

Play one script for 20 seconds:

```text
./run interactive -- main/Cloth_simulation.pix -t 0:20
```

Play every script in `scripts/main`, 5 minutes each, in the background. A
wildcard is reshuffled after each pass and keeps going until it is stopped.
With no script arguments, `start` and `interactive` use `main/* -t 60:00`.

```text
./run start -- main/* -t 5:00
./run status
./run next
./run jump main/Bouncing_Pixels.pix
./run stop
tail -f pixil.log
```

`./run stop` signals the producer. The producer drains or shuts down the
consumer, and the consumer blacks the panel out.

Paths are relative to `scripts/` when they are not already a file. `main/name`
and `main/name.pix` are the same script. `--headless` runs the consumer without
GPIO. `--dry-run` does not start the consumer, and a wildcard plays one pass
in sorted order.

Debug levels: `DEBUG_OFF` (default), `DEBUG_CONCISE`, `DEBUG_SUMMARY`,
`DEBUG_VERBOSE`. `-q` prints, about once a second, the script name, elapsed
time, producer frame/command counts, `cfps=` (consumer presents per second:
`end_frame` plus immediate swaps), and how full the inbox is:
`inbox=120/5000 2%`. That inbox holds commands waiting to be drawn. 5000 is
full, which means the consumer is the limit.

The producer paces on `fps` / `rest` (unless `--dry-run`) so wall-clock reads
like `get_datetime` stay in step with the script — otherwise a fast producer
can stamp hundreds of frames with the same second before the inbox drains
(frozen clocks such as `Boid_Digits`). The consumer pumps burnouts on a timer
while commands flow (not only when the inbox is idle), so preserve-frame
trails like `Sine_Wave_Color_Flow` expire correctly.

Useful checks:

```text
./build/release/pixil --headless --fingerprint --max-commands 800 main/bouncing_ball.pix
make c5-ops            # -q inbox sample + emergency-blackout path smoke
```

`--fingerprint` prints `PIXIL_BUFFER_HASH=` after each script (drawing buffer
only; see `DEVIATIONS.md` D-012).

While a show is running, its status is `/dev/shm/pixil_native_status.json`.
`./run next` and `./run jump` write `/dev/shm/pixil_native_control.json` for
that process. Each script starts from `--seed` (default 1).
