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

## M1: A living world

- Infinite world: chunk streaming around the player and background meshing (job system)
- Biomes (grassland, desert, tundra, swamp), rivers, caves; LOD for distant terrain
- Shadow maps for sun and moon; clouds; wind-animated foliage
- Ambient audio (wind, birds by day, crickets by night) and footsteps
- Cross-platform shaders (HLSL + SDL_shadercross, or Slang), plus Windows and Linux builds

## M2: Action combat

- Melee and ranged attacks, dodge roll, stamina; hit reactions and knockback
- First enemies with simple AI (wander, aggro, chase, attack)
- Health/mana/stamina HUD; death and respawn
- Animation layer: blend procedural locomotion with authored attack poses

## M3: Character and progression

- Classes with distinct roles (protector, healer, damage). Original designs, see VISION.md
- Talent choices per class; skills that level with use
- Loot, inventory, equipment that changes the voxel model, crafting basics

## M4: World with memory

- Towns with NPCs and daily routines; dialogue
- Quest system (data-driven), factions and reputation
- Points of interest: ruins, camps, a first hand-designed dungeon with a boss

## M5: Together

- Authoritative server and client prediction; co-op for 2–4 players first
- Group dungeons, party UI, shared quests
- Persistence (characters, world changes); groundwork for larger shared worlds
