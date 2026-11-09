# flycoos

An original voxel action-RPG in C++23. Colored-voxel art, a procedural open world, and third-person
action. Over time it grows open-world RPG systems (quests, factions, skills you level by using them)
and MMO-style systems (attributes, talents, group dungeons), without classes.

This repository is **milestone 0**: a playable prototype with a voxel character, a small procedural
world with a day/night cycle (72 minutes), and procedural walk/idle animation. It runs at roughly 300+ FPS on an
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
./build/release/flycoos           # play the endless world (streamed around you; --seed picks it)
./build/release/flycoos --world worlds/story.flyw   # or the hand-made story world
```

The `dev` preset (`cmake --preset dev`, etc.) builds Debug with AddressSanitizer, UBSan and the SDL
GPU debug layer.

## Controls

| Input | Action |
|---|---|
| W A S D | Move (relative to the camera) |
| Shift | Run |
| Space | Jump / swim up (fly: rise) |
| Alt (Option) | Roll the way you're moving: on the ground, or just after jumping to roll through the air; jump while rolling to jump out of it. Standing still it's a backstep (ADR-038): a quick hop back, still facing ahead; attack out of it for a lunging sweep. Heavier gear rolls shorter and slower; too much and you can't roll (the equip load, ADR-039, in the character window) |
| Ctrl | Fly: sink |
| Mouse | Orbit the camera |
| Wheel | Zoom |
| T (hold) | Fast-forward time (60×) |
| Left mouse | Attack (Elden Ring style, ADR-035): tap for the next of four light swings (a chain), hold for a heavy that charges while held. Running, late in a roll, in the air (it strikes on landing), right after a block (a guard counter) or at an enemy on its knee (a riposte) it's a different attack. Presses wait a moment for the current swing |
| Right mouse | Guard: blocks hits from the front for stamina (the green bar); too little stamina and the guard breaks. A shield in the off hand guards better; a tower shield makes weak attacks bounce off. Raised just as a hit lands it's a perfect guard (ADR-041): no damage, half the stamina, a bright clink |
| Q | Parry (ADR-036) with a buckler or heater shield, or with the off hand empty a knife or bare fists (ADR-041): timed just before a hit lands, it deflects the attack and leaves the attacker open to a riposte (left mouse); mistimed, it's a weak guard |
| E | Loot a nearby corpse, otherwise talk to a nearby villager or guard |
| C | Character: equipment, attributes and armor |
| B | Bags: backpack, bags and bag slots |
| J | Journal: active and finished quests, the current entry and objectives |
| K | Progress: level and XP, skills, talent trees (click a highlighted talent to learn it) |
| `.` | Open/close the debug window |
| Tab | Lock on to the enemy nearest the middle of the view (again: let go). With nothing in reach the camera swings behind you. Locked on, you walk facing the target and attack toward it (running and rolls go where you steer); the arrow keys, or a quick flick of the mouse, switch to the next enemy that way on screen. A kill moves the lock to the next enemy in reach |
| F1 | Enter/leave the world editor (see below) |
| F2 | With `--debug`: write a debug note. The game pauses; the note goes to `debug_notes/notes.jsonl` with the game's state (player, combat, nearby NPCs, quests, latest events) and a screenshot from that moment. Each note starts `"status": "open"` and becomes `"done"` with a `"resolution"` once handled; list the open ones with `jq -c 'select(.status != "done") \| [.id, .note]' debug_notes/notes.jsonl` |
| F6 | Spawn an NPC 5 blocks in front of you, a bandit unless the debug window's Entities tab picks another kind (for trying out fights; it isn't saved) |
| F5 / F9 | Save / load the game (the player save next to the world file, e.g. `worlds/story.flys`) |
| Esc | Close the open windows, otherwise quit (asks twice with unsaved world edits); quitting saves the game (see below) |

A controller works too (SDL's gamepad layout; PlayStation names here, an Xbox pad's buttons sit in the same places):

| Input | Action |
|---|---|
| Left stick | Move; with a window open, move the cursor |
| L3 (click the left stick) | Run, until the stick lets go |
| Right stick | Orbit the camera; locked on, a flick switches target |
| R3 (click the right stick) | Lock on / let go (like Tab) |
| Cross | Jump / swim up (fly: rise); with a window open, click |
| Circle | Roll (like Alt); fly: sink; with a window open, close the windows |
| Square | Loot or talk (like E); with a window open, right-click (equip / unequip) |
| R1 | Attack (like left mouse: tap for lights, hold for a heavy) |
| R2 | Heavy attack (hold to charge) |
| L1 | Guard (like right mouse) |
| L2 | Parry (like Q) |
| Triangle | Bags |
| D-pad left / up / right | Character / Journal / Progress; locked on, the D-pad (down too) switches to the next enemy that way |
| Options | Close the windows |
| Create | Open/close the debug window (its Camera tab: stick look speed and deadzone, live stick values) |

In the Character and Bags windows, drag items between slots and right-click to equip or unequip;
Ctrl+right-click (or Cmd+right-click) destroys an item. With the bags full, loot goes straight into
its empty equipment slot.
The mouse cursor is free while any window is open.

Quests (`data/quests/*.json`, ADR-026) move on when you talk to, kill or loot what they name;
the top of the screen says when one starts, updates or completes, and NPCs say other lines as
they do. The first one is "Torvald's Helm" in Ravensford (see docs/STORY.md).

Fighting improves skills (ADR-027): hitting with a sword raises One-Handed, punching Unarmed,
taking hits in armor Armor; each skill level makes you a little better at it. Kills and skill
levels give character XP; every level up is a talent point to spend in the Progress window (K).

The game loads its world's player save on start, if there is one (`--new-game` starts fresh). The
save holds the player, the inventory, the quests and world flags, and every NPC (health, dead or alive and its respawn timer,
loot, position); the world file stays as the editor saved it. A save from before the world was
edited loads what still matches and lists what it dropped in the debug window's Save tab.
Quitting doesn't save after a save failed to load (F5 neither: save over it or delete it in the
Save tab), after Delete (until the next F5), or once the editor was opened (F5 still saves).

## World editor

The running game is the world editor. Start it on a world file:

```sh
./build/release/flycoos --world worlds/story.flyw   # loads the file, or generates from --seed if it doesn't exist yet
```

F1 enters editor mode: you fly with noclip, NPCs and combat pause, and a crosshair marks where the
brush lands (the ray from the camera through the screen center, past your character). A ring shows
the brush on the ground. Fly up to aim farther away.

| Input | Action |
|---|---|
| Left mouse | Apply the brush at the crosshair (hold: 8 times a second). Fill box: click two corners. Prefab: place, or select the prefab you aim at. Marker: place, or select the marker there |
| 1–9 | Tool: Raise, Lower, Smooth, Flatten, Paint, Fill box, Plant, Prefab, Marker |
| R | Prefab tool: rotate the next structure a quarter turn |
| Delete / Backspace | Remove the selected prefab instance (Prefab tool) or marker (Marker tool) |
| `[` `]` | Brush radius |
| Q | Free the cursor to use the Editor panel (tool, radius, strength, height, material); Q again to aim |
| Ctrl+Z / Ctrl+Shift+Z or Ctrl+Y | Undo / redo (Cmd works too) |
| Ctrl+S | Save to the `--world` file (`worlds/world.flyw` without `--world`) |

- **Raise / Lower** add or remove blocks on top of each column, most at the center. Raised ground
  keeps its surface material on top with subsoil (dirt under grass) below.
- **Smooth** pulls columns toward their neighbours' average height; **Flatten** pulls them toward a
  height, taken from where each stroke starts. Strength caps the blocks moved per application.
- **Paint** recolors the surface; **Fill box** sets a box of voxels (Air clears). Both take any
  block, building blocks (Planks, Cobble, Thatch, Ember...) too; Paint only solid ones.
- **Plant** grows grass and flowers on the open grass in its circle: the meadow mix, or only tall
  grass or one flower, at a density (1 is the generator's meadow), or clears them. Painting the same
  spot again changes nothing: every column has its own roll. Plants don't collide, and go with
  their grass: raised and lowered with it, cleared where it's dug out or painted over.
- Brushes act on each column's highest solid voxel, so trees and rocks are treated as terrain. The
  surface they keep on top is the top block, but cutting a tree or structure down to or below the
  ground under it leaves the ground's surface (grass, not leaves); a painted road stays a road.
  They never create or drain water: lowering a lakebed keeps it under water, lowering dry land
  makes a dry pit even below sea level.
- **Prefab** places a tree, rock, house, camp... (picker, params sliders, seed reroll, rotation in
  the panel) on the aimed voxel; a box previews it. Each placement is an *instance*: clicking one of
  its voxels selects it, Delete removes it and puts back what was there (except what you sculpted
  over it since). The panel lists all instances with Go to and Remove buttons and any prefab file
  that failed to load. See "Prefabs" below.
- **Marker** (8) places a story marker on the aimed voxel: pick the kind, type a name, set the radius
  (and for a spawn the NPC kind, count and dialogue lines). Clicking a marker selects it and loads its settings;
  **Update selected** applies changed ones (the kind stays; changing Kind deselects), Delete removes
  it. Markers show as pins with their id and
  name, regions as circles on the ground; the panel lists them with Go to and Remove. See "Story
  markers" below.
- The Editor panel shows undo depth, unsaved changes and when you last saved, plus debug numbers
  (aimed voxel, last op time and size, remesh time).

`worlds/` isn't git-ignored: the story world is meant to be committed. A world file is ~3 MB.

## Shaping the world with Claude (MCP)

The game runs a small command server, and `tools/flycoos_mcp.py` turns it into an MCP server, so
Claude Code can edit the world while you watch and edit alongside it.

```sh
./build/release/flycoos --world worlds/story.flyw   # 1. start the game (it listens on 127.0.0.1:47800)
claude                                              # 2. in this repo: .mcp.json registers the "flycoos" MCP server
```

The bridge needs only `python3` (no packages). Claude Code asks once to allow the project's MCP
server. If the game isn't running, the tools say how to start it.

| Tool | What it does |
|---|---|
| `state` | World file, size, seed, unsaved flag, undo/redo depth, player and camera, editor mode, time of day |
| `raise` `lower` `smooth` `flatten` `paint` | The editor's brushes at a column `x`, `z` with `radius`, `strength`, `height` (flatten), `material` (paint) |
| `fill` | Set a box (`min`, `max`, at most 128³) to any block (Planks, Cobble, Ember... too); Air clears, Water makes water |
| `plant` | The Plant brush: grass and flowers in a circle, `kind` (ShortGrass for the meadow mix, TallGrass, a flower, or Air to clear) and `density` |
| `meadow` | The Plant brush over a region (default the whole world), one undo step |
| `apply` | Up to 256 of the brush and fill ops above in one call (within one command's work budget, see below) |
| `path` | A road or river bed along `[x, z]` points: `width`, `follow_terrain` (graded) or a fixed `height`, `depth`, `material` |
| `undo` `redo` | One shared history: Claude's edits and yours undo in order, each command is one step |
| `save` | Save to the world file, or a copy to `path` (a `.flyw` in `worlds/` or the world file's folder) |
| `screenshot` | Render a view from `position` + `look_at` (or `yaw`/`pitch`), up to 1600×900, optional `fov` and `time`; your view isn't touched |
| `map` | Top-down PNG of a region: surface colors, hillshade, water depth, labelled grid |
| `heights` | A region's ground heights as numbers (≤ 4096 samples) and a histogram of top blocks |
| `inspect` | Block counts in a box |
| `goto` | Fly your camera (editor mode) to a point, facing `look_at`, if **Follow Claude** is on |
| `prefabs` | List the prefabs (rescans `prefabs/` first, with parse errors), their params and sizes |
| `place` | Place a prefab at column `x`, `z` (optional ground `y`, `rotation` 0/90/180/270, `seed`, `params`); returns its instance id and box, and the markers it brought |
| `move` `remove` | Move an instance (same id, one undo step) or remove it; its own markers move and go with it |
| `instances` | The placed instances, optionally those touching a region |
| `markers` | The story markers, optionally of one `kind` and in a region |
| `mark` | Add a marker: `kind`, `name`, column `x`, `z` (optional ground `y`), `radius`, and for a spawn `entity`, `count` and `lines`; returns its id |
| `edit_marker` `unmark` | Rename, move, retune or rewrite the lines of a marker by `id` (fields left out stay), or remove it |

Every command shows in the Editor panel's **MCP** section (last 20, with timings; rejected ones in
red), and each edit's box flashes in the world for 3 seconds. **Follow Claude** (on by default) lets
`goto` move your camera.

Safety: the server listens on 127.0.0.1 only and has no authentication. Any program on this machine
can edit the open world while the game runs; it's a development tool. `--no-serve` turns it off,
`--port N` moves it (set `FLYCOOS_PORT` for the bridge to match). A web page can't drive it: a line
that isn't a JSON object (such as an HTTP request line) closes the connection before anything runs.
`save` only writes `.flyw` files directly in `worlds/` or next to the open world file. Requests nested
deeper than 64 levels are refused, and one edit command has a work budget (an estimated 50 ms of
game time: big brushes and a wide spread cost most; e.g. a 5-wide road of 160–400 blocks) so it
can't stall the game: split bigger jobs. A client that sends no complete line for 5 s is dropped. Commands wait while
the window is minimized. `python3 tools/test_mcp.py [--binary build/dev/flycoos]` runs an end-to-end
check (it opens the game on a scratch world, on a free port).

## Prefabs

Generated prefabs grow from a few parameters with the same code as the world's own decoration:
the trees `tree` (an oak: height, crown), `maple` (height, crown, colour), `birch` (height, crown),
`pine` (a conifer: height) and `araucaria` (height, crown), plus `bush` (radius), `boulder` (radius)
and `spire` (height, radius); crown 0 sizes it by the height (docs/ART.md "Trees"). The `replant`
command turns an older world's baked trees into these, as instances. Structures are text files in `prefabs/` (`cottage`, `tent`, `camp`, `well`, `fence`,
`ruin`); write a new one and the `prefabs` command (or **Rescan** in the panel) picks it up. The
file name is the prefab name. This is `prefabs/fence.txt`:

```
description: Short wooden fence segment along x, posts every 3 blocks
foundation: Wood
W = Wood
P = Planks

layer
W  W  W
layer
WPPWPPW
layer
W  W  W
```

- Header: `description:`, optional `foundation: <Block>` (fills down from the bottom layer's solid
  cells to the ground, so it sits on slopes), optional `y: N` (the bottom layer sits at ground
  + 1 + N; `-1` sinks it one block). Legend lines `X = Block` map one character to a block name.
- Layers go bottom to top, each starting with a line `layer`. A layer's lines are rows along +x,
  one per z, so it reads like the top-down map. `.` is Air (clears the world's voxel), a space
  keeps it. A blank line right after `layer` is a row of spaces; blank lines before the next
  `layer` are ignored; `#` lines are comments. At most 64 cells per axis, 512 KB per file and
  2 MB for the whole folder; a world holds at most 10,000 instances.
- The footprint is centered on the placed column. Rotation turns it counterclockwise seen from
  above (90 turns +x toward −z); by convention the front is the last row (+z).
- Optional `marker:` header lines bring story markers along (see "Story markers"):
  `marker: <spawn|poi|region> x=N z=N [entity=<kind>] [count=N] [radius=N] [name=<rest of the line>]`,
  at cell x, z of the layers (the marker stands on the bottom layer), each followed by optional
  `line: <text>` lines for its dialogue. `prefabs/camp.txt` brings two bandits:
  `marker: spawn x=11 z=12 entity=bandit count=2 name=Camp raiders`. Each placement makes its
  own markers; they belong to the instance (the Editor panel shows them as `(of camp #N)`), turn
  and move with it, and go when it's removed. Instances placed before a file declared markers
  bring none.
- Errors name the file and line (unknown block, character not in the legend, too big...).

## Story markers

Markers are authored places, saved with the world:

| Kind | What it does |
|---|---|
| `player_spawn` | Where you start and get back up after being knocked out (one per world) |
| `spawn` | `count` (1–8) NPCs of one kind that wander within its radius and respawn there; editing it respawns them (a new name or new lines don't) |
| `poi` | A named point, e.g. "Old Well"; its name shows while you're within its radius |
| `region` | A named circle; walking into it shows its name for a few seconds (the innermost wins) |

A world from before markers (or a new one) gets the training dummy, villager, boar and bandit as
four spawn markers, so its NPCs stay where they were and can be moved or removed like any other.
NPC kinds: `dummy`, `villager` (talks), `boar` (Lv 1, fights back), `bandit` (Lv 2, attacks on
sight), `wolf` (Lv 1, fast, attacks; drops Wolf Pelt), `guard` (Lv 4, talks, fights back, iron
helm and sword), `bandit_chief` (Lv 3, attacks, always drops an Iron Helm) and `ghoul` (a Lv 3
barrow ghoul, slow, attacks; may drop a Bone Charm). With the Iron set and a sword you beat the
chief alone, or a pair of wolves or ghouls; a pack of three is too much.

Enemies fight back in more ways (ADR-042, ADR-043). In a pack only two swing at once; the others
circle a few blocks out and wait their turn. Bandits, guards, the chief and ghouls swing strings of
two or three hits, some of them delayed to catch an early roll. Some also answer your attacks:
- **Guards** keep a heater shield up while they come at you; keep hitting the shield to break it.
- **Bandits** may parry a light attack with their knife; they can't parry your heavies.
- **Wolves and bandits** may dodge.
- **The chief** guards with his sword.

An enemy that blocks or parries you hits back quickly. Some step back after a string.

Each weapon swings by its class (ADR-044): **daggers** slash and stab quickly but barely shake a
foe's stance; the **sword** is the all-rounder; **axes** chop hard; **maces** smash, wear down
stance the fastest and strike (ghouls are weak to it). The Iron Hand Axe and Iron Mace have no
drop yet: the debug window's Items tab gives them.

**Criticals** (ADR-045): attack an enemy from behind while it isn't attacking or reeling to
**backstab** it: it's caught, hit for the riposte's damage and knocked down. Enemies that have
seen you turn to face you, so backstabs land on unaware or passive ones and on ones turned away.
Any other hit has a 5% chance to be a **critical hit** for 50% more (a knife: 10%); enemies crit you
too. Critical damage numbers are big, orange and end in "!". Big hits shake the camera (the debug
window's Camera tab turns it down or off).

**Status effects** (ADR-046): some hits build up a status on their target, shown as a bar under its
health. When the bar fills, the status takes effect. **Bleed** (the Rusty Knife, wolves' bites) takes a burst of health,
**poison** (barrow ghouls' claws) drains it for 10 seconds and turns the health bar green, and
**frost** (no source yet) takes a burst and then makes the body take 20% more damage for a while.
The bars drain over time; armor resists statuses (the iron set a lot), and a shield's guard stops
the build-up along with the damage. Status damage numbers come in the status's color.

A spawn marker can carry up to 8 **lines** of dialogue (1–160 bytes each). Villagers and guards say
the next one each time you press E, round and round; anyone who fights (guards too) shouts the
next one when they turn on you.
With lines and a count of 1 the NPC goes by the marker's name, so "Hakon the Red" on a
`bandit_chief` marker is Hakon. Names are up to 48 bytes; a world holds up to 4096 markers, and its
spawn markers make at most 256 NPCs together. Every marker edit is one undo step.

## Debug window

Press `.` to open it. The mouse cursor is freed while it's open; the keyboard still moves the player
unless a text field has focus.

| Tab | What's there |
|---|---|
| Render | View mode (shaded, greedy quads, chunks, ambient occlusion, normals), wireframe quad edges, sky/fog/water/character toggles, frustum culling on/off or frozen, per-frame chunk/quad/draw-call counts |
| Player | The region you're in, health, the five attributes and what derives from them (max health, regen, attack damage and speed, combat state), armor K (damage reduction = armor / (armor + K x attacker level)) with the current armor and its reduction vs level 1 and 2, position (editable), velocity, ground/water state, respawn, walk/run/jump speed, gravity, fly, noclip, fly speed |
| Items | Give any item or the whole Iron armor set, see what's equipped and in the bag slots, empty the inventory |
| Data | The loaded item definitions (save id, id, slot, attack, bag size, armor); Reload defs re-reads data/ and keeps the old ones on an error |
| Entities | Every NPC: health and attributes, traits and temper, aggro/leash/chase numbers, angry or calm (toggle), drop chance and what its corpse holds, position, wander state; hit for 10 or kill (attackable only), make talk, the spawn marker it came from, respawn NPCs at their markers |
| Camera | Field of view, distance (up to 80 for overviews), terrain collision, HUD on/off, mouse sensitivity |
| World | In the endless world, streaming: the window, columns at each stage, what the last frame installed and lit (and how long that took), a worker's time per column, the light budget per frame, the player's column's stage. Time of day, time speed (0 = paused), seed + regenerate, generation timings and quad counts |
| Audio | Mute, volume (all and per bus: effects, ambience, interface), random pitch and volume spread, hearing distance, the voices playing, the ambience's readings (water, trees, fire around) and its loops' volumes, the latest sounds, sounds with no takes, and a button per sound to play it (ADR-037) |
| Save | The player save's path, when it was written and its size, Save / Load / Delete, whether quitting saves (and why not), the last result and what the last load restored or dropped |
| Stats | Frame-time graph of the last 240 frames, average and worst frame |

## Command-line options

| Flag | Meaning |
|---|---|
| `--world PATH` | World file: load it if it exists, otherwise generate a 1024² world from `--seed`; the editor saves here. The player save is the same path with `.flys`. Without it: the endless world (streamed around the player; no editor yet, not saved; the player save is `worlds/world.flys`) |
| `--new-game` | Don't load the player save on start (quitting still writes it) |
| `--seed N` | World seed (default 1337) |
| `--time T` | Start time of day, 0..1 (0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset) |
| `--frames N` | Benchmark: the player walks a circle and the camera orbits; exits after N frames and prints frame-time stats. Vsync is off. Each frame is exactly one 60 Hz tick of game time, so a run is the same at any frame rate. |
| `--screenshot out.bmp` | With `--frames`: save the last frame as a BMP |
| `--look YAW PITCH` | With `--frames`: fixed camera angles in degrees instead of the orbit (yaw 0 looks toward −Z, −90 toward +X; negative pitch looks up) |
| `--fly SPEED` | With `--frames` in the endless world: the player rides east at SPEED blocks per second, 12 above the ground, so the stats include streaming (also printed: its cost per frame and repacks) |
| `--debug` | Start with the debug window open (it's also captured by `--screenshot`); F2 writes debug notes |
| `--port N` | Command server port on 127.0.0.1 (default 47800); see "Shaping the world with Claude" |
| `--no-serve` | No command server (benchmarks never start one) |
| `--view MODE` | Start in a debug view: `shaded`, `quads`, `chunks`, `ao`, `normals`, `light` (light levels), or `wire` (wireframe) |

Example benchmark at night:

```sh
./build/release/flycoos --frames 3000 --seed 42 --time 0.02 --screenshot build/night.bmp
```

## Layout

```
src/world.*      voxel grid, region-based terrain (forest / rocky / plains), trees + rocks, parallelFor, markers, world files
src/edit.*       world edits (fill, raise, lower, smooth, flatten, paint) with undo/redo, voxel raycast
src/prefab.*     prefabs: generated ones, the structure file parser, place/move/remove of instances
src/commands.*   remote commands: JSON request -> edits, queries, top-down map PNG (no sockets, unit-tested)
src/server.*     localhost command server thread (newline-delimited JSON)
src/mesher.*     greedy mesher with ambient occlusion -> 8-byte packed quads
src/player.*     movement physics (+ fly/noclip tuning), camera collision, character models, procedural animation
src/stats.h      the five attributes and the stats derived from them
src/entity.*     NPCs: dummy, villager, boar, bandit, wolf, guard, bandit chief, ghoul (traits, temper, AI, talking, loot), attacks and damage
src/defs.*       data definitions: the two-pass loader of data/*.json
src/save.*       player saves (.flys): player, inventory, NPCs by spawn marker and slot, quests and flags
src/quest.*      quests: the per-tick event drain, stages, triggers and actions, dialogue conditions
src/item.*       items, inventory (equipment, backpack, bags), move/equip rules
src/inventory_ui.*  Character (C), Bags (B), Journal (J) and Progress (K) windows
src/hud.*        in-game overlay: nameplates, speech balloons, damage numbers, talk/loot hint, player health, knocked-out message, place names
src/renderer.*   SDL3 GPU (Metal) renderer: pipelines, uploads, culling, day/night lighting
src/debug_ui.*   Dear ImGui debug window (toggle with .), and the debug note box
src/notes.*      debug notes: the game state a note keeps, debug_notes/notes.jsonl
src/stream.*     the endless world: chunk columns generated on worker threads, lit and meshed around the player
src/editor_ui.*  Editor panel, crosshair and brush preview (F1)
src/audio.*      sound (ADR-037): loading data/sounds, the mixer, what each noise sounds like, ambience
src/main.cpp     window, input, camera, main loop, benchmark mode, runs remote commands
data/            definition files: items.json, creatures.json (ADR-018), weapon_classes.json (ADR-044), quests/*.json (ADR-026); fonts/alagard.ttf (region titles);
                 sounds/*.ogg (ADR-037: <name>_<n>.ogg are one sound's takes)
prefabs/         structure prefab files (cottage, tent, camp, well, fence, ruin)
tools/           flycoos_mcp.py (MCP server bridging Claude Code to the game), test_mcp.py (end-to-end check)
shaders/         world.metal (voxel, water, sky), compiled at runtime
tests/           core_test.cpp: mesher, packing, physics, flight, camera, combat, NPC fights and loot, inventory, generation,
                 world files, edits and undo, remote commands, prefabs and instances, markers and spawns, raycast, data files checks, quests
```

## Credits

- [Alagard](https://www.dafont.com/alagard.font) by Hewett Tsoi (free, credit given): the region titles' font.
- Sounds: public domain (CC0) and CC-BY recordings from Kenney, OpenGameArt and Freesound; every source is in
  [CREDITS.md](CREDITS.md).
