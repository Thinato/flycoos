# Roadmap

Each milestone ends playable. Scope is deliberately narrow; later milestones get re-planned when we
reach them.

## M0: Foundation ✅ (this repository)

- C++23 / CMake / SDL3 GPU (Metal) project that builds with one command on macOS
- 1024×192×1024 procedural world made of large regions: tall forests, rocky highlands with spires,
  vast open plains, plus lakes, beaches, mountains and snow
- Greedy-meshed voxel chunks with ambient occlusion, vertex pulling, 4× MSAA, frustum culling
- Original voxel character with procedural walk, run, idle and jump animation
- Third-person orbit camera, collision with 1-block step-up, swimming
- Day/night cycle with sky gradient, sun, moon, stars and distance fog
- Debug window (`.`): mesh views, wireframe, culling tools, movement tuning, fly/noclip, time and
  world controls, frame-time graph
- Benchmark/screenshot mode and engine-core tests
- First NPCs: a training dummy (attackable, static) and a wandering villager you can talk to (E),
  with nameplates, health bars and speech balloons
- Five attributes (stamina, strength, agility, intellect, spirit) with derived health, damage, attack
  speed and regeneration; punching (left mouse) with damage numbers; the dummy topples and recovers
- Inventory and equipment: Character (C) and Bags (B) windows, a 12-slot backpack plus 4 bag slots,
  drag and drop; the first weapon, a one-handed sword with its own slash animation
- Passive and aggressive NPCs: a boar that fights back once hit and a knife-wielding bandit that
  attacks on sight (aggro, chase, leash); the player can be knocked out and respawns; corpses drop
  loot (boar skin, knife) looted with E
- Armor: worn gear reduces damage taken by armor / (armor + K x attacker level), K tunable in the
  debug window; an Iron armor set (helm, cuirass, gauntlets, greaves, sabatons) that shows on the
  character
- World editor groundwork: world files (save/load), an in-game editor (Tab) with terrain brushes,
  fill box and undo/redo, and incremental remeshing of edited chunks
- Shaping the world with Claude: a localhost command server in the game and a dependency-free MCP
  bridge (brushes, roads/paths, undo, save, offscreen screenshots, top-down maps, height queries,
  camera goto), with an MCP log and edit highlights in the Editor panel
- Prefabs: generated trees, pines, bushes, boulders and spires, and structures from text files
  (cottage, tent, bandit camp, well, fence, ruin), placed as instances that can be moved and
  removed (in-game Prefab tool and MCP tools), saved with the world
- Story markers: the player spawn, NPC spawns (kind, count, wander area; the NPCs follow every
  edit and undo), named points of interest and regions (shown as labels and an on-entering name),
  placed in-game (Marker tool) or by Claude (MCP), saved with the world and drawn on the map
- Story NPCs: dialogue lines on spawn markers (talkers say them on E, fighters when they attack; a
  lone NPC takes the marker's name), new kinds (wolf, guard, bandit chief, barrow ghoul) with Wolf
  Pelt and Bone Charm loot, and a 72-minute day

## M1: A living world

- An endless procedural world streamed around the player (ADR-031, 2026-11-07) ✅, the base for
  procedural quests, monsters and dungeons, so play doesn't wait on the story; the story world stays
- A hand-authored story world: the fixed-size 1024×192×1024 world, generated once and then shaped
  in the in-game editor (`--world worlds/story.flyw`). Story markers in the world file ✅.
  Deprecated 2026-11-11: play moves to the endless world, combat tests to the arena. The world's
  own plan is docs/WORLD_ROADMAP.md
- Seas, rivers and lakes at their own levels; soft stone and snow lines (ADR-032) ✅
- Biomes (grassland, desert, tundra, swamp), caves; LOD for distant terrain
- Shadow maps for sun and moon; clouds; wind-animated foliage
- Ambient audio (wind, birds by day, crickets by night) and footsteps
- Cross-platform shaders (HLSL + SDL_shadercross, or Slang), plus Windows and Linux builds

## M2: Action combat

- Weapon attacks and ranged attacks, dodge roll, an action resource for dodging; knockback
- More enemies and smarter AI (pathfinding around obstacles, groups, ranged attackers)
- Mana/stamina HUD; a proper death penalty and graveyard-style respawn
- Animation layer: blend procedural locomotion with authored attack poses

## M3: Character and progression

- No classes: roles (protector, healer, damage) come from attributes, gear and talents
- Talent choices; skills that level with use; intellect-based magic and spells
- Loot, armor and gear attributes, armor that changes the voxel model, crafting basics

## M4: World with memory

- Towns with NPCs and daily routines; branching dialogue (fixed lines per spawn marker ✅)
- Quest system (data-driven), factions and reputation
- Points of interest: ruins, camps, a first hand-designed dungeon with a boss

## M5: Together

- Authoritative server and client prediction; co-op for 2–4 players first
- Group dungeons, party UI, shared quests
- Persistence (characters, world changes); groundwork for larger shared worlds

## Backlog (TODO, collected 2026-11-03 after Act 1)

Not yet scheduled into a milestone; see docs/STORY.md for where Act 1 needs each one.

- [x] Architecture refactor before the features below: data-driven entity definitions and
      behavior (state machines), clearer module boundaries; plan in docs/research/Voxel RPG code architecture.md (11 steps)
- [x] Quests and a quest journal: data-driven objectives and flags (e.g. "bring Hakon's helm to
      Torvald"), dialogue that reacts to quest state (ADR-026)
- [x] Player leveling: XP, levels, a talent point per level, talent trees per ability (ADR-027)
- [x] Skills that improve with use (e.g. one-handed improves by fighting with a one-handed weapon) (ADR-027)
- [x] Dynamic lighting: Ember/torches/campfires emit light at night; dark interiors (barrow)
      (sky and block light, ADR-019; Lantern block)
- [x] Occlusion-based color ramping (ambient occlusion that darkens and shifts block colors):
      AO curve and a cool shadow tint, tunable in the debug Render tab
- [x] Time-of-day spawns (e.g. ghouls only at night): an active window on spawn markers (ADR-023)
- [x] Trinket slot (Bone Charm), and gear attributes on top of base stats (stat pipeline, ADR-022)
- [x] Bigger block palette: 64 colors, and Brick, MossyCobble, DarkPlanks, Hay, Clay, Slate and a
      Lantern (light 11) to build with (ADR-019)
- [x] Editor: `flatten` under tree canopies spreads Leaves; `fill` over MCP only takes natural
      blocks; prefab-placed markers (a camp that brings its own bandits) (ADR-021)
- [x] Re-run the render perf A/B once the 120 fps cap on the dev machine is lifted (2026-11-03:
      pre-refactor vs HEAD in docs/ARCHITECTURE.md, Performance)
