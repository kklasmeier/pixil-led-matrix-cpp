# pixil-led-matrix-cpp

Native (C++) **Pixil language runtime** — Phase 1 producer for the LED matrix show stack.

## Purpose

Pixil scripts (`.pix`) stay the product. This repo replaces the Python producer (`Pixil.py`) with a compile-once C++ runtime that feeds the **existing Python** `rgb_matrix_lib` consumer (legacy tree).

Phase 2 (native consumer) is later and out of scope until Phase 1 is solid.

## Charter

Read first: **[docs/Native_Pixil_Runtime_Phase1_Plan.md](docs/Native_Pixil_Runtime_Phase1_Plan.md)**

## Layout on the Pi

| Path | Role |
|------|------|
| `~/Lightshow/git/` | **Legacy** Python Pixil + `rgb_matrix_lib` (oracle / consumer host) |
| `~/Lightshow/pixil-led-matrix-cpp/` | **This repo** |

Dev machine (Pi 5) mounts Pi 4 `~/Lightshow` at  
`/home/pi/Programming/piMatrix/Lightshow/`.

**Edit** on Pi 5 via NFS. **Build and run** on Pi 4 via SSH (do not compile through NFS).

```bash
ssh pi@192.168.1.58 'cd ~/Lightshow/pixil-led-matrix-cpp && cmake --build build -j3'
```

## Status

Bootstrap only — no runtime yet. Follow plan milestones M0 → M7.

## Legacy reference

- Scripts: `~/Lightshow/git/scripts/`
- Language guide: `~/Lightshow/git/docs/Pixil_Scripting_Guide.txt`
- Consumer: `~/Lightshow/git/rgb_matrix_lib/`
