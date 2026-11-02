# flycoos

An original voxel action-RPG in C++23. Colored-voxel art, a procedural open world, and third-person
action. Over time it grows open-world RPG systems (quests, factions, skills you level by using them)
and MMO-style systems (attributes, talents, group dungeons), without classes.

This repository is **milestone 0**: a playable prototype with a voxel character, a small procedural
world with a day/night cycle, and procedural walk/idle animation. It runs at roughly 300+ FPS on an
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
| Space | Jump / swim up (fly: rise) |
| Ctrl | Fly: sink |
| Mouse | Orbit the camera |
| Wheel | Zoom |
| T (hold) | Fast-forward time (60×) |
| Left mouse | Attack: sword slash with one equipped, otherwise punch (hold to keep swinging) |
| E | Loot a nearby corpse, otherwise talk to a nearby villager |
| C | Character: equipment and attributes |
| B | Bags: backpack, bags and bag slots |
| `.` | Open/close the debug window |
| Esc | Close the open windows, otherwise quit |

In the Character and Bags windows, drag items between slots and right-click to equip or unequip.
The mouse cursor is free while any window is open.

## Debug window

Press `.` to open it. The mouse cursor is freed while it's open; the keyboard still moves the player
unless a text field has focus.

| Tab | What's there |
|---|---|
| Render | View mode (shaded, greedy quads, chunks, ambient occlusion, normals), wireframe quad edges, sky/fog/water/character toggles, frustum culling on/off or frozen, per-frame chunk/quad/draw-call counts |
| Player | Health, the five attributes and what derives from them (max health, regen, attack damage and speed, combat state), position (editable), velocity, ground/water state, respawn, walk/run/jump speed, gravity, fly, noclip, fly speed |
| Items | Give any item, see what's equipped and in the bag slots, empty the inventory |
| Entities | Every NPC: health and attributes, traits and temper, aggro/leash/chase numbers, angry or calm (toggle), drop chance and what its corpse holds, position, wander state; hit for 10 or kill (attackable only), make talk, respawn NPCs near the player |
| Camera | Field of view, distance (up to 80 for overviews), terrain collision, HUD on/off, mouse sensitivity |
| World | Time of day, time speed (0 = paused), seed + regenerate, generation timings and quad counts |
| Stats | Frame-time graph of the last 240 frames, average and worst frame |

## Command-line options

| Flag | Meaning |
|---|---|
| `--seed N` | World seed (default 1337) |
| `--time T` | Start time of day, 0..1 (0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset) |
| `--frames N` | Benchmark: the player walks a circle and the camera orbits; exits after N frames and prints frame-time stats. Vsync is off. |
| `--screenshot out.bmp` | With `--frames`: save the last frame as a BMP |
| `--look YAW PITCH` | With `--frames`: fixed camera angles in degrees instead of the orbit (yaw 0 looks toward −Z, −90 toward +X; negative pitch looks up) |
| `--debug` | Start with the debug window open (it's also captured by `--screenshot`) |
| `--view MODE` | Start in a debug view: `shaded`, `quads`, `chunks`, `ao`, `normals`, or `wire` (wireframe) |

Example benchmark at night:

```sh
./build/release/flycoos --frames 3000 --seed 42 --time 0.02 --screenshot build/night.bmp
```

## Layout

```
src/world.*      voxel grid, region-based terrain (forest / rocky / plains), trees + rocks, parallelFor
src/mesher.*     greedy mesher with ambient occlusion -> 8-byte packed quads
src/player.*     movement physics (+ fly/noclip tuning), camera collision, character models, procedural animation
src/stats.h      the five attributes and the stats derived from them
src/entity.*     NPCs: dummy, villager, boar, bandit (traits, temper, wander/chase AI, talking, loot), attacks and damage
src/item.*       items, inventory (equipment, backpack, bags), move/equip rules
src/inventory_ui.*  Character (C) and Bags (B) windows
src/hud.*        in-game overlay: nameplates, speech balloons, damage numbers, talk/loot hint, player health, knocked-out message
src/renderer.*   SDL3 GPU (Metal) renderer: pipelines, uploads, culling, day/night lighting
src/debug_ui.*   Dear ImGui debug window (toggle with .)
src/main.cpp     window, input, camera, main loop, benchmark mode
shaders/         world.metal (voxel, water, sky), compiled at runtime
tests/           core_test.cpp: mesher, packing, physics, flight, camera, combat, NPC fights and loot, inventory, generation checks
```
