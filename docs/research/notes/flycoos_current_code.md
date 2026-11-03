# flycoos: current code organization (internal source, as of commit 1354f1a + uncommitted ROADMAP.md edit)

Sources are repository files cited as `path:line` (relative to `/Users/paulolisecki/repos/thinato/flycoos`). Read-only investigation; nothing was built or run.

Size snapshot (`wc -l`): `src/` = 6,476 lines in 30 files; `tests/core_test.cpp` = 2,233; `tools/flycoos_mcp.py` = 301, `tools/test_mcp.py` = 282; `shaders/world.metal` = 187; docs: ARCHITECTURE 604, DECISIONS 371, ROADMAP 96. Note `docs/ARCHITECTURE.md:3` still says "about 1,200 lines of C++ (including tests)" — stale by ~7x.

## 1. Module map, dependency graph, cycles and layering violations

### Takeaway
The build has two layers — `flycoos_core` (static lib: world, mesher, player, entity, item, edit, prefab, commands; no SDL/GPU) and the `flycoos` executable (main, renderer, debug_ui, hud, inventory_ui, editor_ui, server) — and there are no `#include` cycles. The pain is *conceptual* layering: `world.h` owns story/NPC enums (EntityKind, Marker), `player.h` mixes physics, combat state, inventory and the character-model catalog, `renderer.h` depends on `player.h` for the model enum, and `main.cpp`'s `run()` is a ~500-line function holding all game state in locals and lambdas.

### Cited Findings
**Build layering**
- `flycoos_core` = world, mesher, player, entity, item, edit, commands, prefab; exe = main, renderer, debug_ui, hud, inventory_ui, editor_ui, server; `core_test` links only `flycoos_core` — [CMakeLists.txt:45-55, 64-67](CMakeLists.txt)
- Core lib publicly includes glm and nlohmann/json; stb and FastNoiseLite private — [CMakeLists.txt:47-50](CMakeLists.txt)
- ARCHITECTURE's module table (one row per file) — [docs/ARCHITECTURE.md:28-45](docs/ARCHITECTURE.md)

**Per-file map (lines, responsibility, main symbols, project includes)**

| File | Lines | Responsibility / main symbols | Includes (project) |
|---|---|---|---|
| `src/world.h` / `.cpp` | 184 / 629 | `Block` enum + `kBlockNames` + `kPalette` (world.h:12-62), `Grid` dense volume (71-82), `kWorldSize` 1024×192×1024, `parallelFor`, `generateTerrain` (world.cpp:147), `grow{Tree,Rock,Pine,Bush}` (60-127), `findSpawn`, `Instance` (world.h:110-122), **`EntityKind` + `kEntityKindNames`** (131-132), **`MarkerKind`/`Marker`/`checkMarker`/`regionAt`** (136-175), **world file save/load incl. INST/MARK/LINE sections** (world.cpp:290-629) | glm only |
| `src/mesher.h` / `.cpp` | 30 / 92 | `Quad` 8-byte packing, `meshRegion` greedy mesher + AO | world.h |
| `src/edit.h` / `.cpp` | 113 / 326 | `Box`, `Tool`, `EditOp`, `EditRecord` (carries voxel parts + instance + marker diffs, edit.h:49-60), `applyEdit`/`undoEdit`/`redoEdit`, `putMarker`/`removeMarker`, `raycast`, `groundTop` | world.h |
| `src/prefab.h` / `.cpp` | 72 / 335 | `Prefab`, `PrefabLibrary`, `parsePrefab` (text files), `loadPrefabs`, `placePrefab`/`removePrefab`/`movePrefab`, `instanceAt` | edit.h |
| `src/commands.h` / `.cpp` | 50 / 688 | `runCommand` JSON dispatcher (string if-chain at commands.cpp:514-688: state, apply, path, undo/redo, save, prefabs, place, remove, move, instances, markers, mark/edit_marker, unmark, heights, inspect, map, screenshot, goto), PNG map, budgets | prefab.h, **entity.h** (cpp:17) |
| `src/stats.h` | 48 | `Stats` (5 attributes), `Attack`, `kPunch`, `maxHealth`, `swingTime`, `rollDamage`, `armorReduction`, `mitigate` (header-only) | – |
| `src/item.h` / `.cpp` | 57 / 113 | `EquipSlot`, `ItemId` enum, `ItemType`, `kItems` table, `Inventory`, `SlotRef`, `moveItem`/`quickMove`/`giveItem`, `armor()`, `weaponAttack`/`mainAttack` | stats.h |
| `src/player.h` / `.cpp` | 90 / 474 | `Input`, `PlayerTuning`, **`Player`** (physics + animation + combat timers + `Stats` + `Inventory`, player.h:29-57), `updatePlayer` physics (cpp:51), `cameraClearance` (117), **`Model` enum + `Part` enum** (h:74-80), `buildPart` voxel model painters (cpp:134-352), `partModels`, `heldModel`, `poseModel` procedural animation (cpp:354-474) | item.h, stats.h, world.h |
| `src/entity.h` / `.cpp` | 103 / 429 | `Temper`, `EntityType`, `kTypes`, `Entity`, spawning (`syncSpawns`, `defaultMarkers`, `playerSpawn`), AI (`updateEntities`), talk/loot, combat (`hurt`, `damage`, `startPunch`, `inReach`, `punchTarget`, `updateCombat`) | player.h |
| `src/renderer.h` / `.cpp` | 110 / 590 | SDL3 GPU device, pipelines, per-chunk slots, `uploadWorld` (also *drives meshing*), `updateWorld`, `uploadModels` (builds all character parts via `buildPart`), `render`, `snapshot`; `ModelInstance`, `FrameParams`, `RenderSettings`, `RenderStats` | edit.h, mesher.h, **player.h** (for `Model`/`PartCount`) |
| `src/server.h` / `.cpp` | 35 / 129 | `CommandServer`: TCP thread, one-slot mailbox (`take`/`reply`) | – |
| `src/hud.h` / `.cpp` | 20 / 159 | `drawHud` (nameplates, balloons, damage numbers, player health), `drawPlaces`, `uiColor` | entity.h |
| `src/inventory_ui.h` / `.cpp` | 8 / 242 | Character (C) and Bags (B) ImGui windows, per-item icon drawing | player.h, hud.h |
| `src/editor_ui.h` / `.cpp` | 85 / 340 | `Editor` state struct (brush, undo history, prefab/marker tool state, MCP log, flash), `editorWindow`, `drawEditorOverlay` | prefab.h, **renderer.h** (for `RenderStats`), hud.h |
| `src/debug_ui.h` / `.cpp` | 56 / 264 | `DebugUI` (owns ImGui frame), `GameSettings`, `DebugActions`, tabs Render/Player/Items/Entities/Camera/World/Stats | entity.h, renderer.h |
| `src/main.cpp` | 605 | CLI options, world load/generate, `run()` main loop (87-588) with input routing, camera, editor tool handling, command side effects, respawns, benchmark | commands, debug_ui, editor_ui, entity, hud, inventory_ui, renderer, server |
| `tests/core_test.cpp` | 2,233 | 18 test functions with a `CHECK` macro | commands.h, edit.h, entity.h, mesher.h, prefab.h |
| `tools/flycoos_mcp.py` | 301 | stdio MCP server; `TOOLS` dict (line 112) maps each tool to one game command | – |
| `tools/test_mcp.py` | 282 | E2E: launches the game, drives MCP, needs a display | – |

