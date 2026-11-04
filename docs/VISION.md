# Vision

**flycoos** (working title) is a voxel action-RPG. You explore a handcrafted-feeling procedural world
on foot, fight in real time, and grow a character over a long journey. It sits in the voxel
action-RPG genre that Cube World popularized. It borrows the *freedom* of open-world RPGs such as
Skyrim and the *structure and social play* of MMOs such as World of Warcraft.

## Pillars

1. **Explore anything you can see.** The world is continuous and has no loading screens. Mountains,
   lakes, forests and ruins are reachable. Exploration is rewarded with places, loot and stories.
2. **Action first.** Combat is real-time and skill-based (dodge, timing, positioning), readable at a
   glance in a chunky voxel style.
3. **A world with memory.** Inspired by open-world RPGs: towns, factions, quests that change the
   world, and NPCs with routines. Skills improve the more you use them.
4. **Roles and teamwork.** Inspired by MMOs: WoW-style attributes and roles you build through gear
   and talents rather than a fixed class, group dungeons and bosses, and gear progression that
   rewards playing together.
5. **Readable voxel art.** Every object is made of flat-colored voxels, lit softly (ambient
   occlusion, warm sun, cool shadows). The art is distinct from any existing game. Colour rules:
   [ART.md](ART.md).

## What we take from each inspiration

| From | We take | We do **not** take |
|---|---|---|
| Voxel action-RPG genre (e.g. Cube World) | Colored-voxel look, procedural world, floating-limb characters, exploration loop | Its names, classes, creatures, character designs, UI, assets or code |
| Skyrim-like open-world RPGs | Freeform exploration, quests and factions, learn-by-doing skills, dialogue | Lore, races, places or names |
| WoW-like MMOs | Attributes (stamina, strength, agility, intellect, spirit), roles (tank/healer/damage), talent trees, dungeons/raids, group play | Lore, races, places, names or UI |

## IP guardrail

flycoos is an **original game**. Genre conventions are fair game; specific expression is not. We
never ship assets, names, character designs, creature designs, class names, maps, music, UI layouts
or code from Cube World, Skyrim, World of Warcraft or any other game. All art is generated in code
or made by us. When a feature is inspired by an existing game, we design our own version and document
it as ours.

## Milestone 0 (this repository)

A walking voxel adventurer (original design) in a 1024×192×1024-block procedural world, with forests,
lakes, beaches, mountains, a day/night cycle and distance fog. See [ROADMAP.md](ROADMAP.md) for what
comes next.

## Non-goals (for now)

- Photorealism or texture-mapped blocks: the look is flat voxel color.
- Minecraft-style block building as a core loop. Terrain editing may arrive as a tool, not a pillar.
- Mobile/console targets. Desktop (macOS first, then Windows/Linux) only.
