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

## M1: A living world

- A hand-authored story world: the fixed-size 1024×192×1024 world, generated once and then shaped
  in the in-game editor, replaces an infinite streamed one. Prefabs and story markers in the world
  file; remote editing commands so Claude can shape it too
- Biomes (grassland, desert, tundra, swamp), rivers, caves; LOD for distant terrain
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

- Towns with NPCs and daily routines; dialogue
- Quest system (data-driven), factions and reputation
- Points of interest: ruins, camps, a first hand-designed dungeon with a boss

## M5: Together

- Authoritative server and client prediction; co-op for 2–4 players first
- Group dungeons, party UI, shared quests
- Persistence (characters, world changes); groundwork for larger shared worlds