— includes from `grep '#include' src/* tests/*` (e.g. [src/renderer.h:9-11](src/renderer.h), [src/commands.cpp:17](src/commands.cpp), [src/editor_ui.h:8-9](src/editor_ui.h), [src/debug_ui.h:9-10](src/debug_ui.h), [src/player.h:6-8](src/player.h), [src/entity.h:7](src/entity.h), [src/hud.h:6](src/hud.h), [src/inventory_ui.cpp:9](src/inventory_ui.cpp), [src/editor_ui.cpp:12](src/editor_ui.cpp))

**Dependency graph (project headers; arrows = "includes")**
```
                      main.cpp
   ┌──────┬──────┬───────┼────────┬──────────┬────────┬────────┐
commands debug_ui editor_ui entity   hud   inventory_ui renderer server
   │   \    │  \     │  \      │       │        │  \       │ \  \
prefab entity│ renderer prefab│  player◄─entity  player hud  edit mesher player
   │        entity  │   hud   │                                │     │
  edit           renderer     ▼                                world world
   │                    player ──► item ──► stats
  world                   ├──► stats
                          └──► world
```
(Linearized: stats ← item ← player ← entity ← {hud, debug_ui, commands.cpp}; world ← {mesher, edit, player}; edit ← {prefab, renderer}; prefab ← {commands, editor_ui}; renderer ← {editor_ui, debug_ui}; player ← renderer.)

- No `#include` cycles exist (all headers form a DAG via `#pragma once`) — derived from the include grep above.
- Conceptual cycle: `world.h` declares `EntityKind` "(their traits are in entity.cpp)" and validates it in `checkMarker` (`m.entity >= EntityKind::Count`), while entity.h depends on world.h — [src/world.h:130-132](src/world.h), [src/world.cpp:247](src/world.cpp)
- `world.h` also carries story concepts (`Marker`, `takesLines` hard-coding `EntityKind::TrainingDummy`) and `world.cpp` serializes instances, markers and dialogue lines — [src/world.h:136-175](src/world.h), [src/world.cpp:354-629](src/world.cpp)
- `edit.h`'s `EditRecord` knows `Instance` and `Marker`; `undoEdit`/`redoEdit` take the instance and marker lists — [src/edit.h:49-60, 80-81](src/edit.h)
- Renderer depends on simulation header `player.h` to get `Model`, `PartCount`; `uploadModels` builds every model via `buildPart` — [src/renderer.h:11-18, 109](src/renderer.h), [src/renderer.cpp:328](src/renderer.cpp)
- `player.cpp` (physics module) also contains the whole character-model catalog and animation; `heldModel` hard-codes `Knife` — [src/player.cpp:134-352, 242](src/player.cpp)
- `commands.cpp` (editing/command layer) includes entity.h only to read `typeOf(m.entity).wanderRadius` as a default radius — [src/commands.cpp:17, 503](src/commands.cpp)
- `editor_ui.h` includes `renderer.h` (SDL GPU types) just for `RenderStats` — [src/editor_ui.h:9, 80](src/editor_ui.h)
- `hud.cpp` uses `cameraClearance` (player.cpp) for nameplate line-of-sight and picks nameplate height per `Model` — [src/hud.cpp:81, 90](src/hud.cpp)
- `debug_ui` mutates simulation directly: `damage(e,10)`, `talk(e)`, toggles `e.angry`, `inv = Inventory{}` — [src/debug_ui.cpp:127-187](src/debug_ui.cpp)
- `main.cpp::run()` spans lines 87-588 (~500 lines) with world, instances, markers, player, entities, editor, camera, server and settings as locals and ~8 capturing lambdas (`buildWorld`, `save`, `edited`, `selectMarker`, `menus`, `updateMouseMode`, `setEditing`, `flyTo`, `pushModel`) — [src/main.cpp:87-235, 424](src/main.cpp)
- ARCHITECTURE's own "Known ceilings" lists "Meshing is driven by `Renderer::uploadWorld` → move meshing into a core job that produces `Mesh`; the renderer only uploads" — [docs/ARCHITECTURE.md:604](docs/ARCHITECTURE.md)

