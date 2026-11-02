# flycoos

An original voxel action-RPG in C++23. Colored-voxel art, a procedural open world, and third-person
action. Over time it grows open-world RPG systems (quests, factions, skills you level by using them)
and MMO-style systems (classes, talents, group dungeons).

This repository is **milestone 0**: a playable prototype with a voxel character, a small procedural
world with a day/night cycle, and procedural walk/idle animation. It runs at roughly 1000 FPS on an
Apple M4.

- [docs/VISION.md](docs/VISION.md): what the game is, and what it is not
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the engine works (modules, frame flow, voxel pipeline)
- [docs/DECISIONS.md](docs/DECISIONS.md): why the tech stack is what it is (ADRs)
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones M0–M5

## Requirements

| Tool | Version used | Install |
|---|---|---|
| macOS on Apple Silicon | 15.x | n/a |
| Xcode Command Line Tools (AppleClang) | 17 | `xcode-select --install` |
| CMake | ≥ 3.28 | `brew install cmake` |
| Ninja | any | `brew install ninja` |
| SDL3 | ≥ 3.2 (tested 3.4.18) | `brew install sdl3` |

glm and FastNoiseLite are header-only. CMake downloads them on the first configure, so the first
configure needs network access. Full Xcode is **not** required, because shaders compile at runtime.

## Build, test, run

```sh
cmake --preset release            # configure (Ninja, Release) into build/release
cmake --build --preset release    # build flycoos + core_test
ctest --preset release            # run engine-core checks
./build/release/flycoos           # play
```

The `dev` preset (`cmake --preset dev`, etc.) builds Debug with AddressSanitizer, UBSan and the SDL
GPU debug layer.

## Controls

| Input | Action |
|---|---|
| W A S D | Move (relative to the camera) |
| Shift | Run |
| Space | Jump / swim up |
| Mouse | Orbit the camera |
| Wheel | Zoom |
| T (hold) | Fast-forward time (60×) |
| Esc | Quit |

## Command-line options

| Flag | Meaning |
|---|---|
| `--seed N` | World seed (default 1337) |
| `--time T` | Start time of day, 0..1 (0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset) |
| `--frames N` | Benchmark: the player walks a circle and the camera orbits; exits after N frames and prints frame-time stats. Vsync is off. |
| `--screenshot out.bmp` | With `--frames`: save the last frame as a BMP |
| `--look YAW PITCH` | With `--frames`: fixed camera angles in degrees instead of the orbit (yaw 0 looks toward −Z, −90 toward +X; negative pitch looks up) |

Example benchmark at night:

```sh
./build/release/flycoos --frames 3000 --seed 42 --time 0.02 --screenshot build/night.bmp
```

## Layout

```
src/world.*      voxel grid, terrain + tree generation, parallelFor
src/mesher.*     greedy mesher with ambient occlusion -> 8-byte packed quads
src/player.*     movement physics, collision, character model, procedural animation
src/renderer.*   SDL3 GPU (Metal) renderer: pipelines, uploads, culling, day/night lighting
src/main.cpp     window, input, camera, main loop, benchmark mode
shaders/         world.metal (voxel, water, sky), compiled at runtime
tests/           core_test.cpp: mesher, packing, physics, generation checks
```
