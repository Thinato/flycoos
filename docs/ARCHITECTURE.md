# Architecture

Milestone 0 is about 1,200 lines of C++ (including tests) and 160 lines of MSL. It has two layers:

- **`flycoos_core`** (static library: `world`, `story`, `mesher`, `model`, `player`, `entity`, `item`, `defs`, `edit`, `prefab`, `commands`, `session`, `save`, `quest`, `arena`): pure CPU
  code with no GPU or SDL dependency, unit-tested by `tests/core_test.cpp`.
- **`flycoos`** (executable: `main`, `renderer` + `shaders/world.metal`, `server`): platform, input,
  camera, GPU and the command server.
- **`tools/flycoos_mcp.py`**: the MCP server Claude Code starts; it forwards tool calls to the game.

```
main.cpp ──► loadWorld(--world) or generateTerrain() ──► Grid (1024×192×1024 bytes)
   │                                                     │
   │                                          Renderer::uploadWorld()
   │                                             └─ meshChunks() (core: parallelFor(chunks) ─► meshRegion()) ─► packed quads ─► GPU storage buffers
   │
   ├─ loop: events ─► SimInput ─► step() × N (fixed 60 Hz) ─► poseModel(alpha) ─► camera ─► Renderer::render()
   │
   ├─ editor (F1): raycast() ─► applyEdit() / undoEdit() / redoEdit() ─► dirty Box ─► Renderer::updateWorld()
   │                saveWorld() on Ctrl+S
   │
   ├─ player save: loadPlayer() on start (unless --new-game) and F9; savePlayer() on F5 and quit ─► worlds/<world>.flys
   │
   └─ CommandServer::take() ─► runCommand() ─► same edits/undo ─► updateWorld(); save, snapshot, goto
            ▲ socket thread, 127.0.0.1:47800 ◄── tools/flycoos_mcp.py (stdio MCP) ◄── Claude Code
```

## Modules