### Inferences
- The real module seams that already exist in code are: voxel world (Grid/terrain/mesher), world authoring (edit/prefab/markers/commands/file I/O), actor simulation (player/entity/combat/stats/items), presentation (renderer/models/animation/HUD/UI), and app shell (main/server). Today "markers + file I/O" sit in `world`, "models + animation" sit in `player`, and app orchestration sits in one function — these are the three biggest boundary mismatches.
- Simulation does not include rendering headers (good); the leak goes the other way (renderer → player.h for `Model`) and sideways (world.h owning story enums).
- Splitting `player.h` into body/physics, combat-state, and model/animation would remove renderer's dependency on simulation internals and let models become data.

### Gaps
- No runtime profiling or compile-time measurements were taken (read-only constraint); no data on incremental build times.
- `renderer.cpp`, `world.cpp` terrain internals, `prefab.cpp`, `editor_ui.cpp` and `mesher.cpp` were outlined by function signature, not read line by line.

## 2. Game data hard-coded in C++ and what could move to data files

### Takeaway
All gameplay content except structure prefabs (`prefabs/*.txt`) and authored world content (`worlds/story.flyw` markers/lines) is compiled in: entity kinds, items, names/phrases, palette, character models (procedural voxel painters), generated prefabs, default spawns, starting gear. Adding one NPC kind today touches ~8 places across 6 files plus the Python MCP schema.

### Cited Findings
- **Entity table `kTypes`**: 8 rows × 15 fields (name, level, 5 stats, attackable, wanders, talks, model, temper, aggro, wander radius, chase speed, weapon, loot, loot chance, respawn) — [src/entity.cpp:17-45](src/entity.cpp); struct at [src/entity.h:13-29](src/entity.h)
- **Villager names `kNames`** (20) and **phrases `kPhrases`** (10) — [src/entity.cpp:47-63](src/entity.cpp)
- **Default spawn offsets** (dummy {3,6}, villager {-4,7}, boar {9,11}, bandit {-5,22}) — [src/entity.cpp:173-176](src/entity.cpp)
- **Global AI/combat constants**: `kTalkRange` 3.5, `kPunchReach` 2, `kLeashRadius` 20, `kPlayerRespawnSeconds` 5 — [src/entity.h:55-58](src/entity.h); `kSpeechSeconds` 5, `kShoutSeconds` 8, `kBodyRadius` 0.8 — [src/entity.cpp:13-15](src/entity.cpp); stats/armor constants — [src/stats.h:16-23, 39-40](src/stats.h); `kDayLength` 4320 — [src/main.cpp:26](src/main.cpp)
- **Items `kItems`**: 12 rows (name, slot, attack, bag slots, armor) indexed by `enum ItemId : uint8_t` — [src/item.cpp:7-21](src/item.cpp), [src/item.h:13-22](src/item.h); `kSlotNames` — [src/item.cpp:3](src/item.cpp)
- **Item icons** are an ImGui-shape `switch (id)` per item — [src/inventory_ui.cpp:17-103](src/inventory_ui.cpp)
- **Item → model mapping** hard-coded: `heldModel` (Knife → ModelKnife else ModelSword) and `partModels` (IronHelm/Cuirass/Gauntlets/Sabatons → ModelIronArmor per part) — [src/player.cpp:242-253](src/player.cpp)
- **Character models**: `enum Model` (12 models) — [src/player.h:79-80](src/player.h); part-size rigs `kHumanoid`, `kDummy`, `kBoar`, `kWolf` — [src/player.cpp:136-171](src/player.cpp); per-model voxel painter functions (`wolfVoxel`, `boarVoxel`, `dummyVoxel`, `armorVoxel`, `guardVoxel`, `chiefVoxel`, `ghoulVoxel`, `buildSword`, `buildKnife`) and an if-chain in `buildPart` — [src/player.cpp:175-352](src/player.cpp); per-model draw scale (chief 1.25×, ghoul 0.82×) in `poseModel` — [src/player.cpp:456](src/player.cpp); nameplate height per model in HUD — [src/hud.cpp:81](src/hud.cpp)
- **Palette and block names**: 32 blocks, `kBlockNames`, `kPalette` sRGB; block ids saved in files ("only append"); Water id duplicated as `kWater` in the shader — [src/world.h:10-62](src/world.h)
- **Material pick lists duplicated** in commands.cpp and editor_ui.cpp — [src/commands.cpp:38](src/commands.cpp), [src/editor_ui.cpp:18-19](src/editor_ui.cpp); and again in Python `SOLID` — [tools/flycoos_mcp.py:23](tools/flycoos_mcp.py)
- **Entity kind names duplicated** in Python `ENTITIES` and the tool descriptions restate per-kind traits (radius defaults, levels) — [tools/flycoos_mcp.py:63-80](tools/flycoos_mcp.py)
- **Generated prefabs** (tree, pine, bush, boulder, spire) declared in `generatedPrefabs()`; `prefabBox` has per-name `if (p.name == "tree") … "spire"` branches — [src/prefab.cpp:13-30, 241-252](src/prefab.cpp)
- **Structure prefabs are already data**: 18 text files in `prefabs/` (barrow_entrance, bridge, camp, campfire, cottage, dock, fence, forge, gate, longhouse, market_stall, palisade, ruin, shrine, standing_stone, tent, watchtower, well), strictly parsed — [src/prefab.cpp:102-224](src/prefab.cpp), ADR-014 [docs/DECISIONS.md:269-300](docs/DECISIONS.md)
- **Story content is already data in the world file**: markers (spawn kind/count/radius) and dialogue lines (LINE section) — ADR-015/016 [docs/DECISIONS.md:302-371](docs/DECISIONS.md)
- **Starting inventory** (Shortsword, Satchel) in main — [src/main.cpp:162-163](src/main.cpp); debug "Give Iron set" list — [src/debug_ui.cpp:178](src/debug_ui.cpp)
- **Terrain generation parameters** (noise frequencies, tree/rock densities, heights) inline in `generateTerrain` — [src/world.cpp:147-208](src/world.cpp), described at [docs/ARCHITECTURE.md:51-70](docs/ARCHITECTURE.md)
- Test pins table content (e.g. `storyNpcs` re-states level/temper/model/weapon/loot of 4 kinds; item table rule "chest and legs give most armor") — [tests/core_test.cpp:1813-1830](tests/core_test.cpp), [docs/DECISIONS.md:158-159](docs/DECISIONS.md)

