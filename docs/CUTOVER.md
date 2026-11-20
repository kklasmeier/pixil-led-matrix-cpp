# Cutover

The daily show runs from this tree with the **native producer and native
consumer** (`pixil` + `pixil-consumer`). There is no Python/legacy consumer path
in this repo.

## Daily show

On the Pi 4:

```text
cd ~/Lightshow/pixil-led-matrix-cpp
./run start
```

That plays every script in `scripts/main` for 60:00 each, then reshuffles and continues. A shorter cycle:

```text
./run start -- main/* -t 5:00
```

`./run status` shows the current script. `./run next` ends it, clears its sprites, and names the script that follows. `./run stop` blacks the panel out.

## Build

Edit on the Pi 5. Compile on the Pi 4, in `~/Lightshow/pixil-led-matrix-cpp`:

```text
nice -n 19 make -j2
```

`--headless` runs the consumer without GPIO. `--dry-run` does not start the consumer.

## Emergency blackout

If the consumer dies uncleanly, the producer runs `pixil-consumer --blackout`
(hardware fill + swap). Headless runs skip that path.
