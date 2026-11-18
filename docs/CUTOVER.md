# Cutover

The daily show runs from this tree. The legacy producer stays installed and is the fallback. The panel is still drawn by the legacy Python consumer. Do not start both producers at once; they both want the matrix.

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

The playlist is every script in this repo's `scripts/main`: the legacy main library plus `Gravity_Plexus.pix` (219 files as of the cutover). The legacy producer stays in its own tree and its own GitHub repository.

## Fallback

```text
cd ~/Lightshow/git
./run stop
./run start -- main/* -t 60:00
```

Stop the native show first. `./run stop` in this tree signals a root-owned producer with `sudo -n`.

## Build

Edit on the Pi 5. Compile on the Pi 4, in `~/Lightshow/pixil-led-matrix-cpp`:

```text
nice -n 19 make -j2
```

`--headless` runs the consumer without GPIO. `--dry-run` does not start the consumer.