**Touch-list for adding one NPC kind today** (derived from the above): `EntityKind` + `kEntityKindNames` (world.h:131-132), `kTypes` row (entity.cpp), new `Model` enum value (player.h:79), part-spec/painter + `buildPart` branch (+ `quadruped()`/`specs()` if non-humanoid) (player.cpp), nameplate height (hud.cpp:81), new loot `ItemId` + `kItems` row + icon case (item.h/item.cpp/inventory_ui.cpp), Python `ENTITIES` + description text (flycoos_mcp.py:63-80). Commit history matches this: "Act 1" touched 36 files (+1,741) — `git log --shortstat`.

### Inferences
- Strong data-file candidates: `kTypes` (+ names/phrases/behavior params), `kItems` (+ icon description, held/worn model mapping), part rigs and model painters (as voxel files or a part-spec format similar to `prefabs/*.txt`), palette + block names, generated-prefab parameter declarations, default spawns, starting gear, global tuning constants.
- Enum-indexed ids that are *saved in world files* (`Block`, `EntityKind`, `MarkerKind`) are a constraint: moving them to data needs a stable string-or-id mapping so existing `.flyw` files keep loading (they currently write `u8 entity` in MARK).
- The model painters are procedural code (conditions on voxel coordinates), so "move to data" for models means either authoring voxel files or a small declarative shape spec; that's a bigger change than the tables.

### Gaps
- Didn't read the full `inventory_ui.cpp` icon switch (only lines 17-40) or every voxel painter; exact item coverage of icons (e.g. whether every ItemId has an icon) unverified.
- The README's structure-file format was not read.

## 3. Entity behavior, combat, inventory/equipment, stats — and their coupling

### Takeaway
Behavior is a flat per-frame function (`updateEntities`) branching on a per-kind `Temper` plus a single `angry` bool (ADR-009 chose this over a state machine); combat is a separate pass (`updateCombat`) where the player hits entities and entities hit only the player. Every NPC body *is* a `Player` struct (physics, animation, tuning incl. fly/noclip, stats, full bag inventory), so player and NPC share all physics/combat code but also all player-only baggage, and the player itself has no "entity" identity (no level, no kind, respawn logic in main.cpp).

### Cited Findings
**Data shapes**
- `Player` = pos/vel/yaw/onGround/inWater + gait/air/breath animation + `PlayerTuning` (walk/run/jump/gravity/fly/noclip) + `Stats` + health/combat/swing/punch timers + `swingWeapon` + `hitTime`/`lastDamage`/`deadTime` + `Inventory inv` — [src/player.h:29-57](src/player.h)
- "A walking body: the player, and the body of every NPC (driven by AI input instead of keys)" — [src/player.h:28](src/player.h)
- `Entity` = kind, name, level, `Player body`, home, wanderRadius, `Marker spawn` copy, wanderDir/Timer, phrase, speechTimer, shoutTimer, `angry`, loot, own `std::mt19937 rng` — [src/entity.h:32-53](src/entity.h)
- `makeEntity` writes into the body: stats, health, `tuning.walkSpeed = 2`, `tuning.runSpeed = chaseSpeed`, `inv.equipped[SlotMainHand] = weapon` — [src/entity.cpp:120-138](src/entity.cpp)
- ADR-008: "Equipment and bags live on the body (`Player::inv`), so NPCs can carry gear later" — [docs/DECISIONS.md:122](docs/DECISIONS.md)

