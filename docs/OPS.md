# Operating the show

`./run` is the launcher. The `pixil` binary is the producer. The panel is still
drawn by the legacy Python consumer, which `pixil` starts itself.

Build on the Pi 4, from this repo:

```text
nice -n 19 make -j2
```

In the foreground, space skips to the next script. `./run start` is not a
terminal, so space does nothing there; use `./run next`.

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

Debug output follows the legacy names: `DEBUG_OFF` (the default),
`DEBUG_CONCISE`, `DEBUG_SUMMARY`, and `DEBUG_VERBOSE`. `-q` prints, about once
a second, the script name, elapsed time, frame count, command count, and how
full the bridge inbox is: `inbox=120/5000 2%`. That inbox holds commands
waiting to be drawn. 5000 is full, which means the consumer is the limit.

While a show is running, its status is `/dev/shm/pixil_native_status.json`.
`./run next` and `./run jump` write `/dev/shm/pixil_native_control.json` for
that process. Each script starts from `--seed` (default 1).