| File | Responsibility |
|---|---|
| `src/world.h/.cpp` | `Block` ids, `Grid` (dense voxel volume in world coordinates, x fastest, out-of-bounds = Air; fixed at `lo`, or `Grid::streamed`: a window onto the endless world whose storage wraps in x and z, with chunk slots `chunkSlot`/`chunkOrigin`), `parallelFor`, region-based terrain (`Terrain`, a pure function of the column), `generateChunkColumn` (one 32-wide column of the world of a seed, built in a padded scratch grid: the same in any order) and `generateTerrain` (every column of a grid), `growRock`, `plantTrees` (the generator's tree sites, chosen from the ground alone), `findSpawn`, `Instance`, the world file container (`writeWorldFile`/`readWorldFile`: header, `VOXL`, `INST`/`INS2`, checksum; other sections pass through as `Section`s), `fnv1a` and `replaceFile` (temp and rename; durable for player saves) shared with `save.cpp` |
| `src/plants.h/.cpp` | Grass and flowers (ADR-030): `PlantModel`s and their voxel grids (`buildPlant`, near and far; `plantQuads` meshed without ground faces), `plantHash`/`plantModel` (the model a cell draws), `meadowPlant` (the meadow's noise) and `plantColumn` (shared by the generator, `Tool::Plant` and `growMeadow`) |
| `src/trees.h/.cpp` | Tree species (ADR-029): `planTree` (a `TreeShape` of bark capsules and leaf clumps), `planBush`, `shapeBox`, `growShape` (draws it, leaf shades by height in the crown); `kFullGrown`, `kSpeciesPrefab`, `TreeSite` |
| `src/story.h/.cpp` | Story data: `EntityKind` save ids (and the string ids of the ones code names), `Marker` kinds and caps (`checkMarker`, `takesLines`, `regionAt`), their file sections (`MARK`, `LINE`, `OWNR`, `TIME`), `activeAt` and `saveWorld`/`loadWorld` (the container plus markers) |
| `src/edit.h/.cpp` | World edits: `EditOp` → `applyEdit` → dirty `Box` + undo record; `undoEdit`/`redoEdit` (voxels, prefab instances and markers); `putMarker`/`removeMarker`; `raycast` (voxel DDA); `groundTop` |
| `src/prefab.h/.cpp` | Prefabs: the generated ones, the structure file parser and loader (`prefabs/*.txt`), `placePrefab`/`removePrefab`/`movePrefab` of instances, `replantTrees` |
| `src/arena.h/.cpp` | The combat test arena (ADR-054): `buildArena` draws it around `worldMiddle` (world.h: a world file's centre, the endless world's origin) into any grid, places its prefabs as instances and writes its markers; `flycoos --build-arena PATH` saves it (`worlds/arena.flyw`). Group spawns are `spawnGroup` (entity.h), at points from the middle in the debug window |
| `src/commands.h/.cpp` | Remote commands: JSON request → `EditOp`s (one undo step), `path`, `heights`, `inspect`, `map` (PNG), `catalog` (NPC kinds and materials for the MCP bridge); returns what needs the game (remesh box, save, screenshot camera, goto) as data |
| `src/server.h/.cpp` | `CommandServer`: localhost TCP thread, newline-delimited JSON, one-slot mailbox to the main thread |
| `src/light.h/.cpp` | Voxel light (ADR-019): sky and block light nibbles per voxel in lazily allocated 32³ chunk arrays (`Light`, wrapping like its grid), `fillLight` (whole world), `relight` (the columns around an edit; returns the box whose light changed), `lightColumns`/`clearColumn` (streaming), `lightCost`/`emission` per block |
| `src/melee.h/.cpp` | Melee (ADR-035): `MoveSpec` (windup, hold point, impact, chain and cancel points, damage, stance, stamina, lunge), the weapon classes' movesets (`WeaponClassDef`, `weaponClass`, `spec`; loaded from data/weapon_classes.json, ADR-044), `MeleeTuning gMelee` (stamina, the guard, timings; the debug Player tab edits it), `updateMelee` (the player's buttons each tick: tap or hold, the input buffer, the chain, charging, the guard, stamina; it rewrites the body's `Input`), `startMove`/`advanceMove`/`interrupt`, `invulnerable`, `hyperarmor`, `guards`, `playerPoise`; shields and the parry (ADR-036): `shieldOf`, `guardOf`, `guardAgainst`, `parries`, `bounces`; `parryOf` (a knife or fists parry too) and `perfectGuard` (ADR-041); `damageOf` (ADR-040) |
| `src/projectile.h/.cpp` | Projectiles (ADR-050): `Projectile` (an arrow: tip, velocity, the bow, power, stance, stuck), `RangedTuning gRanged` (gravity, speeds by draw, the draw time, expiry, pickup, the aim walk; the debug Player tab edits it), `drawOf`, `shotOrigin`, `shotVelocity` (straight at a point, lobbed onto it, or along the facing), `segmentBox`, `hitBox`, `loose` (the release: an arrow from the quiver, or a crossbow's loaded bolt at `boltSpeed`, ADR-052; an archer's, ADR-051: at the player along its facing, off by its aim error), throwables (ADR-053: `fillPouch`, `cycleThrowable`, `hurl` at a throw's release, `burst` and the `Burst` puffs in `Session::bursts`) and `updateProjectiles` (gravity, a swept voxel ray and body boxes, hits through `strike()`, or for an archer's arrow (`Projectile::owner`) only the player's box, through `strikePlayer()`; a bomb or pot bursting on the first thing it meets; sticking, pickup into the quiver or the pouch, expiry) |
| `src/audio.h/.cpp` | Sound (ADR-037): `Audio` (loads `data/sounds/*.ogg` with stb_vorbis into groups of takes; `play` picks a take never twice running with a little random pitch and volume; `mix` is the device thread's stereo mix: up to 48 resampled voices, three buses, a soft limiter), `place` (falloff and pan from the listener), `layersOf`/`hear` (what a `Sound` from the sim plays), `updateAmbience` (looping beds faded by what's around). No device here: main.cpp's `Speaker` feeds `mix` to SDL3's audio stream |
| `src/fx.h/.cpp` | Hit feedback (ADR-058), presentation only: `Fx` (particles, smears, its own RNG, `FxSettings`), `spawnFx` (the burst a `Sound` makes, by noise and creature), `trailFx` (a body's blade edge each frame, `bladeEdge` in model.h), `swingsBlade`, `updateFx` (flight, landing, ageing, the cap, the smear ribbon's triangles), `drawnSize` |
| `src/lockon.h/.cpp` | Lock-on (ADR-033): `pickTarget` (nearest the middle of the view, in range and sight; never a villager, ADR-056), `switchTarget` (the next one that way on screen), `updateLock` (lets go past the break range or out of sight, passes on after a kill), `lockPoint`. main.cpp turns the camera to it and sets `Input::face`; the HUD draws the dot |
| `src/stream.h/.cpp` | `Stream` (ADR-031): keeps a streamed grid centred on the player. Worker threads generate the chunk columns coming in (nearest first); each frame `update` installs them, lights those whose 8 neighbours are in (within a time budget) and hands out the chunk slots to mesh once their 8 neighbours are lit, and to empty for the columns that left. `follow` moves the window |
| `src/mesher.h/.cpp` | Greedy mesher with per-corner AO and per-face light (`meshRegion`; plants become `PlantInstance`s, not faces), `Quad` packing/unpacking, the world's chunk meshes (`meshChunks`, and `remeshChunks` after an edit) that the renderer uploads |
| `src/player.h/.cpp` | `Player` = `Body` (position, velocity, movement tuning, animation state, `teleport`, `drawPos`/`drawYaw`) + `Combatant` (base stats, level, health, swing and damage timers, `Inventory`); `Progress`; `collect`/`sheet` (its modifiers and final stats, optionally with a weapon skill's conditional modifiers), `skillOf`; walking-body physics and collision (`updatePlayer`, player and NPCs), `cameraClearance` |
| `src/stats.h` | Header only: the five attributes (`Stats`), the stat pipeline (`Stat`, `Modifier`, `StatSheet`, `compute`; ADR-022), the derived-stat, damage and armor formulas and their constants, `Attack`/`kPunch`; `Skill` and the progression formulas (skill XP per use and per level, character XP and level costs, default kill XP; ADR-027) |
| `src/progress.h/.cpp` | Progression (ADR-027): `SkillDef`/`TalentDef` (filled by the loader), `Progress` (character XP, skill levels, talent ranks), `addSkillXp`/`addCharacterXp`, `canLearn`/`learn`/`respec`, `talentPoints`, `progressModifiers` (what `collect` adds) |
| `src/model.h/.cpp` | Character models: `Model`/`Part` ids, the voxel model catalog (`buildPart`), held and worn models (`heldModel` by weapon class, `partModels`), procedural animation (`poseModel`; swing poses per weapon class, ADR-044). Core (no GPU), since NPC traits name their `Model` |
| `src/entity.h/.cpp` | NPC entities: per-kind traits, temper and AI numbers (`EntityType`, `typeOf` reads the loaded defs), spawning (`syncSpawns` from spawn markers, `defaultMarkers`), lasting deaths (`DeadSlots`, `buryDead`; ADR-056), the AI state machine (`AiState`, `setState`, `think`; ADR-020), attack tokens and strings of hits (ADR-042), answers to the player's attacks (`answer`: guard, parry, dodge; ADR-043) and their knobs (`AiTuning gAi`, the debug Entities tab), talking, damage both ways (`resolveHit`), the one XP award (`awardHitXp`), loot |
| `src/session.h/.cpp` | `Session`: the simulation state (world, instances, markers, player, entities, the damage-roll RNG seeded from the world seed, time of day, the region the player is in, the tick's `GameEvent`s, quest states, flags, the event log and quest toasts); `startSession` (starting gear, spawns, quests reset), `step(Session&, SimInput, dt)` (one `kTick`: NPC quest lines, interaction, swing, movement, NPCs and combat, time of day, region enter, item count changes, the dead recorded and far corpses buried (`buryDead`), the quest drain, the knocked-out player's respawn, skill and level-up toasts) |
| `src/save.h/.cpp` | Player saves (`.flys`, ADR-025): `savePlayer`/`loadPlayer` (`HEAD`, `PLYR`, `INVT`, `NPCS`, `QUST`, `FLAG`, `PROG`, `WEAR`, `REST`, `GRIP`, `ARRW`, `THRW`, `DEAD`; NPCs keyed by spawn marker id and slot, quests by string id), `savePathFor`, `worldFileChecksum`, `SaveReport` (what a load restored and dropped) |
| `src/quest.h/.cpp` | Quests (ADR-026): `GameEvent` (`Killed`, `ItemCountChanged`, `TalkedTo`, `RegionEntered`), the quest data types the loader fills (`QuestDef`, `Stage`, `Trigger`, `Action`, `Condition`, `QuestLine`), `QuestState`; `updateQuests` (the once-per-tick drain, then failing quests whose needs died), `setQuestStage`/`resetQuest(s)`, the condition evaluator (`passes`, `passesAll`, `pickLine`), `npcDead`/`deadNeed` (ADR-056), `countItem`, `checkQuestMarkers` |
| `src/defs.h/.cpp` | Data definitions (ADR-018): `Defs` registry, the two-pass `loadDefs` over `data/*.json` and `data/quests/*.json` (records by string id, `copy_from`, field and range checks, quest references resolved to handles, warnings), `dataDir`, the current registry (`defs`/`setDefs`/`reloadDefs`) |
| `src/item.h/.cpp` | `ItemId` save ids, `itemType` (reads the loaded defs), `Inventory` (equipment, bag bar, item slots), moving/equipping rules, the main-hand attack |
| `src/inventory_ui.h/.cpp` | In-game Character (C), Bags (B), Journal (J) and Progress (K) windows: slots, drag and drop, right-click equip, tooltips, attributes; quests with the current entry and checked objectives; level, skills and the talent trees |
| `src/hud.h/.cpp` | In-game overlay on ImGui's background draw list: nameplates, speech balloons, damage numbers, talk/loot hint, player health, knocked-out message, poi labels and the region name (`drawPlaces`, in Alagard, `titleFont`, which DebugUI loads from `data/fonts/`) |
| `src/renderer.h/.cpp` | SDL3 GPU device, pipelines, the world's `Light` (filled by `uploadWorld`, relit by `updateWorld`), uploads of the core's chunk and model meshes, instanced plants (each frame's near ones, per model), hit effects (instanced particle cubes and the smear ribbon, ADR-058; `stream` uploads per-frame buffers), render targets, frustum culling, day/night lighting, screenshots |
| `src/text.h` | `validName`: UTF-8 display text without control, bidi or zero-width characters (marker names and lines, item names) |
| `src/render_stats.h` | `RenderStats` (quad, chunk and draw counts, mesh, light and remesh times, light memory), for the debug and editor UIs without `renderer.h` |
| `src/editor_ui.h/.cpp` | Editor state (`Editor`), the Editor panel, crosshair and brush preview (ring on the ground, or Fill's box), marker pins and region circles |
| `src/debug_ui.h/.cpp` | Dear ImGui debug window (`.` toggles): Render, Player, Progression, Items, Entities, Quests, Data, Camera, World, Save, Audio, Effects (ADR-058), Stats tabs. Owns the ImGui frame (`begin`/`end`; the HUD draws in between), `GameSettings` and `DebugActions` |
| `src/notes.h/.cpp` | Debug notes (F2 with `--debug`): `noteContext` turns the session into JSON (world, clock, player place/motion/combat/stats/gear, camera, region, nearby NPCs with AI history, quests, flags, latest events); `saveNote` appends it with the text, its id (line number) and `"status": "open"` to `debug_notes/notes.jsonl` and writes the screenshot next to it. main.cpp opens the box (`noteWindow`, debug_ui), stops stepping the simulation while it's open and takes the state and an offscreen snapshot the frame it opens |
| `src/main.cpp` | CLI options, world load/save, window, input routing (game vs. debug UI vs. editor) into `SimInput`, orbit camera, main loop around `step`, remote commands, benchmark stats |
| `shaders/world.metal` | `voxel_vs` (vertex pulling), `voxel_fs`, `water_fs`, `sky_vs`, `sky_fs`, `plant_vs`/`plant_fs`, `particle_vs`/`particle_fs` and `ribbon_vs`/`ribbon_fs` (hit effects, ADR-058) |

## World

- `Grid` stores one `Block` byte per voxel: 1024×192×1024 = 192 MiB. There is no per-chunk storage.
  Chunks (32³) exist only as meshing and draw units.
- **Blocks** (`Block` in `world.h`): the id is the palette index and the saved byte, so new blocks
  only append. The renderer holds 128 (7 bits in the shader's flat output, ADR-019, widened in ADR-029); 83 are used.

  | Ids | Blocks | Used by |
  |---|---|---|
  | 0–8 | Air, Grass, Dirt, Stone, Sand, Snow, Wood, Leaves, Water | terrain (Water is `kWater` in the shader) |
  | 9–19 | Skin, Tunic, Belt, Boots, Hair, Eye, Rock, Robe, DarkHair, Straw, Burlap | models (Rock: boulders) |
  | 20–27 | Steel, Iron, Hide, Tusk, Snout, Hood, Leather, Plate | items, boar, bandit, armor |
  | 28–31 | Planks, Cobble, Thatch, Ember | prefabs; Ember emits light 13 |
  | 32–38 | Brick, MossyCobble, DarkPlanks, Hay, Clay, Slate, Lantern | building (fill, prefabs); Lantern emits 11 |
- **Regions.** Two very low-frequency noise fields (freq 0.002, domain-warped by up to 100 blocks for
  organic borders) give each column a *forest* and a *rocky* strength from 0 to 1. Rocky land
  suppresses forest, so the two stay mostly separate. Where both are near 0 the land is **open
  plains**. Regions are a few hundred blocks across; that size is what makes the world feel vast.
- **Terrain** (`generateTerrain`, parallel over z-rows):
  - Hill height scales with region: about 8 blocks on plains, 18 in forests, up to 58 on rocky land.
    The hill noise is squared, so hills rise out of flat ground instead of rippling.
  - Rare mountain ranges add up to 80 blocks.
  - Water (ADR-032): seas where a low-frequency, warped coast noise sinks the land below `kSeaLevel`
    (34) along wide coasts; rivers along a meandering noise's zero line, their beds just under the
    sea and their valleys widening gently, waning into the hills; lakes, at most one per 224-block
    cell, in hollows at their own levels (the lowest ground around them), each a stretched, wobbly
    bowl whose shore the ground eases toward, with a rim that holds the water in.
  - Surface block from height, slope and noise: sand on gentle shores (wider by the sea than by
    lakes), sand or silt under water, stone on cliffs, on broad bare patches of rocky land and up
    high, snow on the high flats. The stone and snow lines wander with a low-frequency noise and
    are mottled at two smaller scales, so they come in patches over ~20 blocks of height. Below
    the surface come 3 blocks of subsoil, then stone.
  - Heights are capped at `size.y - 30` to leave headroom. `generateChunkColumn` computes the
    column's shapes once for its padded area (`Terrain::cache`); anything else is computed on demand,
    the same.
- **Decoration**, run sequentially (the shapes cross column boundaries), seeded per cell so it is
  deterministic:
  - **Trees:** one candidate per 7×7 cell, with probability 75% × forest strength plus 0.4%, so the
    plains get a lone tree now and then. Trunks are 8–14 blocks (2×2 from 12 up), with a flattened,
    ragged canopy of radius ~4–5.
  - **Rocks** (`Rock` block): one candidate per 14×14 cell, with probability 40% × rocky strength plus
    0.6%. On rocky land most are tapering spires 10–23 blocks tall; the rest are half-buried boulders.
- The world is deterministic per seed (tested). `core_test` also checks that forests and rocks exist
  and that a 96×96-column plain with no trees or rocks exists. Generation takes ~0.15 s on an M4.

## World files (`saveWorld` / `loadWorld`)

One file per world, e.g. `worlds/story.flyw`, little-endian:

```
"FLYW" | u32 version (1) | i32 size x, y, z | u32 seed
sections: char tag[4] | u64 bytes | payload      (unknown tags are skipped)
u32 FNV-1a checksum of every byte before it
```

- The `VOXL` section holds every 32³ chunk (in `chunkOrigin` order: x, then y, then z), each as
  `u32 bytes` + runs of `(u8 block, LEB128 count − 1)` over its voxels, x fastest, then z, then y.
  Terrain is mostly horizontal layers, so whole layers of stone or air become single runs: the
  1024×192×1024 world is ~2.8 MB (vs 192 MiB in memory). Chunks encode and decode in parallel.
- The `INST` section (written only when there are prefab instances) holds them sorted by id: per
  instance `u32 id | u8 name length | name | i32 x, y, z | u8 rotation | u32 seed | u8 param count |
  f32 params | u32 cell count | cells (u16 x, y, z | u8 before | u8 after)`. A file without it
  loads with no instances; older builds skip it. Builds since ADR-029 read `INST` but write `INS2`:
  the same, except a `u8 form` after the params. Form 0 is the `INST` cell list. Form 1 run-length
  codes the cells over their box (`u16 lo x, y, z | u8 extent − 1 x, y, z | runs of (u8 before,
  u8 after, LEB128 count − 1)`, x fastest, then z, then y; before = after marks a voxel that isn't
  a cell). Prefab placements record cells in that order, so a big tree's thousands of cells take a
  few kB. Anything else (e.g. an instance moved by hand) keeps form 0.
- The `MARK` section (always written; empty when there are no markers) holds the markers sorted by
  id: `u32 count`, then per marker `u32 id | u8 kind | u8 name length | name | f32 x, y, z | f32
  radius | u8 entity | u8 count`. A file without it is older: it loads with `markers` = nullopt.
- The `LINE` section (written after `MARK`, only when a marker has dialogue lines) holds them by
  ascending marker id: `u32 count`, then per marker `u32 id | u8 line count | lines (u8 length |
  text)`. Each marker must exist and pass `checkMarker` with its lines; a second `LINE` section is
  rejected. A file without it (all
  older ones) loads with no lines; older builds skip it and drop the lines.
- The `OWNR` section (written after `MARK`, only when a prefab instance owns a marker, ADR-021)
  holds by ascending marker id `u32 count`, then per owned marker `u32 marker id | u32 instance
  id`, only for owners that are live instances. Each must name a loaded marker (not the
  player_spawn) and a loaded instance; a second section is rejected. A file
  without it (all older ones) loads with every marker hand-placed; older builds skip it, so their
  saves keep the markers but drop the ownership.
- The `MKID` section (ADR-025; only when a removed marker had a higher id than every live one)
  holds `u32` the highest marker id ever used (`EditHistory::markerIds`), so new markers never
  reuse a removed one's id: player saves key NPCs by marker id. A file without it falls back to
  the highest live id; a second one, a bad size or 2³²−1 is rejected.
- The `TIME` section (written after `MARK`, only when a spawn marker has an active window,
  ADR-023) holds by ascending marker id `u32 count`, then per such marker `u32 id | f32 activeFrom |
  f32 activeTo`. Each must name a loaded spawn marker and a real window (from ≠ to) that passes
  `checkMarker`; a second section is rejected. A file without it (all older ones) loads with every
  spawn always active; older builds skip it, so their saves drop the windows.
- Later content gets its own sections; old readers skip them, so adding one needs no version bump.
- Loading rejects, with a message, anything that doesn't parse exactly: wrong magic or version,
  checksum mismatch (any corruption or truncation), bad sizes, block ids or run lengths. A
  `--world` file that exists but fails to load stops the game rather than generating a world that a
  save would then write over it.
- Saving writes `PATH.tmp` and renames it over `PATH`, so a failed save never destroys the last
  good file.
- Real world (seed 1337): save 22 ms, load 13 ms, 2.78 MB.

## Player saves (`savePlayer` / `loadPlayer`, ADR-025)

The world file is the authored base; only the editor writes it. What play changes goes into a
player save next to it, `worlds/story.flyw` → `worlds/story.flys` (git-ignored), little-endian:

```
"FLYS" | u32 version (1)
sections: char tag[4] | u64 bytes | payload      (unknown tags are skipped, missing ones defaulted)
u32 FNV-1a checksum of every byte before it
```

- `HEAD` (required): `u8 length | world file name | u32 world checksum | u32 seed | f64 simulated
  seconds | f32 time of day`. The world checksum is the one the `.flyw` ends with
  (`worldFileChecksum`), so it hashes all of the world's content.
- `PLYR`: `f32 x, y, z, yaw | f32 health | f32 seconds knocked out | u32 level | u32 stamina,
  strength, agility, intellect, spirit`.
- `INVT`: `u8 count` + item save ids of the equip slots (by `EquipSlot`), the same for the bag bar,
  then `u8 container count`, each `u8 slot count` + item save ids (container 0 is the backpack).
- `NPCS`: `u32 count`, then per NPC `u32 spawn marker id | u8 slot | u8 kind | f32 health | f32
  seconds dead | u8 loot | f32 x, y, z, yaw` (31 bytes). The key is (marker id, slot), where
  `Entity::slot` is the NPC's index among its marker's: never the vector index.
- Loading parses and bound-checks everything before it touches the session (file ≤ 1 MiB, every
  count within its cap, known item and creature ids, finite numbers, data-file ranges for level
  and attributes, no duplicate sections or NPC keys). On any failure it tries `PATH.bak`, and if
  that fails too the session is untouched. Unknown item and creature ids are dropped with a note,
  not rejected. A good file sets the time of day (spawns may depend on it), resets the session like
  `startSession` (keeping the movement tuning, so the editor's flight stays on), then applies each
  section.
- A save from another version of the world (checksum or seed differ) loads what still matches:
  NPC entries whose marker is gone, now spawns another kind or fewer NPCs are dropped; an NPC whose
  home moved away starts at home; the player and NPCs are clamped into the world and lifted out of terrain;
  items that no longer fit their slot move to a free one (or are lost). Each case adds a line to
  `SaveReport::notes`: the log prints them, the debug Save tab lists them.
- The AI state isn't saved: living NPCs start calm, dead ones lie in Dead. `DEAD` (ADR-056) keeps
  which marker slots died for good: those whose corpse has gone are left out again after the load.
- Writing builds the file in memory and calls `replaceFile(path, bytes, durable)`: `PATH.tmp`,
  `F_FULLFSYNC`, the old save renamed to `PATH.bak`, the temp renamed over `PATH`, the folder
  synced (best effort). The story world's save is ~1 KB and takes ~5 ms.
- When: F5 and F9, quitting (any way: Esc, the window, SIGTERM), and on start if a save exists
  (`--new-game` skips it). Benchmarks never read or write one. The MCP `save` writes the world only.
  Quitting skips the save after a failed load (F5 too, until the Save tab overwrites or deletes
  the file), after Delete (until the next F5), and in a session that opened the editor.
- `QUST` (ADR-026): `u32 count`, then per quest `u8 length | quest id | u32 stage | u8 bits (1 done,
  2 failed) | u8 objective count`, each `u8 length | objective id | u8 ObjectiveState` (hidden,
  shown, completed, failed). Keyed by string id, so reordering or adding quests and objectives in
  the data scrambles nothing; a quest or objective the data no longer has is dropped with a note,
  a stage it no longer has is kept with a note. `FLAG`: `u32 count` (≤ 4096), then per flag `u8
  length | name (1–48 bytes, checked like names) | i32 value`. Duplicate keys, unknown bits or states
  and stages above 65535 reject the file.
- `PROG` (ADR-027): `u32 character XP` toward the next level (the level is `PLYR`'s) | `u8 skill
  count`, each `u8 length | skill id | u32 level (1–100) | f32 XP toward the next (0–10⁹)` | `u16
  talent count`, each `u8 length | talent id | u8 rank (≥ 1)`. By string id: an unknown skill or
  talent is dropped with a note, a rank above the talent's max lowered with a note, and more points
  spent than `level − 1` resets the talents with a note. Duplicate ids, rank 0 and out-of-range
  levels or XP reject the file. XP at or past the next level's cost is lowered with a note, and the
  ranks are replayed through `canLearn` tier by tier (any failure resets them with a note).
- `WEAR` (ADR-027): `u32 count`, then per NPC with wear `u32 spawn marker id | u8 slot | f32 the
  player's hits on it | f32 its hits on the player` (`Entity::xpWear`, `armorWear`; 0–10⁶). Keyed
  like `NPCS` but apart from it, so `NPCS` keeps its layout; entries without their NPC are dropped.
- `REST` (ADR-047): `u8 flask charges left` (capped by the Flasks stat on load) | `u8 1 if rested`, then
  `f32 x, y, z` of the rest point (`Session::restPoint`). Missing (older saves): full flasks, never rested.
- `GRIP` (ADR-048): `u8 1 if two-handed` (`Combatant::twoHanded`; anything else is rejected). Missing
  (older saves): one-handed.
- `ARRW` (ADR-050): `u8 arrows in the quiver` (`Combatant::arrows`, at most `kQuiver`; more is
  rejected). Missing (older saves): a full quiver. Projectiles in flight or stuck aren't saved.
- Every new runtime state gets a section here and a round-trip test.

## Editing (`applyEdit`)

`EditOp` is plain data (tool, box corners, column center, radius, strength, height, block), so the
in-game editor and remote commands share one entry point:

1. Validate: unknown tool or block, radius > 32, strength outside 1..64, Flatten height outside the
   world, Paint with a non-solid block, or a Fill box over 128³ voxels (requested size, before
   clipping) are rejected with an error string and change nothing. The caps keep one op to a few
   milliseconds.
2. Snapshot the op's reach (the Fill box, or the brush's columns at full height) clipped to the world.
3. Apply. Brushes work per column on its ground top, the highest opaque voxel, weighted by a
   falloff that is 1 at the center. Raise/Lower add/remove `strength × falloff` blocks, with a
   stable per-column dither so repeated strokes build slopes instead of terraces. Raised ground
   gets the column's surface block on top and its subsoil (grass → dirt, snow → stone) below;
   lowered ground gets the surface block on its new top where that is natural ground (Grass, Dirt,
   Stone, Sand, Snow, Rock), so cutting into a canopy leaves leaves. The surface block is the top
   voxel, except that lowering a column topped by a tree or structure (Wood, Leaves, Planks,
   Thatch...) takes that of the highest natural voxel under it: flattening a canopy down to or below
   the ground leaves grass, not leaves. A top that lies right on natural ground counts as paint (a
   Cobble road) and stays the surface. Smooth moves toward the 5×5 average
   height, Flatten toward a height, both by at most `strength`. Paint sets the top voxel.
4. Diff the snapshot against the grid. The bounding box of the changes is the **dirty box**
   (empty means nothing changed: no undo record). The undo record is the snapshot cropped to it.
5. Push the record (at most 256 records and 256 MB of cells; the oldest drop off, the newest always
   stays) and clear the redo stack. With `join`, the
   snapshot is added as one more part of the last record instead: a remote command's ops (`apply`,
   `path`) form one undo step.

Undo and redo swap a record's cells with the grid's, part by part (the last part first when
undoing), and move it to the other stack, so both directions restore bit-exact. Trees and rocks are just voxels: brushes treat them as terrain (raising
under a canopy raises the canopy). Water is never created or drained: lowering a column whose top is
under water fills the removed cells with water, lowering dry land leaves a dry pit even below sea
level, and raising pushes ground up through water.

`raycast` is an Amanatides–Woo DDA through the grid that returns the first opaque voxel and the
face it entered through (water is passed through). The editor casts it from the camera through
the screen center.

## Prefabs and instances

- **Generated prefabs** (`tree` = oak, `pine` = conifer, `bush`, `boulder`, `spire`, then
  `maple`, `birch`, `araucaria`) grow with the same code as `generateTerrain`: trees through
  `planTree`/`growShape` (src/trees.cpp, ADR-029; `prefabBox` is the shape's `shapeBox`), rocks
  through `growRock`. They work on the live grid, so their rules depend on it: trunks grow down to
  the first solid voxel (≤ 3 below the ground), leaves only fill Air, rocks overwrite. Shapes are
  seeded by the instance's seed in coordinates relative to its anchor, so a moved tree keeps its
  shape. They ignore rotation.
- **Replant** (`replantTrees`, the `replant` command) turns a world's old baked trees into today's
  species as instances, in one undo step (ADR-029).
- **Structures** are text files (format in the README) parsed strictly into cells (Air, a block,
  or `kKeep`), at most 64³, with line-numbered errors. Placing writes every non-keep cell at
  `ground + 1 + y`, rotated by quarter turns around the footprint center, then fills
  `foundation` down under the bottom layer's solid cells to the first solid voxel (≤ 32).
  `loadPrefabs` reads regular `.txt` files directly in `prefabs/` (no symlinks or subfolders,
  ≤ 256 files of ≤ 512 KB, ≤ 2 MB in all, so a rescan stays a few ms) from the working folder,
  like `worlds/`; bad files become error lines.
- **Instances.** `placePrefab` snapshots `prefabBox` (everything the prefab may write, from its
  params or size), stamps, and diffs: the changed voxels, with their old and new values, are the
  instance's `cells` (8 bytes each). `removePrefab` puts back `before` only where the voxel still
  holds `after`, so sculpting on top survives. `movePrefab` is remove + place with the same id in
  one undo step; it builds its record in a scratch history, so a failure (the new spot would change
  nothing) leaves the voxels and the real history untouched. Ids grow from the highest id in the
  list and the history, so undo can't make two instances share one.
- **Caps** (placing and loading): 10,000 instances, 8M cells in all (64 MB), 8 params, and one
  instance's cells span at most 96 per axis (a 64³ structure plus its foundation). Loading also
  rejects id 0 or 2³²−1; placing fails when the ids run out. Loaded params aren't checked against
  the prefab (the loader doesn't know it); `move` and the panel check them before using them.
- **Overlapping instances** are allowed but not tracked: removing the lower one first restores its
  `before` values only where the upper one didn't overwrite them, and removing the upper one later
  puts back the lower one's voxels, which then belong to no instance (orphaned voxels).
  `instanceAt` picks the lowest id. Remove overlapping instances newest first to avoid that.
- **Undo.** An `EditRecord` carries, besides its voxel parts, `(id, instance on the other side)`
  pairs. Undo/redo swap them with the live list (sorted by id) the way parts swap cells, so voxels
  and the list always move together; a record's bytes count the instances' cells toward the
  256 MB cap. The returned box covers the instances' boxes too, so a remove whose voxels were all
  sculpted over still marks the world unsaved.
- **Markers** (ADR-021): a structure file's `marker:` lines (with `line:` lines for dialogue)
  declare markers at a cell of the layers (a spawn's window as `active=FROM-TO`, ADR-023), each checked by `checkMarker` (but its position) at its
  own line, with the marker and NPC caps; the other header keys may appear once. Placing with a marker list makes them, standing on the
  bottom layer (feet at `ground + 2 + y`) and turned with the footprint, with fresh ids and
  `owner` = the instance id, in the same undo step; it fails (changing nothing) past the marker
  caps. `movePrefab` carries the owned markers rigidly (each keeps its spot on the footprint, even
  one moved by hand; clamped into the world) and `removePrefab` takes them out, both in the same
  step. Hand-placed markers (owner 0) are never touched, and `putMarker` never changes an owner.
  Loading a world doesn't consult the prefab files, so instances placed before a file declared
  markers (the story's camp) bring none; only new placements do.
- **Editor:** tool 7 places `editor.place` on the aimed voxel or selects the instance owning it.
  `instanceAt` scans every instance cell, so the result is cached until the aimed voxel changes or
  the world is edited (`editor.edits`). The panel's instance list draws only its visible rows.

## Markers

Story markers say where things are and what places are called (ADR-015). A `Marker` is an id, a
kind, a name, a position (feet: x, z at a column center, y the top of the ground voxel), a radius,
and for spawns an entity kind, a count, dialogue lines and an active window. Kinds:

| Kind | Radius | Effect |
|---|---|---|
| `player_spawn` (at most one) | – | The player starts and gets back up here (`playerSpawn`); without one, `findSpawn` as before |
| `spawn` | wander area | `count` (1–8) NPCs of `entity` (see "Entities"); they wander within the radius of their home by the marker; one that dies stays dead (ADR-056) |
| `poi` | label range | Its name floats above it while the player is within the radius (fading in over the outer quarter) |
| `region` | the circle | Walking into it shows its name for 4 s; `regionAt` picks the smallest containing circle, so nested regions work |

- **Validation** (`checkMarker`, used by commands, the editor and the loader): known kind and
  entity, count 1–8, name ≤ 48 bytes of UTF-8 without control, bidi-override or zero-width
  characters (required for poi and region), finite position inside the world (y up to the world
  height), radius 0–512, and up to 8 lines of 1–160 bytes (checked like names) only on spawn
  markers, not the dummy's (`takesLines`; deliberately not tied to NPC traits, so saved worlds keep
  loading when a kind's traits change), and an active window only on spawn markers: two times of day
  in [0, 1), different unless both 0 (always). The list holds at most 4096 markers, one player spawn, and spawn counts
  adding up to at most 256 NPCs (every NPC is simulated each frame); ids are never 0 or 2³²−1 and
  don't wrap.
- **Edits** (`putMarker`, `removeMarker`) are one undo step each: an `EditRecord` carries `(id,
  marker on the other side)` pairs like it does for instances. The returned box is the voxel at the
  marker (on undo/redo: the side that ends up live), so the game marks the world unsaved and
  remeshes one voxel's chunks (~1 ms) without a special case; a far move doesn't remesh what lies
  between.
- **Spawns follow the markers.** Every NPC remembers the spawn marker it came from as it was then
  (`Entity::spawn`). `syncSpawns` runs after every edit, undo, redo and command (`edited` in
  main.cpp): a marker whose NPCs are missing, too many, or out of date (position, radius, entity,
  count) gets them all respawned, but never a slot whose NPC died (`Session::dead`, ADR-056), and a corpse still holding loot stays; NPCs of a removed marker go; the rest keep their state. The first
  NPC stands on the marker, the others on a small spiral around it, each on the first dry ground
  within a few blocks of the marker's height. A spot inside terrain (a marker built over since) is
  lifted until the body is free (`clearOfTerrain`), for the player's spawn and for NPCs, whose
  update also frees one buried by a later edit. A rename or new lines only update the NPCs' copy of
  the marker (new lines start over from the first).
- **Active windows** (ADR-023): a spawn marker's `activeFrom`/`activeTo` are times of day (0..1,
  like `Session::timeOfDay`; 0 midnight, 0.5 noon). `activeAt` is true in `[from, to)`, past
  midnight when from > to (0.8, 0.2: at night), and always when they're equal (both 0, the default).
  `syncSpawns` takes the time of day: an inactive marker spawns nothing, and its NPCs leave once
  they aren't fighting (`Entity::angry()`: Chase, Windup, Recover), its corpses once looted (an
  empty one after `kCorpseLinger`, 3 s, so a kill shows) or back up; the missing ones don't come
  back until the window opens again. Then only the missing slots (`Entity::slot`) spawn, so one
  still out (a fighter, a corpse with loot) keeps its state; any marker missing slots fills them
  the same way, and only a changed marker respawns all of its NPCs. A player save loaded where the
  save's time is outside a marker's window drops that marker's saved NPCs with a note saying so.
  `step()` runs `syncSpawns` every tick
  when any marker has a window (worlds without one pay nothing; a per-marker tally buffer, no
  hashing), after combat, so a corpse that
  gets back up by day goes before it's drawn. Editing or the debug slider jumping the clock apply
  on the next tick. The debug window's World tab lists the timed markers, active or not, and how
  many of their NPCs are out.
- **Lines** (ADR-016): talkers say the marker's next line on each E (cycling) instead of a random phrase;
  anyone that fights (guards too) says the next one when it enters `Chase` from calm (`setState`), at most every
  8 s, so hits from beyond its chase range (where it gives up at once) don't make it shout per hit;
  a shout's balloon doesn't hold a calm NPC still. Only the living talk. A balloon stays up 5 s,
  longer for long lines. With lines and count 1 the NPC's `displayName()` is the marker's name (nameplate,
  debug window), so a story character is one marker. Caps: 8 × 160 bytes per marker, ~5 MB for
  4096 markers at most, so no total cap.
- **Older and new worlds** get `defaultMarkers`: one spawn marker per NPC `spawnEntities` places
  around the world spawn, with its kind's wander radius. The first NPC of a marker is seeded like
  `spawnEntities` seeded it, so the same NPCs (positions, facing, villager name) come back. A world
  that has a `MARK` section, even an empty one, keeps exactly its markers.
- **Editor:** tool 8 places the panel's marker on the aimed column's ground (under trees) or selects
  the one standing there. Selecting, Go to, and any change to the selected marker (undo, Claude)
  load its settings into the panel; Update selected applies them but keeps the kind (changing Kind
  deselects); Delete removes it, and a selection that disappears is cleared. In editor mode every
  marker within 300 blocks of the camera gets a pin with `#id name`, and regions (and the selected
  marker's radius) a circle projected onto the ground under the trees, all on ImGui's background
  draw list. The panel edits a spawn's lines (hidden for the dummy, when Update selected keeps the
  marker's lines) in a multiline box, one per text line (blank ones are
  dropped). Circle heights are cached per marker until the world changes, and only the 32 nearest
  circles are drawn. The panel lists the markers with Go to and Remove (an instance's own say
  `(of camp #12)`, their pins `(of #12)`); the debug window's Player tab shows the current region.

## Command server and MCP bridge

Claude edits the world through the running game, so its edits share the user's undo history,
remesh path and screen.

- **Protocol:** newline-delimited JSON over TCP on 127.0.0.1 (port 47800, `--port`, `--no-serve`;
  never in benchmark mode). Request `{"id", "cmd", "args"}`, response `{"id", "ok": true, "result"}`
  or `{"id", "ok": false, "error"}`. One client at a time, any number in sequence; one with no
  complete line for 5 s is dropped. No authentication: localhost only, a development tool. A line
  that doesn't start with `{` closes the connection, so a browser's cross-origin POST dies at its
  HTTP request line and its body never runs. Responses are serialized with invalid UTF-8 replaced,
  and error texts have non-ASCII bytes replaced (parse errors quote the raw input).
- **Threading:** `CommandServer` owns one thread that only does socket I/O (`poll` with a 100 ms
  timeout, so shutdown never hangs). It puts each request line in a one-slot mailbox and waits; the
  main thread checks the mailbox once per frame (one atomic load when idle), right after the
  frame's own editor input and before drawing. It runs the command, then `reply` wakes the thread,
  which writes the response and reads the next line. So commands run strictly in order, one per
  frame, on the main thread, with no locking of the world.
- **Dispatcher:** `runCommand` (core library, unit-tested without sockets or GPU) parses and
  validates arguments (types, ranges, materials by name, centers inside the world, fill and
  inspect boxes clamped to it and capped at 128³), refuses JSON nested deeper than 64 levels while
  parsing (copying and dumping a deep tree recurse), checks every op with `checkEdit` and the
  command's work budget before applying any, and applies them with `join` so one command is one
  undo step. The budget is an estimate of main-thread time, at most 50 ms: 20 ns per voxel of a
  brush's `editBox` (its full columns, walked with a 1 MB stride), 1 ns per fill voxel, and 0.8 ms
  per chunk column of the command's bounding box (the game remeshes all of it; a world-spanning box
  took ~0.3 s). The weights are measured worst cases (M1 Pro, release); the worst accepted command
  in an adversarial run took 27 ms including the remesh. A 5-wide road fits 160–400 blocks. `save` copies only go to a `.flyw` directly in `worlds/` or the
  world file's folder, after resolving `..` and symlinks. It returns the
  response plus what only the game can do: the dirty box to remesh and flash, a save request, a
  screenshot camera, a goto target. Errors change nothing.
- **Prefab commands:** `prefabs` rescans the folder; `place` checks the prefab, column, ground y,
  rotation, seed and every param (range, known name) before touching anything, and is budgeted by
  `prefabBox` (20 ns per voxel plus the remeshed chunk columns); `remove` by the instance's box;
  `move` by the old and new boxes together (the new one at the ground before the removal: an
  estimate), so far moves are refused with advice to remove and place again.
- **`catalog`:** the loaded NPC kinds (string id, name or null, level, health, temper, attackable,
  talks, wander radius, chase speed, aggro range, weapon and loot with chance) and the material
  names (all, and the solid ones), plus every block name. The MCP bridge builds its schemas from it.
- **Marker commands:** `markers` (optionally of one kind and in a region, a region's circle
  counting), `mark`, `edit_marker`, `unmark`. Markers stand at the center of column `x`, `z` on
  ground voxel `y` (default the column's top). `map` draws them as dots labelled with their id
  (white player spawn, red spawn, yellow poi, cyan region with its circle) and lists the drawn ones
  (at most 500) with their name and pixel, since the map has digits only. A circle that misses the
  image or covers all of it isn't drawn, and one has at most as many points as the image's outline
  has pixels, so 1024 huge regions don't slow a small map. Default ground `y` is under any tree;
  ids must be whole numbers. A spawn's `active: [from, to]` sets its window (`[]`: always), listed
  as `active` (rounded to 4 decimals) only when it has one.
- **`path`:** samples the polyline every half brush radius, takes the ground height under each
  sample (ignoring trees), averages it over ±(16 + width) blocks along the path (or uses a fixed
  height), subtracts `depth`, and emits Flatten discs at full strength, then Paint discs.
- **`map` and `heights`:** column tops come from a layer-by-layer scan from the top down (rows are
  contiguous in x, so it's cache-friendly), on every `step`-th column. `map` colors each column by its
  top block (kPalette in sRGB), shades by slope toward the north-west light and by height, tints
  water by depth, draws grid lines with 3×5 pixel digits, and writes a PNG with stb_image_write.
- **Screenshot:** `Renderer::snapshot` draws the scene (`drawScene`, the same code as the window's
  frame, with its own culling and without the UI) into offscreen color/MSAA/depth textures of the
  requested size, downloads the resolved image and waits on a fence. The window's targets, culling
  freeze and stats are untouched; the player and NPC models are the current frame's. It blocks the
  main thread for the render (~8–20 ms at 960×540–1600×900).
- **Feedback in the game:** each command adds a line (with its time, red if rejected) to the
  Editor panel's MCP log, and an edit's dirty box flashes for 3 s. `goto` moves the editor flight
  only if Follow Claude is on.
- **MCP bridge:** `tools/flycoos_mcp.py` is a dependency-free stdio MCP server (initialize, ping,
  tools/list, tools/call; protocol 2025-06-18, accepting the client's version if it knows it). Each
  tool is one game command over a fresh connection; `screenshot` and `map` return the PNG as image
  content, read only from `$TMPDIR/flycoos` (where the game writes them and deletes them after an
  hour). The NPC kinds (`entity` enum and a description of each kind's traits) and the materials in
  the schemas come from the game's `catalog`, fetched afresh on every tools/list (2 s timeout).
  A list made without it (game not running or too busy) has no enums and says so; the next tool
  call that reaches the game then fetches the catalog and sends `notifications/tools/list_changed`.
  A game that answers `catalog` with an error (an older build) isn't asked again. The catalog fetch
  never changes a tool call's own result. Lines that aren't UTF-8 JSON get -32700, non-objects and requests without a method -32600.

## Voxel light (`light.h`, ADR-019)

Two channels of 0..15 per voxel, stored as one byte `sky << 4 | block`:

- **Sky light** is 15 in every voxel with only air above it up to the world top. From there it
  spreads in all six directions, losing `lightCost` per voxel entered: 1 for air and leaves, 2 for
  water; everything else stops it. Only air passes the full 15 straight down, so the ground under
  a canopy, inside a house or at a lake bed is lit by what spreads in sideways.
- **Block light** floods the same way from emitters: `Ember` emits 13 (its own voxel holds 13),
  `Lantern` 11 (a dimmer light for interiors).
- **Storage:** `Light` keeps one entry per 32³ chunk: a single `fill` value, or a 32 KiB array once
  its voxels differ. Open sky and solid ground cost nothing; the story world needs 47 MiB in 1,515
  of its 6,144 chunks (a dense grid would be 192 MiB). A relight that makes a chunk uniform again
  frees its array. Outside the volume reads as open sky.
- **Filling** (`relightColumns`, behind both entry points) works on a set of whole columns: find
  each column's sky top (one past its highest non-air voxel, scanning layers top-down in parallel),
  rewrite the columns' voxels as 15 above the top and 0 below (plus emission), in parallel per
  chunk, then flood from seeds in waves, raising a neighbour's channel to `value - cost` when that
  is brighter. Seeds are emitters, open-sky voxels beside a column that is dark at their height,
  the voxel above a leaf or water top, and the lit voxels of the columns just outside the set.
  Raising only moves values toward the exact result (the brightest path), so order doesn't matter.
- **After an edit** `relight(box)` refills the columns within 15 of the box (all heights, since
  sky light falls any distance): light from anywhere farther changes by at least 16 on the way, so
  it can't reach, and the unchanged light just outside seeds the border. That replaces the usual
  BFS add plus two-phase removal with one code path, and is exact: `core_test` checks random edits
  across chunk borders against a full fill. It returns the box of voxels whose light changed, which
  `updateWorld` adds to the remesh, so no stale light seams remain. Small edits relight in
  ~1.1–1.5 ms; a 64×10×64 fill in ~4.6 ms; clearing 200×4×200 in ~23 ms.

## Meshing (`meshRegion`)

Per region (a 32³ chunk, or a whole character part):

1. Skip the region if it is all air.
2. For each of the 6 face directions, sweep slices along the face axis. Build a 2D mask of visible
   faces, keyed by `block | AO << 8 | light << 17`. A face is visible if the neighbour isn't opaque.
   Water faces are visible only against air. The light is the `Light` value of that neighbour (the
   voxel in front of the face); without a `Light` (character parts) every face is under open sky.
3. **AO** per face corner is the classic 3-sample rule (`side1 && side2 ? 0 : 3 - (side1 + side2 + corner)`),
   sampled in the cell outside the face.
4. **Greedy merge**: grow rectangles along u, then v, while the key is identical. Faces with different
   AO or light never merge, so both stay exact (light costs ~5% more quads in the story world).
5. Opaque and water quads go to separate lists, so water can be drawn in its own blended pass.

Neighbours are read from the full `Grid`, so chunk borders produce no internal faces and AO is
seamless across chunks. The full world (~2,500 non-empty chunks, ~1.8M quads) meshes in ~1.1 s using
all cores.

### Packed quad layout (8 bytes)

Defined in `src/mesher.h`, decoded in `voxel_vs`:

```
a (uint32):  x:6 | y:6 | z:6 | (w-1):6 | (h-1):6 | unused:2      position relative to region origin
b (uint32):  face:3 | ao:8 (corners 0..3, 2 bits each) | block:8 | light:8 (block:4, sky:4) | unused:5
```

- Faces: `0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z`. A face on axis `a` spans `u = (a+1)%3` (width) and
  `v = (a+2)%3` (height). For positive faces `u × v = normal`, so corners 0→1→2→3 =
  (0,0),(1,0),(1,1),(0,1) wind counter-clockwise when seen from outside.
- AO values run from 0 (dark) to 3 (lit). Light is the byte `Light` stores, `sky << 4 | block`.

## Rendering

- **API:** SDL3 GPU on the Metal backend. MSL is compiled at runtime from `shaders/world.metal`
  (copied next to the binary at build time).
- **Vertex pulling:** there are no vertex buffers. Each chunk draws `6 × quadCount` vertices
  from a single storage buffer holding every world quad (one for opaque, one for water). The chunk's first quad index arrives in its
  Draw uniforms (`firstQuad`), and draws always use `first_vertex = 0`, because SDL_gpu.h
  documents `first_vertex` + `vertex_id` as non-portable.
  `voxel_vs` reads `quads[firstQuad + vertex_id / 6]` and takes the corner from `vertex_id % 6`.
  It flips the triangle diagonal when AO is anisotropic and reverses winding for negative faces.
- **Chunk slots and edits:** every chunk (empty ones too) owns a slot `{first, count, capacity}` in
  each world buffer; `uploadWorld` packs them back to back and leaves ~1/8 + 64k quads of headroom at
  the end. After an edit, `updateWorld(dirty)` relights the columns around it (`relight`, below)
  and remeshes only the chunks overlapping the dirty box and the box whose light changed, grown by
  1 voxel (faces, AO and light read neighbours), in parallel. A chunk's new quads overwrite its
  slot if they fit; otherwise it gets a new slot with 25% spare at the end of the buffer. All writes
  of one edit go through one transfer buffer and copy pass. When the headroom runs out, every slot is
  repacked from the CPU copy of the meshes (no remeshing). See ADR-012.
  Measured (M4, release): an edit touching 1–8 chunks applies and re-uploads in 1–2 ms, a radius-32
  brush (18 chunks) in ~2.7 ms; 400 random brushes averaged 2.7 ms with one repack, worst ~10 ms.
- **Per-draw data:** one 96-byte uniform push per chunk (`model` matrix, `modelLight`,
  `firstQuad`). Frame data (matrices, lighting, 64-entry palette and patch tones) is pushed once per frame to
  vertex and fragment slot 0. The projection is built from the swapchain size inside `render()`.
- **Binding order** (SDL_gpu.h, MSL): `voxel_vs` has `buffer(0)` Frame, `buffer(1)` Draw and
  `buffer(2)` quads. The fragment shaders use `buffer(0)` Frame.
- **Frame:**
  1. Push uniforms.
  2. Render pass into a 4× MSAA color + D32 depth target, resolving into `resolved_`:
     1. Sky: fullscreen triangle, no depth.
     2. Opaque chunks, back-face culled, after CPU frustum culling.
     3. Character: 6 parts, same pipeline, one model matrix each.
     4. Wireframe (debug only): the same quads again through a line-fill pipeline, depth-tested.
     5. Water: alpha-blended, depth test on, depth write off.
  3. Debug UI (when open): a second pass draws ImGui into `resolved_`.
  4. Blit `resolved_` to the swapchain. Owning the final image is what makes `--screenshot` possible.
- **Debug views:** `Frame.debug.x` selects shaded, greedy quads (color per merged quad), chunks
  (color per 32³ cell), AO only, normals, or light levels (sky blue-white, block orange;
  `--view light`). The branch is on a uniform, so it's coherent. The quad's light and index ride in
  the existing flat `blockAndQuad` output (block id bits 0-6, light 7-14, quad index 15-29, bit 30
  set for a glowing model, bit 31 set for models) rather than new ones: on Apple's tile-based GPUs every extra vertex output costs
  bandwidth, and two extra outputs measured ~15% slower frames.
- **Color:** the swapchain is `SDR_LINEAR` (sRGB format), so all lighting is in linear space. The
  palette is sRGB in `kPalette` and converted at upload. Output goes through an ACES filmic tonemap.
- **Shading:** a directional light (sun by day, moon by night) plus a hemispheric sky/ground
  ambient, multiplied by AO.
- **Albedo (ADR-028, [ART.md](ART.md)):** one colour per voxel. `albedoOf` fades the block's palette
  colour toward its two `kTones` variants by two smooth gradient-noise fields (lattice `size` voxels
  apart) read once at the voxel's cell centre, `floor(worldPos - normal / 2) + 0.5` (chunks draw in
  world voxel coordinates, so patches cross chunk borders). No per-voxel random term. Strength-0
  blocks and models (bit 31) take the plain palette colour. Tones upload linearised as
  `Frame.toneA/toneB`, the Render tab's global strength and size baked in.
- **Lighting (occlusion ramp):** the vertex shader maps each corner's AO level 0..3 through
  `Frame.aoCurve` (default `0.35, 0.62, 0.82, 1`) into the existing `ao` output, giving `o` (1 = open).
  `light()` first shades albedo toward `Frame.shadowTint.rgb` by `shadowTint.a * (1 - o)`, so corners
  get a cooler, deeper hue before lighting and fog; ambient is multiplied by `o`, the sun by half as
  much. The CPU fades the tint to 30% at night (moonlight is already blue). Both uniforms come from
  `RenderSettings` (debug UI Render tab, Lighting); off sends the old linear curve and no tint.
- **Voxel light (ADR-019):** each quad carries the sky and block light of the voxel in front of it
  (models: one value sampled where they stand, sent in the Draw uniforms' `modelLight.x`). `light()`
  maps a level through `curve(l) = x / (f - (f-1) x)`, `x = l/15` (`Frame.lightParams.y`, default
  2), and:
  - multiplies `curve(sky)` into `o`, so ambient and the cool tint follow sky visibility: roofed
    interiors, barrows and the ground under canopies go dark and cool;
  - fades sun/moon diffuse in over sky light `sunGate..1` (`smoothstep`, default 0.6), the stand-in
    for shadows;
  - adds `emberColor × curve(block)` on the untinted albedo, independent of the time of day, faded
    out where the sky's light is brighter (`1 - 0.5 × luma(sky lit) / luma(ember lit)`), so embers
    glow at night and in dark rooms but don't tint sunlit ground;
  - adds a small `darkAmbient × (1 - curve(sky))` so unlit voxels aren't black.
  Day/night stays a uniform change: stored light never depends on the time. The Render tab's
  Lighting section toggles voxel light (off = everything under open sky, the old look) and tunes the
  gate, falloff, ember color and strength and the dark ambient; it shows the light memory and the
  last relight time.
  Exponential-squared fog blends to the sky color in the view direction, so the horizon matches the
  sky.
- **Water:** the top surface is lowered by 0.12, with Fresnel sky reflection and a sun specular
  highlight.

## Simulation tick (ADR-017)

- `step()` runs at a fixed 60 Hz (`kTick`). Each frame adds its frame time, clamped to 0.25 s, to an
  accumulator and runs as many ticks as it covers: 0 on a fast frame, 2+ on a slow one. After a
  long stall the game slows down instead of spiralling.
- Every tick first copies each body's `pos`/`yaw` into `prevPos`/`prevYaw`. A frame draws models,
  nameplates, damage numbers and the camera target at `drawPos`/`drawYaw`: `alpha` (the leftover
  accumulator / `kTick`) of the way from the previous tick to the current one, yaw the short way
  round. So the picture lags the simulation by at most one tick (17 ms). `teleport()` (spawns,
  respawns, the editor's fly-to) sets both, so a jump is never drawn as a slide.
- A click or an E press is held until a tick takes it: never lost on a 0-tick frame, never doubled
  on a 2-tick one. Held keys (movement, a held attack) apply to every tick of the frame.
- On frame time: the camera's smoothing and orbit, editor brushes, the command mailbox, HUD layout
  and UI. Animation timers (gait, punches, breathing, falls, damage-number rise, balloon fades) live
  in the bodies and advance per tick.
- Benchmarks (`--frames`) add exactly one tick per frame, of both game and camera time, so a run
  walks the same path and takes the same screenshot on any machine. Frame-time stats still use the
  real clock.
- The debug window's Stats tab shows the ticks run this frame and the alpha.

## Player and animation

- **Physics:** AABB 0.7×1.8×0.7, feet-centered.
  - Moves resolve per axis (x, z, then y) in sub-steps of ≤ 0.4 blocks. On contact the player snaps
    flush to the block face.
  - A blocked horizontal move while grounded or swimming tries a 1-block step-up first.
  - `onGround` comes from a 2 mm probe below the feet after moving, so it holds at any frame rate
    (tested at 1000 Hz).
  - Defaults: gravity 28, jump speed 9.5, walk 5 b/s, run 9 b/s, with exponential velocity smoothing.
  - In water, gravity is weak and Space swims up.
- **Character models:** six floating parts (head, torso, 2 hands, 2 feet), each a tiny `Grid` built by
  `buildPart` and meshed by the same mesher. One character voxel is 0.1 blocks. Three models share
  the rig: player, villager and bandit (recolored; the bandit has a red hood and mask), the boar
  (a quadruped: hands are its front legs, feet its hind legs) and training dummy (post, base, straw body and arms, sack
  head; never animated). All parts of all models live in one GPU buffer; each frame draws a list of
  `ModelInstance`s. Held items are models too: `ModelSword` has voxels only in its first slot, and
  `poseModel` returns its pose (gripped in the right hand) through the `held` output. Empty parts
  are skipped at draw time. Worn armor is a model too: `ModelIronArmor` has plated versions of the
  humanoid parts (helm with a face opening, cuirass, gauntlets, sabatons; plate is its own `Plate`
  palette color, since `Iron` reads black in shade) at the same sizes. Each `ModelInstance` names
  the model per part (`partModels`), so a worn piece swaps in the part it covers without changing
  the pose. The rig has no legs, so greaves don't show. A model is lit by the light where its parts'
  origins average out; `ModelInstance::glow` (ADR-053, a burst's puff) draws it in its own colours,
  unshaded (bit 30 of `blockAndQuad`).
- **Tuning and flight:** walk/run/jump speed, gravity, fly and noclip live in `Body::tuning` (every `Player` and NPC body has one), so
  the debug UI edits them live. Flight ignores gravity (Space rises, Ctrl sinks, Shift is 2.5×) and
  still collides; noclip moves straight through terrain.
- **Camera:** an orbit around a smoothed target 1.4 blocks above the feet. `cameraClearance` marches
  a cube of half-size 0.3 (larger than the near plane's half-diagonal) out from the target in 0.1
  steps. The camera snaps in to the first blocked distance and eases back out, so terrain never
  cuts into the view and the camera doesn't pop.
- **Animation:** procedural, in `poseModel`.
  - The gait phase advances with the *distance actually travelled* (stride 1.7 blocks), so the feet
    don't slide or keep walking against walls.
  - Feet swing ±3 voxels and lift on the forward half of the swing. Hands counter-swing. The torso
    bobs, leans and twists.
  - `gaitAmount` blends between idle breathing and walking/running, and `airAmount` raises the arms
    and tucks the feet in the air.
  - Body yaw eases toward the movement direction, except during a punch, which keeps facing where it
    was aimed.
  - A roll (`Input::roll`, Alt or Circle; on the ground or up to 0.4 s after a jump's take-off, not
    mid-swing) turns the body to face the way it moves (or keeps its facing) and carries it at
    `rollSpeed`, slowing to half over `rollSeconds`; no swings until it ends. Jumping out of a roll
    keeps its speed and scales its time so the rest plays out in 0.12 s, pose unbroken.
  - A jump still takes off up to 0.1 s (6 ticks) after stepping off an edge, once (`offGround`). The pose tucks, turns forward once about the
    tucked middle, and is lifted so its lowest corner stays on the ground.
  - Punch: the fist shoots forward to shoulder height and the torso twists into it, alternating
    hands. The dummy wobbles back when hit and topples over when out of health.
  - Sword slash (main-hand weapon equipped): the right hand winds up to the right side, sweeps an arc
    at chest height through the front (blade pointing ahead at the impact frame), follows through to
    the left and settles back. The torso turns with the sweep. At rest the blade tilts forward.

## Stats and combat

- Five attributes (`Stats` in stats.h, a body's base); every other stat derives from them. No classes, so no attack
  power or other in-between step. Gear adds modifiers on top (ADR-022, below):

  | Attribute | Effect |
  |---|---|
  | Stamina (shown as Vitality) | +10 max health per point (the id stays `stamina`; "Stamina" in the UI is the melee bar, ADR-035) |
  | Strength | physical damage by the weapon's scaling (ADR-048): `damage = baseDamage + sum(attribute x scaling) x speed` |
  | Agility | +1% physical attack speed per point (haste: swing time = speed / (1 + 1% x agility)); damage by the weapon's scaling |
  | Intellect | damage by the weapon's scaling (no weapon scales with it yet; magic later) |
  | Spirit | +0.5 health per second, out of combat only; damage by the weapon's scaling |
  | Armor (from gear) | damage taken x (1 − min(80%, armor / (armor + K x attacker level))), at least 1; K = 40 |

- `speed` in the damage formula is the attack's *base* speed (WoW's weapon speed), not the hasted
  swing time. Slow attacks hit harder per swing, and agility only adds damage per second.
- The attack is the main-hand weapon's `Attack`, or the punch when the hand is empty
  (`mainAttack`):

  | Attack | Damage | Speed | Base player hits for |
  |---|---|---|---|
  | Punch (unarmed) | 1–4 | 1.0 s | 3–6 |
  | Iron Shortsword | 6–10 | 1.0 s | 8–12 |
  | Rusty Knife | 3–6 | 0.75 s | 5–8 |

  Base player: stamina 10, everything else 2, so 100 health and a swing every 0.98 s.
  The weapon is fixed when a swing starts (`swingWeapon`): changing gear mid-swing affects the
  next swing, not this one.
- **Stat pipeline (ADR-022):** a body's final stats are `sheet(c) = compute(c.stats, collect(c))`,
  uncached. `collect` lists the modifiers of the equipped items (each item's armor as a flat Armor
  modifier, then its `mods`, sourced by the item's name). `compute` does, per stat,
  `(base + Σflat) × (1 + Σincreased) × Π(1 + more)`, kept in 0..10⁶ (haste down to −0.9, a
  slow; a more factor below −100% counts as −100%): two +50% increased are ×2.0,
  two +50% more ×2.25. The `StatSheet` holds the five attributes, then the derived stats computed
  from the final attributes before their own modifiers: max health (stamina × 10), damage (a hit
  multiplier, base 1), haste (agility × 1%), regen (spirit × 0.5), armor (base 0) and damage taken
  (a multiplier on hits received, base 1). Max health, regen, swing time and both UIs read the
  sheet, so unequipping restores the base exactly. The debug Player tab shows the sheet: each
  stat's base and final value and every modifier with its source.
- **Every landed swing goes through `resolveHit(attacker, defender, attack, ctx)`** (entity.cpp):
  the attack's roll, + `attributeBonus` (each attribute × the weapon's scaling × attack speed, rounded once), × the attacker's damage, less
  `armorReduction` (stats.h): `armor / (armor + K x attacker level)` capped at 80%
  (`kMaxArmorReduction`), × the defender's damage taken, rounded once and never below 1.
  `HitContext` carries the damage RNG and K. K (`kArmorK` = 40) is the armor that halves a level 1
  attacker's damage, and a slider in the debug Player tab (`Session::armorK`, passed to
  `updateCombat`). The attacker level is `Combatant::level`: an NPC's kind's, the player's from
  XP (ADR-027; a debug slider in the Progression tab). The full Iron set (37 armor) takes 48% off a level 1 hit and 32% off a
  level 2 hit: the bandit's knife does 3–5 instead of 5–8. Debug "Hit for 10" / "Kill" bypass
  it. The damage number shows what landed.
- **Progression (ADR-027):** skills (`one_handed`, `unarmed`, `armor`; numbers in
  `data/skills.json`) run 1..100 from 1. A use earns `use_mult × base XP + use_offset` skill XP;
  L → L + 1 costs `improve_mult × L^1.95 + improve_offset`; each new skill level L adds L character
  XP; level N → N + 1 costs `25 × (N + 3)`. Kills add the creature's `xp` (default `10 × level`
  for hostile, attackable kinds; Hakon 60). Each level is one talent point (`level − 1 − spent`).
  `awardHitXp` (entity.cpp), called by `updateCombat` after every landed swing either way, is the
  only source of XP: attackable NPCs only, by the attack's *average base damage* (sword 8, punch
  2.5, knife 4.5), the dummy at 25%, halved per 20 counted hits on the same NPC (`xpWear` for the
  player's hits, `armorWear` for the NPC's; fading 1 per 10 s, saved in `WEAR`); armor XP only
  while wearing armor. Kill XP takes the same `xpWear` factor.

  | Skill | XP per use | Per level gained |
  |---|---|---|
  | One-Handed | 1 × base damage of a landed weapon hit | +2% damage with one-handed |
  | Unarmed | 3 × base damage of a landed punch | +3% damage with unarmed |
  | Armor | 2 × base damage of a hit taken in armor | +2% increased armor |

  Skills (`per_level` × levels gained) and talents (`per_rank` × rank) are modifiers from
  `collect()`. A modifier `with` a weapon skill counts only in `sheet(c, skill)`, which
  `resolveHit` and `startPunch` call with `skillOf(swingWeapon)`; the plain sheet (health, regen,
  UIs) never counts it. Talents (`data/talents.json`): one tree per skill, tier N needs 3 × N
  points in its tree, `requires` a lower-tier talent of the tree at max rank, all in `canLearn`;
  respec clears the ranks. The Progress window (K) spends points; the debug Progression tab grants
  XP, sets level and skills, respecs and shows the formulas.
- **Melee** (ADR-035, melee.h): every attack is a `Move` of its weapon class's moveset (ADR-044,
  `data/weapon_classes.json`), its timings scaled by the weapon's swing time and the class's speed
  (fists 0.75x). The sword's moveset is described here; daggers, axes and maces swing their own
  (ADR-044). A tap of LMB / R1 is the next of four lights (diagonal,
  backhand, sweep, a thrust finisher; the chain goes on from a light's chain point or within 0.25 s
  after one); held 0.16 s it's a heavy (a thrust, then an overhead chop), as is R2 at once. A heavy
  holds its windup pose while its button is held and is charged after 0.55 s (x1.45 damage, x1.5
  stance damage). The move under way picks a different one: running (a leaping sweep), late in a roll
  or just after (a rising thrust; the roll ends), in the air (a chop that waits for the landing),
  just after a block (a guard counter), at a stance-broken enemy in reach (a riposte, x3, can't be
  hit). Presses wait up to 0.5 s for the body to be free (the input buffer); a roll, the guard or
  walking cuts a swing from its cancel point. Attacks step forward into the hit (`Input::speed`) and
  turn toward the aim through the windup: the lock-on target, else the way the player moves, else
  the facing. Damage lands once, at the impact frame, on the nearest living, attackable entity within
  2 blocks and 60° of the facing (`inReach`), times the move's damage (`HitContext::power`); hits
  freeze both bodies a moment (hit-stop). Stamina (100) pays for attacks (the sword's: 10 to 22), rolls (14),
  blocked hits and running in combat; it refills at 45/s after 0.5 s without acting (x0.35 while
  guarding). RMB / L1 guards: a hit from the front (70°) loses 75% of its damage (bare arms 40%) and
  costs 10 + 2.5 per point of damage in stamina (35% less with a sword); with too little left the
  guard breaks and the player reels 1.1 s. Unblocked hits make the player flinch (0.3 s, cancelling
  its swing) unless its poise (0.4 x armor) is above the hitter's `hit_stance` or it's in a heavy's
  windup (hyperarmor); a roll can't be hit from 0.04 to 0.34 s in. NPCs have a hidden stance
  (`stance`): hits wear it down (it comes back after stance/13 s, at 13 a second), at 0 it breaks
  (2.5 s on a knee, open to a riposte, which knocks it down 1.2 s), and a hit of at least its `poise`
  makes it flinch, cancelling its swing. NPC swings land the same way on the player.
- **Criticals** (ADR-045, entity.h): an attack from behind an NPC (within 60° of its back) that
  isn't attacking, rolling or reeling and wasn't hurt for 2 s is a backstab: the riposte move
  (`criticalTarget`, `opensToCritical`, `behind`), which holds the NPC until it lands
  (`holdCritical`, called by `step` before the NPCs think) and knocks it down. Every other hit, both ways, rolls `crit_chance` (base 5%) for `crit_damage` more (base
  +50%); the damage number marks it (bigger, orange, "!"). `Player::trauma` shakes the camera
  (main.cpp, `GameSettings::cameraShake`).
- **Status effects** (ADR-046, entity.h): the stats `bleed`, `poison`, `frost` are what a landed hit
  adds to the target's `Combatant::buildup` (`buildUp`, times what a guard let through), and
  `*_resist` (base 100) what fills it; bars drain at `gStatus.decay`. Full, the status takes effect
  (`afflict`): a bleed bursts 10% of max health + 5, poison is a `TimedEffect` ticking 0.5% + 0.5 a
  second for 10 s, frostbite bursts 10% + 3 and adds +20% damage taken for 15 s. `Combatant::effects`
  is the one list of timed effects (TALENTS.md foundation 4): `collect()` adds their modifiers,
  updateCombat runs their clocks; a death clears them. Status damage skips armor. `hurt()` keeps the
  last four `DamageNumber`s (a hit's, or a status's by kind) for the HUD. Creatures' `mods` (data)
  are copied into `Combatant::innate`.
- **Healing flasks and rest points** (ADR-047): `Move::Drink` is a code move like the parry
  (`kDrinkSpec`, `codeMove`): updateMelee starts it from a buffered `Want::Drink` on the ground, it
  walks at `gMelee.drinkSpeed`, and its impact frame calls `drink()` from updateCombat instead of a
  hit (a charge heals its `flask_healing` share; `Combatant::flasks`, filled to the `flasks` stat). A
  flinch (`interrupt`) before then cancels it. `canRest` (session.h): alive, not hit for
  `kCombatSeconds` (poison's ticks don't count), nothing angry within 24 blocks, an Ember within `kRestReach`; `step`'s interact rests (`rest()`) after loot
  and talk and sets `Session::restPoint`, where `respawnPlayer` puts the player back, flasks full.
  The endless world generates a campfire per 96x96 cell where the ground allows
  (`generateChunkColumn`). model.cpp draws the left hand raising `ModelFlask` (`sip`).
- **Weapon scaling and two-handing** (ADR-048): `Attack::scaling` (items.json `scaling`; default
  strength 1 for fists and creatures) by `kScalingStats`; `attributeBonus` sums `attributeShare`
  (attribute x scaling x the `scaling` stat x speed) and rounds once; `scalingGrade` gives E..S.
  `twoHanding()` (melee.h) is `twoHanded` with a main-hand weapon: `collect()` adds strength more
  (`gMelee.twoHandStrength`), `shieldOf` returns null (so every guard path uses the weapon's
  `twoHandNegation`/`twoHandBoost`), `parryOf` 0. updateMelee toggles it from a buffered `Want::Grip`
  once the body is free and eases `gripBlend`; model.cpp holds the weapon in front at rest
  (`kTwoHandIdle`), puts the left hand `kPommel` voxels down the weapon's axis in every pose, and
  hangs the shield on the back (`kShieldBack`).
- **Great weapons** (ADR-049): `WeaponClassDef::twoHanded` makes `twoHanding()` true with that
  class's weapon in hand (or swinging), whatever `twoHanded` says; updateMelee drops a grip press
  then (the flag is kept for the next one-handed weapon). `reach` and `arc` (`arcCos`, worked out
  at load) replace `kPunchReach` and the 60 degrees in `inReach` (the held weapon's, NPCs' too), so
  `punchTarget` and `criticalTarget` reach as far, and `answer` sees a swing coming as far. model.cpp: `ModelGreatsword` / `ModelGreataxe` with long grips
  (`heldGrip`, the left hand `pommel()` voxels below), `kGreatswordSwings` / `kGreataxeSwings`
  (long blades end their downswings above the ground; a test checks every move).
- **Bows and projectiles** (ADR-050): a `ranged` class (the bow) has one move, `Move::Shot`;
  `pick` returns it for a ranged main hand, updateMelee draws at once on a press, holds at the hold
  point while the button is down (`charge` toward `gRanged.drawSeconds`, no auto-release), lets the
  draw down for a roll or the flask, drops presses with an empty quiver, and with the guard button
  sets `aiming` (no guard, no parry) and walks slowly facing the aim. `updateCombat` sets `loosed` at
  the shot's impact frame; `step()` then calls `loose` with `SimInput::aimAt` (main.cpp: the point
  under the crosshair in aim mode, else the lock-on target's middle) and ticks every projectile. The
  player's hits on NPCs go through `strike()` (entity.h), swings and arrows alike; `Strike::melee`
  adds hit-stop, the greatshield bounce and camera shake. model.cpp: `ModelBow`, `ModelBowDrawn`,
  `ModelArrow`, the bow pose (`nock`: raise, draw, the nocked arrow), `heldModel(const Player&)` and
  `arrowPose`; main.cpp draws every projectile as a model instance, moves the camera over the right
  shoulder by `aimBlend`, and hud.cpp draws the crosshair and the quiver's count.
- **Crossbows** (ADR-052): a ranged class that `loads` (`loads(b)`). `Player::loaded` is the
  crossbow with a bolt in it; `pick` returns `Move::Shot` for it loaded (no hold: full power,
  `gRanged.boltSpeed`) and `Move::Reload` empty. At the shot's release updateCombat also queues
  `Want::Light` (the automatic reload, replaced by a roll like any press); at the reload's impact
  frame it seats a bolt from the quiver. `loose` spends the bolt, not the quiver. The reload walks
  slowly like the shot and is let down by a roll before its seat. model.cpp: `ModelCrossbow`,
  `ModelCrossbowSpanned`, `ModelBolt` and the pose (`span`: raised, tipped, the string hand), held in
  both hands; main.cpp draws bolts for crossbow projectiles.
- **Throwables** (ADR-053): items with a `throw` record (`ItemType::thrown`: look, speed, lob,
  radius, pouch, stance, status build-up) and no slot. `Player::pouch` counts each by ItemId and
  `readied` is the one the throw button throws (saved in THRW; `fillPouch` on a new game, a rest and a
  respawn). `Want::Throw` (SimInput::toss) starts the code move `Move::Throw` (kThrowSpec) once the
  body is free, on the ground, with stamina and one left, not with a bow or crossbow in hand;
  `throwing` is the one in hand. At its impact frame updateCombat sets `hurled` and `step()` calls
  `hurl`: a projectile whose weapon is the throwable. A knife hits through `strike()` with
  `Strike::buildup` (its own build-up instead of the thrower's) and sticks to be picked back up into
  the pouch; a bomb or pot calls `burst` on the first body or voxel it meets: every living,
  attackable, not dodging NPC within the radius and in sight takes a `strike()` from the burst point
  (less toward the edge, `gRanged.burstEdge`), never the player. model.cpp: the left-hand throw
  (`toss`), `ModelThrowingKnife`/`ModelBomb`/`ModelPoisonPot` (in the hand through poseModel's
  `flask` output, in flight a knife by `arrowPose`, the rest `tumblePose`), `ModelBurst`/
  `ModelPoisonBurst` (`burstPose`, drawn glowing). The Bags window's pouch row readies one; the HUD
  shows it left of the flask; the debug Player tab's Throwables node edits the counts and knobs.
- **Archers** (ADR-051): a creature whose `weapon` shoots has a range band (`keep_range`), an
  `aim_error` and maybe a `sidearm`. In `think()` its hands come first: the sidearm once the player is
  within `gAi.closeIn` in a fight (the bow back past the band's near end), never mid-move. With the bow
  out, Chase hands over to `keepDistance`: a line from its bow to the player's chest (`clearLine`, the
  voxel `raycast`) or it steps in; too far it steps in, too close it backs off facing the player (when
  it can), and in its band (or cornered) it starts `Move::Shot` once `sinceSwing` passes its
  `attack_cooldown`, holding the draw `gAi.drawHold` (the tell; Windup builds its `charge` as
  updateMelee does the player's), else strafes. updateCombat sets its `loosed` at the shot's impact
  frame and `step()` calls `loose(s, e)`. The NPC→player hit moved out of updateCombat into
  `strikePlayer()` (entity.h), swings and arrows alike (`Strike::melee`: only a swing can be parried,
  freezes its attacker on the guard and bounces off a greatshield). An archer with its bow out takes no
  side or attack token (`assignSides`) and doesn't guard. The Entities tab shows its band, line of
  sight, hands and draw; the AI node has `drawHold` and `closeIn`.
- **Shields and the parry** (ADR-036, melee.h): an off-hand item with a `shield` guards by its own
  numbers instead of the sword's (`guardOf`): the Oak Buckler stops 80% and saves 46% of the stamina,
  the Heater Shield 100% / 56%, the Tower Shield 100% / 72%. A guard of at least 65% boost makes an
  attack of less than 20 `hit_stance` bounce: its attacker reels 0.6 s. Q / L2 parries with a shield
  that can (`parry`: the parry's length, x; greatshields can't): 10 stamina, a `Move::Parry` that
  commits to its end (0.75 s x the shield's). A hit from the front 0.1 to 0.33 s into it (at speed 1)
  is deflected: no damage, and the attacker reels 1.5 s with its stance broken, open to a riposte; a
  kind with `parries` above 1 only reels 0.6 s until its last one (they don't wear off; `Entity::
  parried` counts them). Caught outside the window, or by a kind with `parryable: false`, the parry is
  a weak guard (half the shield's negation and boost). The debug Player tab shows the guard and
  tunes these.
- **The backstep** (ADR-038, player.cpp): the roll button with no direction, on the ground, hops back
  about 2 blocks facing ahead (`Body::backstep`; it counts as a roll), for 8 stamina, with i-frames
  0.02 to 0.14 s in; attacking late in it or just after is `Move::Backstep`, a lunging sweep.
- **Equip load** (ADR-039): the worn items' `weight` (`equipWeight`) against 30 + 2.5 per vitality
  (`maxEquipLoad`): under 30% light (rolls farther), up to 70% medium, over it heavy (a short, slow
  roll with fewer i-frames, slower to roll again and to regain stamina), over 100% overloaded (no
  roll, backstep or run). `loadOf` classes it; the body keeps `load` and the roll's `rollLoad`.
- **Damage types** (ADR-040): slash, strike, pierce. A weapon's `damage` is its swings' (a slashing
  weapon's thrusts, `MoveSpec::thrust`, pierce), fists strike, a creature without a weapon uses its
  `damage`. A hit on a creature is scaled by 1 − its `resist` for the type; a shield stops its
  `negation` for the type.
- **Perfect guard** (ADR-041): a hit in the first 0.15 s of a guard raised after 0.3 s down takes no
  damage and half the stamina. With the off hand empty a knife (its `parry`) or fists parry like a
  shield.
- `hurt()` damages any body; `damage()` is hurt() for attackable entities and also angers fighters
  and rolls loot on a kill.
- Dealing or taking damage puts a body in combat for 5 s; spirit regeneration only runs out of
  combat.
- At 0 health a body falls over and can't be hit. NPCs stay dead (ADR-056): nothing respawns, and the
  training dummy is immortal instead (its hits show their numbers and take no health). The player is
  knocked out: no movement or attacks, then back at the player spawn (marker, else the world spawn) with full health after 5 s.
- HUD: a damage number pops above whoever was hit and rises for a second (the latest hit only):
  gold over NPCs, red over the player.

## Items and inventory

- Items are defined in `data/items.json` (ADR-018) and loaded at startup by `loadDefs` into a table
  indexed by save id: name, equip slot, weapon `Attack`, bag size, armor. A record:
  `{"id": "iron_helm", "save_id": 5, "name": "Iron Helm", "slot": "head", "armor": 6}`; optional
  fields are `slot` (head, chest, hands, legs, feet, main_hand, off_hand, trinket, bag; default none),
  `attack` `{min, max, speed}` (required for main_hand), `class` (required for main_hand: dagger,
  sword, axe or mace, ADR-044), `bag_slots` (required for bag), `armor`,
  `shield` (off_hand only, ADR-036: `{size: small | medium | great, negation, boost, parry}`, the
  last two shares 0..1, `parry` the parry's length x, omitted for none; greatshields can't parry;
  `negation` is one share or one per damage type `{slash, strike, pierce}`, ADR-040), `weight`
  (worn items, 0..100, the equip load: ADR-039), `damage` (weapons: slash, the default, strike or
  pierce, ADR-040), `parry` (main_hand weapons: they parry with the off hand empty, ADR-041),
  `mods` (worn items only: up to 8 stat modifiers such as `{"stat": "spirit", "flat": 2}`, each a
  stat id of `kStatIds` and exactly one of `flat`, `increased` or `more`; ADR-022), `throw`
  (throwables, ADR-053, no slot and an `attack` required: `{look: knife | bomb | pot, speed 5..80,
  lob, radius 0..8 (a knife none, a bomb or pot some), pouch 1..99, stance, bleed, poison, frost}`), and `copy_from` (another record's id: its fields but save_id, merge-patched with the record's own,
  so objects merge and `null` removes; at most 16 deep). Any bad record stops the game with a
  message naming file, record and field. `data/` is read from the working folder, else from the
  source tree.
- **Adding an item:** append a record with the next `save_id` (ids are saved in inventories: never
  renumber or reuse one). Code names an item only through the `ItemId` constants; an item that code
  must name gets one, at its save id, plus its row in `kNamedItems` (defs.cpp). Its icon (`inventory_ui.cpp`) and worn or held model
  (`model.cpp`) are code; without them it shows no icon. Shields need neither: their size picks both; weapons neither: their class does (ADR-044).
- The debug window's **Data** tab lists the loaded items (with their mods) and creatures and has **Reload defs**: it
  reloads `data/` between ticks and keeps the old definitions (showing the error) when the files
  are bad, drop an item or creature, or give a loaded save id another string id. Living NPCs keep
  the name, level, stats, wander radius, chase speed and weapon they spawned with; the other
  traits apply at once (the Entities tab's respawn picks up all of them).

  | Item | Slot | Notes |
  |---|---|---|
  | Iron Shortsword | Main Hand | sword, 6–10 damage, speed 1.0 |
  | Canvas Satchel | Bag | +6 item slots |
  | Rusty Knife | Main Hand | dagger, 3–6 damage, speed 0.75, +5% crit chance (ADR-045), 30 bleed build-up (ADR-046); dropped by bandits |
  | Boar Skin | – | crafting material later; dropped by boars |
  | Iron Helm / Cuirass / Gauntlets / Greaves / Sabatons | Head / Chest / Hands / Legs / Feet | 6 / 12 / 4 / 10 / 5 armor (37 for the set); bleed and frost resist 40 / 85 / 25 / 75 / 25, poison resist 30 / 70 / 20 / 60 / 20 (ADR-046) |
  | Wolf Pelt | – | crafting material later; dropped by wolves |
  | Bone Charm | Trinket | +2 spirit (regen 1 → 2 per second); dropped by barrow ghouls |
  | Oak Buckler / Heater Shield / Tower Shield | Off Hand | shields (ADR-036): stop 80 / 100 / 100% of a blocked hit (the buckler 85 / 70 / 80% of slash / strike / pierce, the heater 90% of strikes: ADR-040), guard boost 46 / 56 / 72%; the first two parry, the tower bounces weak attacks; weigh 2 / 4.5 / 13; no drop yet (the debug Items tab gives them) |
  | Iron Hand Axe / Iron Mace | Main Hand | axe 7–11 (speed 1.1, weight 3.5) / mace 6–10 strike (speed 1.15, weight 4), ADR-044; no drop yet (the debug Items tab gives them) |
  | Iron Greatsword / Barrow Greataxe | Main Hand (both hands) | greatsword 9–13 (speed 1.45, strength A, agility D, weight 9) / greataxe 10–14 (speed 1.55, strength S, weight 10), ADR-049; rare drops of bandits (10% when the knife doesn't drop) / barrow ghouls (12% when the charm doesn't) |
  | Short Bow | Main Hand (both hands) | 5–8 pierce (speed 1.0, agility B 1.1, strength E 0.3, weight 2), shoots arrows from the quiver (ADR-050); dropped by bandit archers 25% (ADR-051) |
  | Light Crossbow | Main Hand (both hands) | 12–16 pierce (speed 1.0, no scaling, weight 3.5), fires bolts from the quiver and reloads after (ADR-052); dropped by guards 50% |
  | Throwing Knife | – (the pouch, 10) | 4–6 pierce (agility 0.6), 40 bleed build-up, stance 6, flat at 32 b/s, sticks to be picked up (ADR-053) |
  | Clay Bomb | – (the pouch, 3) | 16–22 strike (no scaling), stance 40, lobbed at 14 b/s, bursts 2.5 blocks (ADR-053) |
  | Poison Pot | – (the pouch, 3) | 1–2 strike, 110 poison build-up, lobbed at 14 b/s, bursts 2.5 blocks (ADR-053) |

  Weapon classes (ADR-044, `data/weapon_classes.json`): one record per class (fists, dagger, sword,
  axe, mace, greatsword, greataxe), each with every attack move (`seconds`, `hold`, `hit`, `chain`, `cancel` in order,
  `damage`, `stance`, `stamina`, `lunge`, optional `heavy` and `thrust`) and optional `speed` and
  `hit_stop` multipliers; `copy_from` works as in items (fists copy the sword at speed 0.75).
  ADR-049 adds optional `two_handed` (always held in both hands; not fists), `reach` (blocks, 1–4,
  default 2) and `arc` (degrees either side of the facing, 15–120, default 60). ADR-050 adds
  `ranged` (shoots; needs `two_handed`): a ranged class has only the `shot` move, the others every
  move but it. ADR-052 adds `loads` (needs `ranged`): such a class (the crossbow) has the `reload`
  move too, and only it. Creatures can't wield one that loads yet.

  | Class | Lights: stance / stamina | Thrusts | Hit-stop | Feel |
  |---|---|---|---|---|
  | Fists | 10–14 / 12–14 | as the sword (strike) | x1 | the sword's moves, 0.75x long |
  | Dagger | 6–9 / 9–11 | light 3–4, heavies, running, rolling, jumps, riposte | x0.7 | slashes and stabs; riposte x3.5 |
  | Sword | 10–14 / 12–14 | light 4, heavy 1, rolling, riposte | x1 | the baseline |
  | Axe | 13–18 / 14–16 | none | x1.15 | chops, the head leading (pose `roll`) |
  | Mace | 16–22 / 15–17 | none (strike) | x1.3 | smashes, the most stance damage of one-handers |
  | Greatsword | 17–22 / 18–20 | rolling, riposte | x1.35 | two-handed; wide sweeps, overhead smashes; reach 2.6, 75 degrees |
  | Greataxe | 19–24 / 20–22 | none | x1.5 | two-handed; big chops, a wide cleave; reach 2.5, 70 degrees |

  Chest and legs pieces always give more armor than any head, hands, feet or hand item; a test
  checks the whole table.

- `Inventory` lives on the body (`Player::inv`): 8 equip slots (head, chest, hands, legs, feet,
  main hand, off hand, trinket; not saved yet, so the trinket was appended before `SlotBag`), a bar of 4 bag slots, and the item slots: the 12-slot backpack plus one
  container per equipped bag. The player starts with a shortsword and a satchel in the backpack.
- `moveItem` moves or swaps between any two slots when both ends accept what they'd receive: equip
  slots take their own slot's items, bag slots take bags, item slots take anything. A bag leaves the
  bar only when empty, and never into its own slots, so items are never lost. `quickMove`
  (right-click) equips from the item slots or unequips into the first free one.
- `stowItem` (loot, quest rewards): the first free item slot, else the item's empty equip slot (a
  bag: an empty bag bar slot); `canStow` asks without changing anything. `takeItem` removes one
  (item slots first, then worn). `discardItem` destroys a slot's item (Ctrl or Cmd+right-click in
  the windows), or an empty bag from the bar. No stacks and no dropping yet.
- **UI:** C opens Character (equipment around an attribute sheet with final attributes, what gear
  adds in green, damage, speed, damage per second, regen, armor and its damage reduction against a
  level 1 enemy; the weapons and the trinket below); B opens Bags (backpack,
  each bag, the bag bar, free slots). Drag to move, right-click to equip/unequip, Ctrl+right-click to destroy, hover for a
  tooltip (with armor pieces' "N Armor" and an item's mods in green). Icons are drawn with ImGui shapes. Any open window frees
  the cursor and pauses mouse look and attacks (a click queued before it opened is dropped, and a
  press that began in a window doesn't attack after it closes); the keyboard still moves the
  player. Esc closes the
  debug window first, then these.

## Entities

- An `Entity` is a kind, a name and a `Player` body (position, animation, health, level). Fixed
  traits per kind are defined in `data/creatures.json` (ADR-018), loaded after the items, into a
  table indexed by save id (= `EntityKind`, from 0). A record:
  `{"id": "wolf", "save_id": 4, "name": "Wolf", "level": 1, "stats": {"stamina": 8, "strength": 2,
  "agility": 4, "intellect": 2, "spirit": 2}, "attackable": true, "wanders": true, "model": "wolf",
  "temper": "aggressive", "aggro_range": 28, "wander_radius": 10, "chase_speed": 7.5, "loot":
  "wolf_pelt", "loot_chance": 0.6}`. Required: `level`, all five `stats`,
  `model` (a body model: player, villager, dummy, boar, bandit, wolf, guard, chief, ghoul) and
  `temper` (harmless, passive, aggressive). `immortal` (ADR-056; attackable kinds without loot): hits
  show their numbers and take no health. `name` null or absent gives each
  NPC a random villager name; flags default to false, ranges and speeds to 0; `weapon` (a main_hand
  item's id) and `loot` (any item's id) to none. `aggro_range` is for aggressive kinds only, a
  `loot_chance` needs `loot`, kinds that fight need a `chase_speed`, and stamina is at least 1. `copy_from` works as for items.
  `rare_loot` and `rare_loot_chance` (ADR-049, both or neither) are a drop rolled only when `loot`
  isn't dropped; a corpse still holds one item (the save is unchanged), and kinds without one draw
  their drop roll as before.
  `xp` (0–100000, ADR-027) is the character XP for a kill: by default 10 × level for hostile,
  attackable kinds and 0 for the rest; one that can't be hurt can't have any.
  The AI's numbers (ADR-020) are optional and only for kinds that fight: `chase_range` (8..128,
  default the larger of 32 and twice `aggro_range`, must be more than it; ADR-055), `reaggro_cooldown` (3 s),
  `give_up_seconds` (5), `attack_windup` (0.05..2, default 0.5 s: the swing holds its windup pose
  until then), `attack_recover` (0.5 s), `hit_stance` (15: the stance damage of its hits, ADR-035),
  `attack_cooldown` (0) and `flee_health` (a health fraction 0..0.95, default 0: never flees). A
  kind that fights must wander. Attackable kinds may set `stance` (default 30; 0: never breaks) and
  `poise` (0: every hit makes it flinch), ADR-035; kinds that fight may set `parryable` (default
  true) and `parries` (1..10, default 1: parries to open it to a riposte), ADR-036. The boar can't
  be parried; the chief takes 2. Sound (ADR-037): `voice` (its sounds are `<voice>_windup`,
  `_attack`, `_hurt`, `_death`; lowercase and `_`; none: silent), `voice_pitch` (0.5..2, default 1)
  and `body` (`flesh`, the default, or `wood`: how hits on it sound). Damage types (ADR-040):
  `damage` (kinds that fight without a weapon: what their attacks do, default strike) and `resist`
  (attackable kinds: one share or one per type, -1..0.9, default 0; negative is a weakness).
  Tactics (kinds that fight; ADR-042, ADR-043): `combo` (1..4, default 1: the longest string of
  hits), `delay` (0..1: the chance a hit after the first is delayed; needs a combo of 2+), `shield`
  (an off-hand shield item; only kinds with hands), and the chances it answers a player's attack:
  `guard` (needs a weapon or a shield), `parry` (needs a shield that parries or a weapon with
  `parry`), `dodge` (together at most 1), and `back_off` (after its own string). `mods` (ADR-046):
  its own stat modifiers, as items' (wolves: 22 bleed build-up; ghouls: 25 poison, +400 poison resist).
  Archers (ADR-051): a `weapon` that shoots needs `keep_range` (`[min, max]` blocks between feet,
  2 <= min < max <= 40) and allows `aim_error` (radians either way, 0..0.5) and `sidearm` (a main-hand
  weapon that swings, for up close); all three only with one, and no `guard` or `parry` (a bow does
  neither, and it holds no `shield`). Its `attack_cooldown` is the time from one shot's start to the
  next, and spaces its sidearm's swings too.

  | Kind | Combo (delay) | Guard / parry / dodge | Backs off | Shield |
  |---|---|---|---|---|
  | Bandit | 2 | – / 15% (knife) / 20% | 30% | – |
  | Wolf | 1 | – / – / 20% (a hop back) | 50% | – |
  | Guard | 3 (25%) | 60% / 10% / – | – | Heater Shield |
  | Bandit Chief | 3 (35%) | 10% / – / – | 30% | – |
  | Barrow Ghoul | 2 (40%) | – | – | – |
  | Bandit Archer | 1 | – / – / 25% | – | – (bow; Rusty Knife up close) |

  | Kind | Name | Lv | Sta/Str/Agi/Int/Spi | HP | Temper | Weapon | Drops | Talks |
  |---|---|---|---|---|---|---|---|---|
  | Training Dummy | "Training Dummy" | 1 | 10/0/0/0/10 | 100 | harmless (attackable, immortal, static) | – | – | no |
  | Villager | random from a name list (seeded) | 5 | 25/5/5/5/5 | 250 | harmless (attackable: runs from a hit at 6 b/s, ADR-056) | – | – | yes (E) |
  | Boar | "Boar" | 1 | 10/2/2/2/2 (the player's) | 100 | passive | tusks (= punch, 3–6) | Boar Skin 75% | no |
  | Bandit | "Bandit" | 2 | 12/3/2/2/2 | 120 | aggressive, aggro 24 | Rusty Knife (5–8) | Rusty Knife 40% | no |
  | Wolf | "Wolf" | 1 | 8/6/70/2/2 | 80 | aggressive, aggro 28 | bite (= punch, 7–10, quick) | Wolf Pelt 60% | no |
  | Guard | "Guard" | 4 | 16/5/3/2/4 | 160 | passive | Iron Shortsword (11–15) | Short Bow 50% | yes (E) |
  | Bandit Chief | "Bandit Chief" | 3 | 11/3/2/2/2 | 110 | aggressive, aggro 24 | Iron Shortsword (9–13) | Iron Helm 100% | no |
  | Barrow Ghoul | "Barrow Ghoul" | 3 | 7/4/2/2/2 | 70 | aggressive, aggro 16 | claws (= punch, 5–8) | Bone Charm 30% | no |
  | Bandit Archer | "Bandit Archer" | 2 | 9/2/4/2/2 | 90 | aggressive, aggro 30 | Short Bow (keeps 7–16 blocks, aim ±0.07 rad, a shot every 3.2 s), Rusty Knife within 3 | Short Bow 25% | no |

  The story's foes are tuned for a level-1 player (there's no leveling yet): in the whole Iron set
  with the sword, played the Elden Ring way (guarding a foe about to strike, attacking when its
  light lands first, riposting a broken stance), the chief alone or a pair of wolves or ghouls is won
  with about a fifth to two fifths of the player's health left, a pack of three wolves is lost, and
  bare-handed without armor the chief wins (`storyNpcs` simulates these fights). Stance and poise
  (ADR-035): dummy 80 / never flinches, boar 50, bandit 60, wolf 70, ghoul 90 / 12, guard 90 / 12,
  chief 180 / 20; windups wolf 0.28, ghoul 0.6, chief 0.6 s (the rest 0.5), recoveries wolf 0.15, chief
  0.4, ghoul 0.5 s (ADR-042: strings came with longer openings after them); `hit_stance` wolf 8, boar and bandit 12, ghoul 14, guard 20, chief 26. Higher levels get through more armor, so the
  chief and the ghouls pay for theirs with less health. Per kind there's also a wander radius (villager 12, boar and wolf 10, guard
  and chief 6, bandit 5, ghoul 4), a chase speed (ghoul 4, bandit and chief 5.5, boar and guard 6,
  wolf 7.5 b/s; the villager runs from a hit at 6; the player walks 5 and runs 9). Nothing respawns (ADR-056). Against the whole Iron set (armor 37) a wolf hits for 4–5 (quickly: agility 70), a ghoul 4–6, the chief 7–10.
- **Adding an NPC kind:** append a record to `data/creatures.json` with the next `save_id` (spawn
  markers save it: never renumber or reuse one). Reload defs in the Data tab, then place it with a
  spawn marker: the editor's Entity list, `mark`/`edit_marker` and the MCP schemas all read the
  loaded kinds. Code changes only for: a new body (a `Model` value and its painter in `model.cpp`,
  its name in `kModelIds` in `defs.cpp`, its nameplate height in `hud.cpp` if it isn't humanoid
  height); new loot (an item record, plus its icon in `inventory_ui.cpp`); or a kind that code must
  name (an `EntityKind` constant at its save id and its string id in `kEntityKindNames`, which
  loading checks).
- **Models:** the wolf is a quadruped on the boar's animation with its own part sizes (longer
  legs, snout and ears, a tail in the torso's grid); the guard wears the iron helm and gauntlets
  and a tabard over the cuirass; the chief is the bandit's rig drawn 1.25× wide and 1.12× tall; the
  ghoul is a skull-headed humanoid drawn thinner. Only existing palette colors (it was full at 32 then).
- **Wandering:** alternates standing (2–6 s) and strolling at 2 b/s (2–5 s) in a random direction.
  Before each step it checks the block ahead: no walls it can't step up, no water, no drops of 2+.
  Beyond its wander radius it heads home. NPC bodies use the same `updatePlayer` physics as the
  player, driven by AI input.
- **Temper and AI** (ADR-020): one state machine for every kind, `Idle` (kinds that don't wander) /
  `Wander` / `Talk` / `Chase` / `Windup` / `Recover` / `Return` / `Flee` / `Dead` / `Stagger` / `Search`, in `think()`; every
  transition goes through `setState` with a reason, kept in an 8-entry ring per entity. Harmless
  kinds never fight. Passive ones chase when damaged (`damage()` sets `EventDamaged`, read on the
  entity's next update); aggressive ones also when they see a living player within their aggro range
  (in 3D, and in line of sight: `canSee`, nothing solid from 1.5 above its feet to 1.1 above the
  player's; ADR-055), unless they got home in the last `reaggro_cooldown` (3 s). A chasing NPC runs at the player; within reach it winds up (`startPunch`: the next of the
  first three lights, holding its windup pose until `attack_windup` x 0.85..1.35 has passed, turning
  after the player until 0.12 s before the strike, then committed), the hit lands once at the impact
  frame if the player is still in reach and in front, then it stands `attack_recover` and chases
  again; waiting for its next swing in reach it holds a sword's length off; the weapon's swing time
  gates the cadence (the boar's swing is a head-down gore: drawn back, then lunging). A hit that
  makes it reel (ADR-035) puts it in `Stagger` (reason `flinched` or `stance_broken`) until it
  steadies, cancelling its windup; then it fights on. It gives
  up (`Return`) when the player is `chase_range` from it (not from home: no leash) or knocked out, or after
  `give_up_seconds` (5) unable to step toward a player out of reach (too far, or a body height above
  or below; no pathfinding). `Return` walks home ignoring the player (pending hits are dropped), then
  heals fully if it had to walk; a hit on the way brings it back to `Chase` unless it gave up for want
  of a way to the player (`lost_target`) and the player is still out of reach (ADR-055). A state forced in the debug window skips the enter
  actions (no shout). Below `flee_health` of its health (0 for
  every shipped kind: never) it runs from the player for 4 s, then returns. No field of view yet.
  **Pursuit** (ADR-055): fighting, it notes where it last saw the player (`lastSeen`, `unseen`); out
  of sight for `gAi.sightGrace` (0.5 s: round a corner) a chase turns into `Search` (reason
  `lost_sight`): it runs to where it last saw them, then turns one way and the other, looking; it
  chases again on seeing them (`found_target`, no shout) or when hit, and after `gAi.searchSeconds`
  (8) gives up (`gave_up_search`) and returns. A hit, even one it reels from, sets `lastSeen` too;
  searching counts as in a fight (`Entity::hunting()`: no resting near it, its health bar shows). Chasing, searching or going home, a blocked step tries
  45 then 90 degrees to either side (`detour`, checking the body's width: `roomy`); a detour counts
  toward `give_up_seconds` and a free step only winds it back. The Entities tab shows the state, time in it, the
  transitions, whether it sees the player and when it last did, and a force-state combo.
  For trying out fights it also spawns a picked kind 5 blocks in front of the player (`spawnAhead`,
  F6 too): not from a marker, so marker syncs and the player save leave it alone.
- **Packs** (ADR-034): `updateEntities` first gives every fighter on the player (`Chase`, `Windup`,
  `Recover`) a side to come in from (`Entity::side`, `pack`): evenly spaced around the player, in
  the order they stand, turned to fit where they are. Within 6 blocks, one that isn't at its side yet
  spirals round to it (up to 0.8 rad a step), then closes in; in reach it swings wherever it is.
  After moving, living NPCs push apart to 1 block (half each; ones that don't wander aren't moved).
- **Attack tokens and strings** (ADR-042): of the fighters on the player only `gAi.attackers` (2)
  hold a token (`Entity::token`) and may swing: one in its `Windup` keeps its own, the rest go to
  those within 6 blocks that have waited longest since their last string (`sinceSwing`); one
  recovering from its string keeps its token too. The others circle on a ring 3.6 blocks out at
  their side, facing the player, walking slowly one way round and then the other. A swing starts a string of 1 to `combo`
  hits: each next light starts at the last one's chain point while the player is within reach + 1,
  some (`delay`) held in the windup ~0.5 s longer, and each adds its swing time to the swing timer,
  so the pause after a string is as long as the hits in it would have taken apart.
- **Answers** (ADR-043): while it chases, an NPC whose player's attack can reach it (in front of
  the player, within reach + 1.5) picks an answer as the attack starts: a guard, a parry or a dodge
  by its kind's chances, once per attack, as soon as it's chasing (a heavy or riposte can't be
  parried: it guards). After a reaction time
  (0.06–0.18 s) the guard goes up (and stays 0.4 s after); a parry starts so its window meets the hit,
  and a dodge rolls aside (four-legged kinds hop back) 0.12 s before it, both give or take 0.07 s. It
  doesn't swing while it answers. One with a shield also keeps it up whenever it chases. The
  player's hit on its guard is the player's guard mirrored: the
  shield's or weapon's negation by type, stamina (`blockStamina` + per damage, less the boost; it
  refills as the player's), no stance damage; out of stamina the guard breaks (it reels 1.1 s). Its
  parry deflects lights (the player reels 1 s); its roll's i-frames let the hit pass through. After
  a block or a parry its next swing comes within 1 s with a 0.3 s windup (a counter). After its
  string it may back off (`back_off`) for 0.9 s, walking away facing the player.
- **Loot:** a kill rolls the kind's drop into `Entity::loot`. The corpse lies where it fell (ADR-056:
  until the player is `gAi.corpseGone`, 64 blocks, away and can't see it, and never while it holds
  loot); E within 3.5 blocks moves the loot into the bags, else into its empty equip slot (it
  stays on the corpse if neither has room, and E talks instead).
- **Default spawns:** dummy and villager just in front of the spawn, the boar about 14 blocks out, the
  bandit's camp about 34 blocks out: even at the edge of its wander radius more than its aggro range
  (24) away. A bandit archer (ADR-051) stands a few blocks behind the bandit, about 41 blocks out (aggro 30).
- **Talking:** E picks the nearest talker within 3.5 blocks. It shows its spawn marker's next line,
  or else a random phrase (never the same twice in a row), for 5 s, while it stops and turns to
  face the player.
- **Separation:** the player is pushed out of an entity's 0.8-block radius (never into terrain).
- **HUD:** nameplates (level, name, health bar; red name for aggressive, amber for other attackable,
  green for friendly; red health bar if attackable; friendly and passive ones show health only in
  combat) within
  40 blocks. They're hidden when terrain blocks the line of sight, using the camera's box march. The
  HUD always runs an ImGui frame; the UI render pass is skipped when there is nothing to draw.
- **From markers:** the NPCs come from spawn markers (see "Markers"); a world without markers
  gets the five above as default markers. Regeneration resets the markers to those defaults; the
  Entities tab respawns every NPC at its marker (clearing who died for good).

## Quests (`quest.h`, ADR-026)

- **Events.** Systems tell quests what happened through `Session::events`, one vector per tick of
  `GameEvent`s carried by value: `Killed{kind, spawnMarkerId}` (the player's killing hit, in
  `updateCombat`; the debug window's Hit and Kill too), `TalkedTo{spawnMarkerId}` (E),
  `RegionEntered{markerId}` and `ItemCountChanged{item, count}`. RegionEntered is for the innermost
  region only (`regionAt`), when it changes: from the Vale into Ravensford enters Ravensford, back out
  enters the Vale again, and an `entered` trigger on an outer region doesn't fire from inside an inner one.
  Item counts are derived: `step()` compares the player's counts (`countItem`: item slots, worn,
  bag bar) with last tick's (`Session::itemCounts`), so loot, quest actions, the UIs and a load all
  count without each pushing. `updateQuests` runs once at the end of `step()`: it swaps the vector
  out and hands each event to the triggers of every quest's current stage, as the quests were before
  that event (a quest another one started on it hears the next one, so quest order doesn't matter).
  An item's `ItemCountChanged` counts once per batch, and has_item reads the count now, not the
  event's. What handling raises (a stage's has_item re-check, an item it gave) lands in the next
  tick's batch: no re-entrancy, no feedback loop within a tick.
- **Data.** `data/quests/*.json` (file name order), each an array of quest records loaded after
  items and creatures (pass 2a collects every quest's id, stage ids and objective ids, so
  references may point forward; pass 2b resolves the rest to handles):

  ```
  {"id": "torvalds_helm", "title": "Torvald's Helm",
   "objectives": [{"id": "get_helm", "text": "Take the helm Hakon the Red wears"}, ...],
   "stages": [{"id": 0, "on": [{"talked_to": "Maren Holt", "do": [{"set_stage": 10}]}]},
              {"id": 10, "journal": "...", "actions": [{"show_objective": "talk_torvald"}], "on": [...]}, ...],
   "lines": [{"spawn": "Torvald", "if": [{"stage": "torvalds_helm", "value": 20}], "say": "..."}, ...],
   "needs": ["Torvald"]}
  ```

  - **Needs** (ADR-056): spawn marker names the quest can't do without. Once every NPC of one has
    died (`npcDead`) the quest fails, started or not: a started one with a toast ("Quest failed:
    Torvald's Helm (Torvald is dead)"), one not started silently, and it never starts. The journal
    says why under Failed. Each name once; `checkQuestMarkers` checks them like the others.

  - **Stages** are numbered 0–65535 (gaps leave room). Stage 0 is "not started": only its triggers
    (what starts the quest), no journal or actions. A quest needs one other stage. Entering a stage
    runs its `actions` in order and queues an `ItemCountChanged` for each of its has_item
    triggers ("has item" is a state: the player may hold it already).
  - **Triggers** (`on`): exactly one of `talked_to` / `killed` (a spawn marker's name),
    `killed_creature` (a creature id), `entered` (a region marker's name), `has_item` (an item id,
    `count` ≥ 1), optional `if` (conditions), required `do` (actions). The first trigger of the
    current stage that matches the event and passes runs; then the quest is done with that event.
    Before it runs, its actions (and the stages they enter) are played out on a copy of the
    inventory: if an item it gives wouldn't fit, it doesn't run (a "Make room in your bags" toast)
    and fires on the next matching event instead, so a reward is never lost. Done and failed quests
    listen no more and enter no stages; entering the current stage again does nothing.
  - **Actions** (closed set): `show_objective`, `complete_objective`, `fail_objective` (own
    objectives), `give_item` (`stowItem`: an item slot, else its empty equip slot or bag bar slot),
    `take_item` (one; the item slots first, then worn gear; not bags, which may hold items), `set_flag`
    (+ `value`), `set_stage` (one of its own stages but 0), `start_quest` (its lowest stage above 0,
    if not started), `complete_quest`, `fail_quest` (by quest id). `set_stage` from a stage's own
    actions chains up to 16 deep; a cycle stops there with a log line.
  - **Conditions** (all must pass, AND; `"not": true` flips one): `stage` (quest id, `op`
    `== != < <= > >=`, default `==`, `value`), `done`, `failed` (quest id), `objective`
    (`"quest.objective"`, `is`: hidden, shown, completed, failed), `flag` (name, `op`, `value`;
    unset is 0), `has_item` (item id, `op` default `>=`, `value` default 1), `in_region` (a region
    marker's name: any region of that name around the player, nested ones too), `npc_dead` (a spawn
    marker's name: every NPC of the spawns of that name has died, and there is one; from
    `Session::dead`, so it holds for good, ADR-056). `passes` is a
    plain function; dialogue asks it directly.
  - Errors are hard and name file, record and field (`quests/a.json: record 0 'q': field
    'stages[1].on[0].do[0].set_stage': 50 is not one of this quest's stages`); unknown fields,
    references (quests, objectives, items, creatures) and stages, duplicate ids (across files too)
    and out-of-range values fail the load.
- **Marker names stand in for aliases.** Triggers and conditions name spawn and region markers by
  name, resolved at run time against the world (`Marker::id` → name), since the world can change
  under the data. Names aren't unique (the story has three "Hakon's Raiders"), so on world load
  `checkQuestMarkers` warns (log, debug Quests tab) about each name that doesn't name exactly one
  marker of its kind. Act 1's quest names only unique ones.
- **Dialogue.** Quest-owned lines live in the quest file keyed by spawn marker name; the world's
  `LINE` section stays the unconditioned, in-game-editable fallback. On E (and the debug Talk
  button) `pickLine` finds the first line (quests in load order, lines in file order) for the NPC's
  marker name whose conditions all pass, and `talk` says it, unless it said that same line last
  time and the marker has lines of its own: then the marker's next line. So quest lines and the
  marker's lines take turns, and a new quest line is always said first. Shouts on turning angry say
  only the marker's lines. The line is picked before the tick's events are handled, so a talk that
  moves the quest on is answered from the state the player walked up in (Act 1 gives stage 0 its
  own lines for that talk). The loader warns when a line without conditions comes before another line of the
  same spawn (it would never be said).
- **State.** `Session::quests` (by quest handle: `QuestState` stage, done, failed, objective
  states) and `Session::flags` (one `string → int32` map) are saved (`QUST`, `FLAG`).
  `startSession` resets both. A defs reload may add or drop quests at the end, not reorder them.
- **UI.** J opens the Journal: active quests with the current stage's journal text and their
  shown objectives (a check mark when completed, dimmed when failed), then completed and failed
  titles (a failed one whose needed NPC is dead, started or not, says who and that it can never be
  finished or started now). `updateQuests` leaves quest news in `Session::toasts` ("Quest started/updated/complete/
  failed: title", "Make room in your bags: title"; one per quest per tick); main shows them in the notice line. The debug Quests
  tab lists every quest (set any stage, which runs its actions, reset, done/failed, objective
  states), the flags (edit, delete, add), the loader's and the marker check's warnings, and the
  last 32 events the drain saw with the stage changes they caused.

## Sound (ADR-037)

- **From the sim:** bodies set `Noise` bits (`Combatant::noises`) where things happen (player.cpp,
  melee.cpp, entity.cpp, session.cpp). At the end of each `step()`, `hear` turns them into `Sound`s
  in `Session::sounds`: the noise, where, who (`kind`, -1 the player), the ground under it, the move,
  the weapon (or what hit it, or the shield that blocked) and an amount (gait, fall speed, stance
  damage). Footsteps come from the gait: a foot lands each time `gaitPhase` passes a quarter turn
  (1.5 times as often for the quadrupeds). Bodies outside the loaded world are cleared unheard. The list
  stops at `kMaxSounds` when nobody drains it.
- **To the speakers:** each frame main.cpp `hear`s the frame's sounds at the player (panned by
  the camera's right), runs `updateAmbience`, plays the interface's sounds (windows, lock-on,
  equipping, quest news) and clears the list. `layersOf` is the table of what each noise plays. The
  device thread pulls `Audio::mix`; `play` and `setGain` take the same mutex.
- **Files:** `data/sounds/<name>_<n>.ogg` (or `<name>.ogg`): the takes of the sound `<name>`, mono for
  effects (they're panned), stereo for the ambience beds. A missing sound is silent and listed in
  the debug window's Audio tab; core_test checks every sound `layersOf` and main ask for has takes.
  Sources and licences: CREDITS.md.

## Hit feedback (`fx.h`, ADR-058)

- **From the sim, one way:** each frame main.cpp hands every `Sound` of the frame's ticks to
  `spawnFx` before the sounds are cleared. A hurt, death or crit carries where the blow came from
  (`Sound::from`, from `Combatant::hitFrom`, set next to `hitWith` in `strike` and `strikePlayer`) and
  whether the body wears plate (`plated`); a guard's noises come from straight ahead of it; an arrow's
  thunk from back along its flight. The sim never reads `Fx`, and `Fx` has its own RNG.
- **What each makes:** a body hit throws its creature's `blood` (data: red, splinters, dust; the
  default follows `body`): 6 to 12 bits by the hit's stance damage, 22 on a death, 14 more on a crit,
  out of the struck side. Plate adds 2 to 3 sparks. A block sparks (10), or with a small shield throws
  splinters and 2 sparks, or with bare arms a puff; a parry 18 sparks, a perfect guard 10, a bounce 8, a
  guard break 16. An arrow into terrain throws chips of the block, and sparks off stone.
- **Particles:** each a flat-coloured cube (one colour all its life). They fall under gravity; drops
  and chips that come down on an opaque voxel lie flat on it until they fade (blood 6 s, chips 1.5 s),
  sparks die there. At most `cap` (4096): a new one replaces the oldest. Nothing spawns over `reach`
  (48) from the eye. They advance by the frame's time while the sim runs (a benchmark: one tick).
- **The smear:** main.cpp calls `trailFx` for every body holding a blade, with its held item's pose.
  While `swingsBlade` (from the hold point to just past the impact frame) and the blade's tip moves at
  least 6 blocks/s, each new edge (`bladeEdge`: the blade's line in its model, so a dagger's is short
  and a greatsword's long) joins the body's samples. A sample fades out over 0.12 s (held during
  hit-stop); `updateFx` turns the samples into a ribbon of triangles, smoothed with Catmull-Rom, its
  alpha by age and fading toward the hilt.
- **Drawing:** `FrameParams::fx`. The renderer streams the particles (32 bytes each: position, size,
  colour, glow, flat, the light where it is) and draws them in one instanced call of 36 vertices each
  after the models, lit like a model voxel (sparks glow). The ribbon (16 bytes a vertex) is one draw
  after the water: unlit white, alpha-blended, no depth write, both sides.

## Day/night

`timeOfDay` runs from 0 to 1 (72-minute day, `kDayLength` = 4320 s; hold T for 60×). The sun path is tilted so its peak is
~51°, which keeps it inside the orbit camera's reachable view. `ambience()` in `renderer.cpp`
derives the following from the sun height:

- sun direction
- sky zenith and horizon colors, with a dusk tint
- light color and intensity (sun, or a dim bluish moon)
- ambient colors
- a night factor that drives the stars and the moon disk in `sky_fs`

## Performance (Apple M4, 1600×900, release)

| Metric | Value |
|---|---|
| World generation | ~0.15–0.2 s |
| Light fill (`fillLight`, story world) | ~0.16–0.20 s; 47 MiB of light arrays in 1,515 of 6,144 chunks |
| Meshing + upload | ~0.35–1.1 s |
| World file save / load (2.8 MB) | ~22 ms / ~13 ms |
| One brush stroke: edit + relight + remesh + upload | ~2–4 ms (relight ~1.1–1.5 ms; a 200×4×200 clear ~23 ms) |
| Remote screenshot (offscreen render + readback) | ~8–20 ms (960×540 to 1600×900) |
| Command server when idle | one atomic load per frame |
| GPU memory for world quads | ~14 MB |
| Mean frame time (benchmark, vsync off) | ~2–3 ms |

### Perf A/B 2026-11-03: pre-refactor (f7e14aa) vs 840bc30

6 alternating runs per cell, `--frames 900 --look 0 -20`, 1600×900; the story world is a scratch copy.
The machine ran on battery; about 1 run in 6 can pin to the display's 120 Hz with identical flags
(mean ~8 ms), none did here.

| Scene | Build | Mean frame | p99 (median) | Opaque quads | Light fill | Mesh + upload | RSS |
|---|---|---|---|---|---|---|---|
| Generated world, seed 1337, day | f7e14aa | 3.42 ms | 6.6 ms | 1,766,719 | – | 1.01 s | 335 MiB |
| | 840bc30 | 3.70 ms | 7.8 ms | 1,853,847 | 0.37 s | 1.07 s | 390 MiB |
| Story world, day (`--time 0.4`) | f7e14aa | 3.40 ms | 5.3 ms | 1,772,666 | – | 0.99 s | 339 MiB |
| | 840bc30 | 3.67 ms | 7.1 ms | 1,863,601 | 0.31 s | 1.06 s | 400 MiB |
| Story world, night (`--time 0.9`) | f7e14aa | 3.59 ms | 8.4 ms | 1,772,666 | – | 1.06 s | 338 MiB |
| | 840bc30 | 3.84 ms | 9.0 ms | 1,863,601 | 0.31 s | 1.11 s | 400 MiB |

The whole refactor plus the backlog costs about +0.27 ms (+7–8%) per frame, which is voxel lighting:
light breaks greedy merges (+5% quads, ADR-019), the light arrays add ~47 MiB plus ~10 MiB of
quads, and the light fill adds ~0.3 s to startup. Everything else (Session, fixed tick, data files,
AI, stats, quests, progression) is not measurable in frame time.

### Perf A/B 2026-11-04: 8009332 vs colour patches (working tree)

Same flags as above (`--frames 900 --look 0 -20`, 1600×900, `--no-serve`), up to 6 alternating runs per
cell, each binary run from its own checkout; the story world is a scratch copy. Runs with mean > 6 ms
(120 Hz present pin) or a 1000 ms hitch were discarded: 2 of 6 (story day, base and candidate each),
3 of 6 (generated, base and candidate each), 2 of 6 base / 1 of 6 candidate (story night), so some cells
rest on only 3-5 runs and the machine was heat-drifting. The candidate with `--patches 0` ran once.

| Scene | 8009332 | Colour patches | Delta |
|---|---|---|---|
| Generated world, seed 1337, day | 3.75 ms (3 runs) | 4.22 ms (3 runs) | +0.47 ms (+13%) |
| Story world, day (`--time 0.4`) | 3.77 ms (4 runs) | 4.04 ms (4 runs) | +0.27 ms (+7%) |
| Story world, night (`--time 0.9`) | 3.65 ms (4 runs) | 4.00 ms (5 runs) | +0.35 ms (+10%) |
| Generated, candidate `--patches 0` | – | 3.28 ms (1 run) | – |

The patch noise costs about +0.3-0.5 ms per frame (+7-13%) in the fragment shader; the quad counts are unchanged, and `--patches 0` brought the frame time back to (or below) the baseline, but that is a single run.

### Perf A/B 2026-11-04: 78be3e8 vs tree species (ADR-029, working tree)

Same flags (`--frames 900 --look 0 -20`, `--no-serve`), 6 alternating runs per cell, all under 6 ms.
The baseline runs the story world as it is; the candidate runs a scratch copy after `replant`
(826 trees as instances: 1.96M opaque quads vs 1.86M, 817 chunks drawn vs 725). Medians:

| Scene | 78be3e8 | Tree species | Delta |
|---|---|---|---|
| Generated world, seed 1337, day | 2.08 ms | 2.26 ms | +0.18 ms (+9%) |
| Story world, day (`--time 0.4`) | 1.99 ms | 2.31 ms | +0.32 ms (+16%) |
| Story world, night (`--time 0.9`) | 2.16 ms | 2.39 ms | +0.23 ms (+11%) |
| Story world as it is, both builds (3 valid runs each of 5) | 2.10 ms | 2.26 ms | +0.16 ms (+7%) |

About half the cost is the trees themselves: taller crowns reach into chunk layers that were empty
(+13% chunks drawn, +5% quads). The rest is the build on the same world (the 128-entry palette
doubles the frame uniforms pushed twice a frame to ~6 KB; nothing else in the shader changed); that
row rests on 3 runs on a heat-drifting machine. Frame times are still ~2.3 ms.

## Known ceilings

| Ceiling | Upgrade path |
|---|---|
| Fixed-size world (192 MiB dense grid) | Deliberate: the story world is hand-authored at this size (M1) |
| Edits remesh on the main thread (1–3 ms, ~10 ms with a repack) | Background remesh jobs if brushes grow |
| Each held-brush application is its own undo step | Group a stroke into one record (`applyEdit`'s `join`) |
| One remote command per frame; the window must not be minimized | Drain several per frame if Claude batches |
| A long remote `path` (and its undo) remeshes its whole bounding box | Remesh per op / record part |
| Far prefab moves are over the budget (one bounding box is remeshed) | The same: remesh per record part |
| `instanceAt` scans every instance's cells (cached per aimed voxel) | A per-chunk index of instance cells if there are thousands |
| At most 256 NPCs from spawn markers, all simulated every frame | Simulate only NPCs near the player (M1 streaming), then raise the cap |
| Overlapping instances can leave orphaned voxels when removed oldest first | Track which instance owns each voxel |
| One draw call per chunk | `SDL_DrawGPUPrimitivesIndirect` with a GPU-culled draw list |
| MSL-only shaders (macOS) | HLSL/Slang + SDL_shadercross for SPIR-V/DXIL (M1) |
| No shadows: sun light is gated by sky light, which has no direction (wrong under overhangs at low sun) | Cascaded shadow map for sun/moon (M1) |
| Stepped per-face light (one value per quad, Minecraft's "smooth lighting off"); models get one value | Player-centred RG8 3D light texture sampled per fragment, or per-corner light in a third quad word |
| Relighting recomputes whole columns (all 192 heights) around an edit | BFS add / two-phase removal on the stored light if edits get big or frequent |
| A 128³ Ember fill (the biggest `fill`, now that it takes any block) stalls the game ~130–250 ms, mostly relighting 2M emitters (the same box of Stone ~60 ms, Air 30–50 ms; M4, release, story world); the command budget doesn't count relight | Count emitters in the budget, or the BFS relight above |
| Quest triggers and lines name markers by name, checked for uniqueness only on world load | Stable alias ids on markers if names keep colliding |
| A trigger whose reward doesn't fit waits for room (the player destroys something with Ctrl+right-click) | Ground items to drop it on |
| `pickLine` scans every quest line on each talk | Index lines by spawn name if there are thousands |
| Absolute world-space float positions (jitter far from the origin) | Camera-relative rendering: subtract the camera's chunk origin on the CPU (needed for M1 streaming) |