**Behavior (`updateEntities`, entity.cpp:240-273)** — one loop per entity per frame:
- Timers tick; body unburied with `clearOfTerrain`; non-wanderers (`!t.wanders`, i.e. the dummy) skip AI — [src/entity.cpp:242-246](src/entity.cpp)
- Territory/leash: `inTerritory = player.alive() && distance(player, home) < kLeashRadius` — leash measured home→player, not home→NPC — [src/entity.cpp:252](src/entity.cpp); ADR-009 [docs/DECISIONS.md:141-142](docs/DECISIONS.md)
- Aggro: `Aggressive && inTerritory && dist < aggroRange → anger(e)`; out of territory or dead → `angry = false` — [src/entity.cpp:253-254](src/entity.cpp)
- Priority chain: dead (lie) → angry (chase if `walkable`, else turn; `startPunch` when `dist < kPunchReach - 0.2`) → talking (stand, face player, unless shouting) → `wander` — [src/entity.cpp:255-270](src/entity.cpp); "ponytail: no pathfinding" — [src/entity.cpp:261](src/entity.cpp)
- `wander`: 40% stand 2–6 s / 60% stroll 2–5 s random dir; head home beyond wanderRadius; `walkable` probe (walls, water, 2+ drops, the player) — [src/entity.cpp:71-107](src/entity.cpp)
- Shout: `anger()` says next marker line on turning angry, rate-limited by `kShoutSeconds` — [src/entity.cpp:111-117](src/entity.cpp)
- Talk: marker lines cycle, else random `kPhrases` (never same twice) — [src/entity.cpp:301-313](src/entity.cpp); `talkTarget`/`lootTarget` nearest within 3.5 — [src/entity.cpp:288-327](src/entity.cpp)
- Physics: NPC calls the same `updatePlayer(e.body, world, in, dt)` with AI-built `Input` — [src/entity.cpp:271](src/entity.cpp)
- Separation pushes only the player out of NPCs — [src/entity.cpp:275-286](src/entity.cpp)
- Spawning is derived from markers: `syncSpawns` diffs entities against spawn markers and respawns stale ones — [src/entity.cpp:202-225](src/entity.cpp); ADR-015 [docs/DECISIONS.md:311-313](docs/DECISIONS.md)

**Combat (`updateCombat`, entity.cpp:409-429)**
- `startPunch`: needs alive + `swingTimer <= 0`; latches `swingWeapon`; `swingTimer += swingTime(stats, weaponAttack)`; animation length `min(kPunchSeconds 0.35, swingTimer)`; alternates hands — [src/entity.cpp:352-362](src/entity.cpp)
- Impact frame: `lands()` true once when `punchTime >= kPunchImpact (0.4) × punchLength` — [src/entity.cpp:397-402](src/entity.cpp), [src/player.h:59-60](src/player.h)
- `inReach`: < 2 blocks, within 60° of facing, |dy| < body height — [src/entity.cpp:364-370](src/entity.cpp)
- `roll()` = `mitigate(rollDamage(stats, weaponAttack(swingWeapon)), armor(victim.inv), attackerLevel, armorK)` — [src/entity.cpp:405-407](src/entity.cpp); formulas in [src/stats.h:25-48](src/stats.h)
- Player's attacker level hard-coded to 1 ("the player has no level yet") — [src/entity.cpp:411](src/entity.cpp)
- Asymmetric: player → `punchTarget(entities)`; each entity → only `player` — [src/entity.cpp:411-418](src/entity.cpp); ADR-009 "Only the player can be a target; NPCs don't fight each other" — [docs/DECISIONS.md:150](docs/DECISIONS.md)
- `hurt(Player&)` for any body; `damage(Entity&)` adds attackable check, anger, loot roll — [src/entity.cpp:335-350](src/entity.cpp)
- `tick()` handles swing/combat timers and spirit regen out of combat — [src/entity.cpp:385-394](src/entity.cpp)
- NPC respawn (home, full health, after `respawnSeconds`) is inside `updateCombat` — [src/entity.cpp:419-427](src/entity.cpp); the *player's* respawn is in main.cpp — [src/main.cpp:549-556](src/main.cpp)
- `armorK` threaded from `GameSettings` through `updateCombat` (not a global) — ADR-010 [docs/DECISIONS.md:163-164](docs/DECISIONS.md), [src/main.cpp:340](src/main.cpp)
- Animation reads combat state directly (`punchTime`, `swingWeapon`, `deadTime`, `hitTime`) in `poseModel` — [src/player.cpp:429-474](src/player.cpp)

**Inventory/equipment/stats**
- `Inventory`: 7 equip slots, 4 bag-bar slots, backpack(12) + one container per bag — [src/item.h:29-33](src/item.h); `moveItem` single move rule — [src/item.cpp:50-~70](src/item.cpp)
- Gear adds no attributes; health/regen/swing read `Player::stats` directly; ADR-008/010 note "must read base + gear stats" later — [docs/DECISIONS.md:129-130, 171-172](docs/DECISIONS.md)
- NPCs wear nothing (only a main-hand weapon), so armor only protects the player — [docs/DECISIONS.md:169-170](docs/DECISIONS.md)
- Bone Charm has `SlotNone` ("a trinket; no slot for it yet") — [src/item.cpp:20](src/item.cpp)

### Inferences
- The `Temper` + `angry` design is effectively an implicit 4-state machine (dead / angry-chase / talking / wandering) encoded as an if-chain; adding states (flee, patrol, schedule, night-only, ranged) would grow that chain. ADR-009 explicitly chose "instead of a state machine", so a refactor reverses an accepted ADR and should supersede it.
- Combat is coupled to "the player" as a privileged parameter (`updateCombat(Player& player, entities…)`), blocking NPC-vs-NPC, guards defending villagers, or multiple players (M5 co-op).
- `Player` as NPC body carries `PlayerTuning.fly/noclip` and a full bag inventory for every NPC; a split into Body (physics), Combatant (stats/health/timers/equipment), Animator state, and Controller (player input vs AI) would match how the code is actually used.
- Leveling (backlog) will need `level` on the player body; today it lives on `Entity` and is passed separately into `roll`.

