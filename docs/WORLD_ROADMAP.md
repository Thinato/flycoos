# World roadmap

What the procedural world has and what's still to build: terrain, places, and the life in it. The
same rules as docs/COMBAT_ROADMAP.md: genre conventions are fair game; no other game's names, assets
or characters. Each item gets its own ADR when built (take the next free number in DECISIONS.md);
tick it here and point to the ADR. World generation is still a work in progress, so the open
questions below stay open until the world is far enough along to answer them.

**The story world is deprecated** (2026-11-11). `worlds/story.flyw` and its quests still load, but
nothing new is built for it. Combat is tested in the arena (ADR-054); play happens in the endless
world. It can be removed once nothing depends on it (tests, the MCP tools' defaults, docs/STORY.md).

## Done

- [x] The endless world: a wrapping 1024-wide window streamed around the player (ADR-031)
- [x] Terrain as a pure function of (x, z): seas, rivers and lakes; soft stone and snow lines (ADR-032)
- [x] Regions: forests, rocky highlands and plains, 250 to 500 blocks across (`Regions`, world.cpp)
- [x] Five tree species (ADR-029); grass and flowers (ADR-030); one colour per voxel (ADR-028)
- [x] A campfire (a rest point) in every 96x96 cell where the ground is dry, level and clear (ADR-047)
- [x] Prefabs and markers, for hand-placed places (ADR-014, ADR-015, ADR-021)

## Terrain and biomes

- [ ] More biomes: desert, tundra, swamp (a biome map over the current regions)
- [ ] Caves
- [ ] LOD for distant terrain

## Places

- [ ] Points of interest placed by the generator: ruins, camps, towers, barrows
- [ ] Villages and towns, with people and daily routines
- [ ] Dungeons, with a boss
- [ ] Roads between places

## Populations

- [ ] **The spawner** (moved here from the combat roadmap; it needs biomes and places to decide what
  lives where). Enemies appear at random out of the player's view and go when far away and unseen,
  so the world stays alive although nothing respawns (ADR-056).
  - A budget of living hostiles within a ring around the player. Spawn points at a minimum
    distance, out of sight, loaded, on dry ground.
  - The kind and group size from the region or biome (and the place: a bandit camp, a barrow).
  - Harder kinds further from the start.
  - Despawn the living when far away, unseen and not hunting.
  - Notes for whoever builds it:
    - `spawnGroup` (entity.cpp) already spawns a group of NPCs that aren't from a marker.
    - `Regions` is file-local in world.cpp; expose it under a new name (story.h has `regionAt`).
    - The sim has no camera: "out of view" has to come in through `SimInput` or use the player's
      facing plus `canSee`.
    - `buryDead` already removes corpses of spawned NPCs when the player is far away and can't see them.
    - Every entity is simulated each frame, so the spawned count needs a frame-time check.

## Quests

- [ ] Procedural quests, from the places and people the generator makes
- [ ] Factions and reputation

## Open questions

Decide these when world generation is far enough along to judge them in play.

- **Density:** how many hostiles around the player, and how far? A starting point: 12 to 16 living
  within ~130 blocks, spawning 70 to 120 blocks away, despawning past 160 and unseen.
- **Difficulty by distance:** only harder kinds further out (boar and wolves near the start, bandits
  past ~300 blocks, the chief and ghouls past ~600), or also scaled levels and stats? Creature levels
  are fixed per kind today; scaling matters once talents and levelling exist.
- **Cleared areas:** does an area whose enemies were killed stay empty for a while (a per-cell
  cooldown, ~10 minutes)? Saved, or lost on reload (the endless world itself isn't saved)?
- **Night:** ghouls only at night? More wolves at night? Spawn markers already have time windows
  (ADR-023).
- **Guards and villagers:** should guards turn on a player who attacks a villager?
- **Quest items:** should quest items be impossible to destroy? Destroying Torvald's helm today leaves
  the quest unfinishable, and nothing says so.