### Gaps
- Didn't measure per-frame cost of `updateEntities` with 256 NPCs.
- The exact `quickMove` / bag rules beyond item.cpp:60 weren't read.

## 4. Game loop, timestep, input, editor, command server, HUD and debug UI wiring in main.cpp

### Takeaway
One variable-`dt` loop (no fixed timestep, no dt clamp) in `run()` does, in order: events → input → player physics → NPC AI/separation/combat (skipped in editor) → time of day → camera → editor tool clicks → model posing → one remote command → UI (HUD/editor/inventory/debug) → render → deferred editor/debug actions → respawns. State flows through captured locals and three "action" structs (`EditorActions`, `DebugActions`, `Command`) that UI and the command server fill and main executes.

### Cited Findings
- `dt` = wall-clock ns delta per frame, used directly for physics, AI, combat and time of day; no clamp or accumulator — [src/main.cpp:249-253, 336-345](src/main.cpp)
- Physics sub-steps moves of ≤ 0.4 blocks and `onGround` is probed to be frame-rate independent ("tested at 1000 Hz") — [docs/ARCHITECTURE.md:385-389](docs/ARCHITECTURE.md)
- Minimized window: loop waits on events and doesn't advance — [src/main.cpp:242-248](src/main.cpp)
- Event handling is one big if/else on keys: `.` debug, C character, B bags, E loot-else-talk, Tab editor, Q editor cursor, 1–8 tools, R rotate, Del remove/unmark, [ ] brush radius, Esc layered close/quit, Ctrl/Cmd Z/Y/S — [src/main.cpp:255-313](src/main.cpp)
- `menus()` lambda: any window open frees the cursor and blocks mouse look/attacks — [src/main.cpp:206-207, 305-310, 333](src/main.cpp)
- Input built from keyboard state into `Input`; benchmark overrides movement; knocked out zeroes input — [src/main.cpp:315-330](src/main.cpp)
- Attack: `startPunch(player, forward)` from held/queued LMB — [src/main.cpp:331-335](src/main.cpp)
- Simulation step: `updatePlayer` always; `updateEntities`, `separate`, `updateCombat` only when `!editor.active` — [src/main.cpp:336-341](src/main.cpp)
- Editor mode = player flies with noclip (saves/restores tuning) and NPCs/combat pause — [src/main.cpp:209-223](src/main.cpp)
- Orbit camera with `cameraClearance` box-march — [src/main.cpp:347-360](src/main.cpp)
- Editor tool handling inline in main (raycast, marker place/select, prefab place/select, Fill corners, brush rate limit, Flatten height) ~60 lines — [src/main.cpp:362-423](src/main.cpp)
- `edited(dirty,…)` lambda is the single post-edit hook: remesh `renderer.updateWorld`, `syncSpawns`, unsaved flag, edit counter — [src/main.cpp:168-178](src/main.cpp)
- Models for the frame: `pushModel` poses player + NPCs (+ held weapon) into `FrameParams.models` — [src/main.cpp:424-434](src/main.cpp)
- Command server: one request per frame via `server.take()`; `runCommand` returns `Command` (response, dirty, save?, shot?, go?) and main performs the side effects (remesh, save, offscreen `renderer.snapshot` + PNG, `flyTo`), logs to `editor.log`, replies — [src/main.cpp:436-475](src/main.cpp); thread model in [src/server.h:9-35](src/server.h), ADR-013 [docs/DECISIONS.md:228-266](docs/DECISIONS.md)
- After commands: marker panel resync, region-enter timer, `ui.region` — [src/main.cpp:477-483](src/main.cpp)
- UI: `ui.begin` → `drawHud` + `drawPlaces` → `drawEditorOverlay` → `characterWindow` / `bagsWindow` → `editorWindow` → `ui.end` (debug window) → `renderer.render` — [src/main.cpp:484-498](src/main.cpp); DebugUI owns the ImGui frame — [src/debug_ui.h:27-45](src/debug_ui.h)
- Deferred actions applied *after* render: undo/redo, prefab remove, go-to, unmark, update marker, save, regenerate, player respawn (`deadTime >= kPlayerRespawnSeconds`), respawn NPCs — [src/main.cpp:500-561](src/main.cpp)
- Game settings knobs (`GameSettings`: fov, camera, hud, timeOfDay, timeScale, armorK, seed) and `DebugActions` — [src/debug_ui.h:12-25](src/debug_ui.h)
- Window title / benchmark stats — [src/main.cpp:563-587](src/main.cpp)
- Damage RNG is a main-local `std::mt19937` seeded from `random_device`; each NPC has its own seeded rng — [src/main.cpp:192](src/main.cpp), [src/entity.h:47](src/entity.h)

### Inferences
- Because `dt` is variable and the damage RNG is non-deterministically seeded, the simulation is not reproducible run-to-run; a fixed-step sim (with render interpolation) would also be a prerequisite for M5's "authoritative server and client prediction" ([docs/ROADMAP.md:75](docs/ROADMAP.md)).
- Editor actions from the UI execute after `render`, so their result shows one frame later — harmless, but evidence that ordering is incidental rather than designed.
- main.cpp currently plays four roles — app shell (window/CLI/benchmark), input mapping, game-session orchestration (sim step, respawn, starting gear), editor controller (tool clicks, marker panel sync, command side effects). These are natural extraction targets (e.g. a `Game`/`Session` struct holding world+instances+markers+player+entities+history, an `EditorController`, an input-to-intent mapper).
- `Editor` struct mixes persistent world state (`history`, `prefabs` library, unsaved/path) with panel UI state (char buffers, rings cache, log); the undo history belongs to the session, not the panel.

### Gaps
- No measurement of how often frame `dt` spikes (e.g. during a 20 ms screenshot or a ~10 ms repack) and what that does to physics/AI; the code doesn't clamp, but the effect wasn't tested.

## 5. Tests: structure, coverage, and what's untestable today

### Takeaway
One 2,233-line executable with 18 hand-rolled test functions and ~534 `CHECK`s covers the core library well (meshing, physics, generation determinism, entities, combat, balance fights, items, armor, world files, editing/undo, the full command dispatcher, prefabs, markers, lines, raycast). Everything in `main.cpp`, the UI, HUD, renderer and server is untested by `core_test` (the server only by a display-requiring Python E2E script).

### Cited Findings
- Custom `CHECK` macro (not `<cassert>` because release builds define NDEBUG), failure count as exit code — [tests/core_test.cpp:1, 21-28, 2211-2232](tests/core_test.cpp)
- Test functions and their line ranges: `meshing` 39, `packing` 81, `physics` 100, `generation` 223, `regions` 237, `entities` 279, `combat` 349, `fighters` 467, `items` 617, `protection` 710, `worldFiles` 789, `editing` 868, `commands` 991 (~230 lines), `prefabs` 1219 (~290), `markers` 1508 (~300), `storyNpcs` 1813, `markerLines` 1970 (~200), `raycasting` 2171 — [tests/core_test.cpp](tests/core_test.cpp) (grep of `static void`)
- 535 lines match `CHECK|expect(|assert(` (one is the macro definition) — grep count on tests/core_test.cpp
- `storyNpcs` pins table rows (level, temper, model, weapon, loot) for 4 kinds, model shapes via `buildPart`, `poseModel` outputs, and simulates fights headless to check balance — [tests/core_test.cpp:1813-1860+](tests/core_test.cpp); ADR-016 "A test simulates these fights" — [docs/DECISIONS.md:353](docs/DECISIONS.md)
- Terrain determinism pinned by hashes; prefabs must keep terrain bit-identical — [docs/ARCHITECTURE.md:69, 142-143](docs/ARCHITECTURE.md)
- Shipped prefab files checked via `FLYCOOS_SOURCE_DIR` — [CMakeLists.txt:66](CMakeLists.txt)
- `runCommand` is designed to be testable "without sockets or GPU" — [src/commands.h:10-13](src/commands.h), ADR-013 [docs/DECISIONS.md:239-241](docs/DECISIONS.md)
- E2E: `tools/test_mcp.py` starts the game on a scratch world, drives MCP, sends hostile input; "Needs a display" — [tools/test_mcp.py:2-8](tools/test_mcp.py)
- Shader errors only show at runtime; `--frames` smoke run catches them — ADR-002 [docs/DECISIONS.md:33-34](docs/DECISIONS.md)
- User workflow memory: no CI (MEMORY.md "no CI").

**Untestable today (lives in main.cpp/UI with no seam):**
- Input → intent mapping and the Esc/menus/cursor rules — [src/main.cpp:255-335](src/main.cpp)
- Editor tool click logic (marker select-or-place radius 1.5/height 3, prefab select-or-place, Fill corners, Flatten-at-stroke-start) — [src/main.cpp:362-423](src/main.cpp)
- Command side effects (save-to-path response rewrite, screenshot, goto/follow), log — [src/main.cpp:436-475](src/main.cpp)
- Player respawn, starting inventory, region-enter timing, regenerate flow — [src/main.cpp:160-163, 481-483, 539-560](src/main.cpp)
- Marker panel ↔ selected marker sync (`selectMarker`, `markedWas`) — [src/main.cpp:180-187, 477-480](src/main.cpp)
- HUD decisions embedded in drawing (nameplate color by temper, LOS hide, hint target choice) — [src/hud.cpp:72-135](src/hud.cpp)
- Inventory UI drag/drop is UI-only, but rules go through tested `moveItem`/`quickMove`.
- Whole-frame ordering (sim vs editor vs command) and frame-rate dependence of AI/combat timing.

### Inferences
- The core/exe split is the testability boundary; anything moved from `run()` into core-library functions (session step, editor controller, respawn) becomes testable with the existing harness.
- The single-file, single-binary test suite is already 2,233 lines; ~1/3 of the repo's C++. Splitting per module (still one executable, no framework) would ease navigation, consistent with "no frameworks" taste.
- Table-pinning tests (storyNpcs) will need to change if tables move to data files — they could instead load the shipped data and check invariants (as prefab files already are).

### Gaps
- No coverage tool was run; coverage claims are from reading test names and selected bodies, not line coverage.

## 6. Existing decisions and constraints a refactor must respect (ARCHITECTURE, DECISIONS, ROADMAP)

### Takeaway
16 ADRs fix the tech stack (C++23/CMake/SDL3 GPU/Metal, header-only deps via FetchContent, ImGui), the world/edit model (dense 1024×192×1024 grid, `.flyw` sectioned file with append-only ids, data-driven `EditOp` + snapshot undo, prefab instances, markers with derived NPCs), the MCP bridge contract, and the combat/stats formulas. The Backlog's first item is exactly this refactor: "data-driven entity definitions and behavior (state machines), clearer module boundaries".

### Cited Findings
- Backlog item 1: "Architecture refactor before the features below: data-driven entity definitions and behavior (state machines), clearer module boundaries (see the architecture research notes)"; followed by quests/journal (data-driven objectives and flags), leveling + talents, use-based skills, dynamic lighting, AO color ramping, time-of-day spawns, trinket slot + gear attributes, bigger palette, editor fixes, perf A/B — [docs/ROADMAP.md:79-96](docs/ROADMAP.md)
- Later milestones: M2 pathfinding/groups/ranged, animation layer; M3 talents/skills/magic, gear attributes, crafting; M4 routines, branching dialogue, data-driven quests, factions; M5 authoritative server + client prediction, co-op 2–4 — [docs/ROADMAP.md:54-77](docs/ROADMAP.md)
- ADR-001: C++23, CMake ≥ 3.28, Ninja, AppleClang, SDL3 GPU, glm, FastNoiseLite; "goal is C++ with full control" (no engine) — [docs/DECISIONS.md:3-24](docs/DECISIONS.md)
- ADR-002: runtime-compiled MSL, macOS-only shaders for now — [docs/DECISIONS.md:26-36](docs/DECISIONS.md)
- ADR-003/004/012: greedy meshing, 8-byte quads, vertex pulling, owned resolve target, per-chunk buffer slots — [docs/DECISIONS.md:38-66, 205-226](docs/DECISIONS.md)
- ADR-005/013: header-only deps fetched as pinned tarballs; nlohmann/json and stb added the same way — [docs/DECISIONS.md:68-76, 244-245](docs/DECISIONS.md) (so a JSON data-file loader has no new dependency cost)
- ADR-006/008: Dear ImGui for debug and game windows, icons drawn from shapes — [docs/DECISIONS.md:78-95, 125-126](docs/DECISIONS.md)
- ADR-007: five classless attributes; "All formulas and tuning constants live in `src/stats.h`" — [docs/DECISIONS.md:97-113](docs/DECISIONS.md)
- ADR-008: bags of fixed slots; equipment on the body; one move rule; gear must later read base + gear stats — [docs/DECISIONS.md:115-131](docs/DECISIONS.md)
- ADR-009: Temper + `angry` flag "instead of a state machine"; leash home→player; NPC attacks reuse player swing; "New fighters are table rows"; no pathfinding; only the player is a target — [docs/DECISIONS.md:133-150](docs/DECISIONS.md)
- ADR-010: armor formula, K passed into `updateCombat` (not a mutable global) — [docs/DECISIONS.md:152-173](docs/DECISIONS.md)
- ADR-011: versioned `.flyw`, tagged sections, unknown sections skipped, FNV-1a checksum, temp+rename save; `EditOp` single entry point; snapshot undo; caps — [docs/DECISIONS.md:175-203](docs/DECISIONS.md)
- Saved enums are append-only: `Block` ("only append", palette index, shader `kWater`), `EntityKind` ("Saved in spawn markers: only append"), `MarkerKind` — [src/world.h:10-11, 130-136](src/world.h)
- ADR-013: one command per frame on the main thread, no world lock; dispatcher in core lib, side effects returned as data; localhost-only, hostile-input hardening, ≤50 ms budget — [docs/DECISIONS.md:228-266](docs/DECISIONS.md)
- ADR-014: prefab instances recorded with before/after cells; generated prefabs must keep terrain bit-identical; palette full at 32 — [docs/DECISIONS.md:269-300](docs/DECISIONS.md)
- ADR-015: NPCs are derived state from markers via `syncSpawns`; undo/redo/commands never touch NPCs directly; ≤256 NPCs since all simulate every frame — [docs/DECISIONS.md:302-329](docs/DECISIONS.md)
- ADR-016: lines on spawn markers (LINE section), new kinds as `kTypes` rows, `takesLines` deliberately not tied to NPC traits so saved worlds keep loading when traits change; foes tuned for a level-1 player and pinned by a fight test — [docs/DECISIONS.md:331-371](docs/DECISIONS.md)
- Known ceilings table incl. 256-NPC cap, main-thread remesh, meshing driven by renderer, absolute float positions — [docs/ARCHITECTURE.md:587-604](docs/ARCHITECTURE.md)
- User workflow preferences (from memory index): commit only when asked, never push unasked, no CI, "Cube World-scale taste", an IP guardrail; ponytail/minimal-code style is visible in code comments (`ponytail:` markers at [src/world.h:65](src/world.h), [src/entity.cpp:261](src/entity.cpp)).

### Inferences
- A refactor must keep `.flyw` compatibility: marker `entity` is a `u8` index into `EntityKind`; if entity kinds move to data files, the file needs either a stable numeric id per kind in the data or a new section mapping names, without a version bump (ADR-011/016 precedent: add sections, never change `MARK`).
- ADR-009's "instead of a state machine" and "only the player can be a target" would be superseded by a behavior/state-machine refactor; it should be recorded as a new ADR that supersedes ADR-009.
- ADR-007's "all formulas live in stats.h" and ADR-010's "K passed in, not global" favor keeping formulas as pure functions in code while tables (numbers) move to data.
- ADR-013's main-thread, one-command-per-frame model and ADR-015's "NPCs derived from markers via one sync function" are already clean seams worth preserving.
- The "only append" enum rule and the full 32-color palette are hard constraints on anything that adds blocks or kinds.

### Gaps
- `docs/VISION.md` (48 lines) and `docs/STORY.md` (56 lines) were not read; they may add product constraints (e.g. which Act 1 features the backlog serves).
- The uncommitted `docs/ROADMAP.md` change (per git status) is what was read; its committed version may differ.
