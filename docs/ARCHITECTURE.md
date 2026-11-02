# Architecture

Milestone 0 is about 1,200 lines of C++ (including tests) and 160 lines of MSL. It has two layers:

- **`flycoos_core`** (static library: `world`, `mesher`, `player`): pure CPU code with no GPU or SDL
  dependency, unit-tested by `tests/core_test.cpp`.
- **`flycoos`** (executable: `main`, `renderer` + `shaders/world.metal`): platform, input, camera
  and GPU.

```
main.cpp ──► generateTerrain() ──► Grid (1024×192×1024 bytes)
   │                                  │
   │                       Renderer::uploadWorld()
   │                          └─ parallelFor(chunks) ─► meshRegion() ─► packed quads ─► GPU storage buffers
   │
   └─ loop: events ─► Input ─► updatePlayer() ─► posePlayer() ─► camera ─► Renderer::render()
```

## Modules

| File | Responsibility |
|---|---|
| `src/world.h/.cpp` | `Block` ids, `Grid` (dense voxel volume, x fastest, out-of-bounds = Air), `parallelFor`, region-based terrain, trees and rocks, `findSpawn` |
| `src/mesher.h/.cpp` | Greedy mesher with per-corner AO, `Quad` packing/unpacking |
| `src/player.h/.cpp` | Walking-body physics and collision (player and NPCs), character models (`buildPart`), procedural animation (`poseModel`) |
| `src/entity.h/.cpp` | NPC entities: per-kind traits, spawning, villager wander AI, talking, damage |
| `src/hud.h/.cpp` | In-game overlay on ImGui's background draw list: nameplates, speech balloons, talk hint, player health |
| `src/renderer.h/.cpp` | SDL3 GPU device, pipelines, buffer uploads, render targets, frustum culling, day/night lighting, screenshots |
| `src/debug_ui.h/.cpp` | Dear ImGui debug window (`.` toggles): Render, Player, Entities, Camera, World, Stats tabs. Owns the ImGui frame (`begin`/`end`; the HUD draws in between), `GameSettings` and `DebugActions` |
| `src/main.cpp` | CLI options, window, input routing (game vs. debug UI), orbit camera, main loop, benchmark stats |
| `shaders/world.metal` | `voxel_vs` (vertex pulling), `voxel_fs`, `water_fs`, `sky_vs`, `sky_fs` |

## World

- `Grid` stores one `Block` byte per voxel: 1024×192×1024 = 192 MiB. There is no per-chunk storage.
  Chunks (32³) exist only as meshing and draw units.
- **Regions.** Two very low-frequency noise fields (freq 0.002, domain-warped by up to 100 blocks for
  organic borders) give each column a *forest* and a *rocky* strength from 0 to 1. Rocky land
  suppresses forest, so the two stay mostly separate. Where both are near 0 the land is **open
  plains**. Regions are a few hundred blocks across; that size is what makes the world feel vast.
- **Terrain** (`generateTerrain`, parallel over z-rows):
  - Hill height scales with region: about 8 blocks on plains, 18 in forests, up to 58 on rocky land.
    The hill noise is squared, so hills rise out of flat ground instead of rippling.
  - Rare mountain ranges add up to 80 blocks, and lake basins dip below sea level.
  - Surface block: sand at the shore, grass, broad bare-stone patches on rocky land, stone above 110,
    snow above 130. Below the surface come 3 blocks of subsoil, then stone.
  - Water fills up to `kSeaLevel` (34). Heights are capped at `size.y - 30` to leave headroom.
- **Decoration**, run sequentially (the shapes cross column boundaries), seeded per cell so it is
  deterministic:
  - **Trees:** one candidate per 7×7 cell, with probability 75% × forest strength plus 0.4%, so the
    plains get a lone tree now and then. Trunks are 8–14 blocks (2×2 from 12 up), with a flattened,
    ragged canopy of radius ~4–5.
  - **Rocks** (`Rock` block): one candidate per 14×14 cell, with probability 40% × rocky strength plus
    0.6%. On rocky land most are tapering spires 10–23 blocks tall; the rest are half-buried boulders.
- The world is deterministic per seed (tested). `core_test` also checks that forests and rocks exist
  and that a 96×96-column plain with no trees or rocks exists. Generation takes ~0.15 s on an M4.

## Meshing (`meshRegion`)

Per region (a 32³ chunk, or a whole character part):

1. Skip the region if it is all air.
2. For each of the 6 face directions, sweep slices along the face axis. Build a 2D mask of visible
   faces, keyed by `block | AO << 8`. A face is visible if the neighbour isn't opaque. Water faces
   are visible only against air.
3. **AO** per face corner is the classic 3-sample rule (`side1 && side2 ? 0 : 3 - (side1 + side2 + corner)`),
   sampled in the cell outside the face.
4. **Greedy merge**: grow rectangles along u, then v, while the key is identical. Faces with different
   AO never merge, so AO stays exact.
5. Opaque and water quads go to separate lists, so water can be drawn in its own blended pass.

Neighbours are read from the full `Grid`, so chunk borders produce no internal faces and AO is
seamless across chunks. The full world (~2,500 non-empty chunks, ~1.8M quads) meshes in ~1.1 s using
all cores.

### Packed quad layout (8 bytes)

Defined in `src/mesher.h`, decoded in `voxel_vs`:

```
a (uint32):  x:6 | y:6 | z:6 | (w-1):6 | (h-1):6 | unused:2      position relative to region origin
b (uint32):  face:3 | ao:8 (corners 0..3, 2 bits each) | block:8 | unused:13
```

- Faces: `0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z`. A face on axis `a` spans `u = (a+1)%3` (width) and
  `v = (a+2)%3` (height). For positive faces `u × v = normal`, so corners 0→1→2→3 =
  (0,0),(1,0),(1,1),(0,1) wind counter-clockwise when seen from outside.
- AO values run from 0 (dark) to 3 (lit).

## Rendering

- **API:** SDL3 GPU on the Metal backend. MSL is compiled at runtime from `shaders/world.metal`
  (copied next to the binary at build time).
- **Vertex pulling:** there are no vertex buffers. Each chunk draws `6 × quadCount` vertices
  from a single storage buffer holding every world quad. The chunk's first quad index arrives in its
  Draw uniforms (`firstQuad`), and draws always use `first_vertex = 0`, because SDL_gpu.h
  documents `first_vertex` + `vertex_id` as non-portable.
  `voxel_vs` reads `quads[firstQuad + vertex_id / 6]` and takes the corner from `vertex_id % 6`.
  It flips the triangle diagonal when AO is anisotropic and reverses winding for negative faces.
- **Per-draw data:** one 96-byte uniform push per chunk (`model` matrix, color-jitter offset,
  `firstQuad`). Frame data (matrices, lighting, 32-entry palette) is pushed once per frame to
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
  (color per 32³ cell), AO only, or normals. The branch is on a uniform, so it's coherent. The quad
  index rides in the high 24 bits of the existing flat `blockAndQuad` output rather than a new
  one: on Apple's tile-based GPUs every extra vertex output costs bandwidth, and two extra outputs
  measured ~15% slower frames.
- **Color:** the swapchain is `SDR_LINEAR` (sRGB format), so all lighting is in linear space. The
  palette is sRGB in `kPalette` and converted at upload. Output goes through an ACES filmic tonemap.
- **Shading:** a directional light (sun by day, moon by night) plus a hemispheric sky/ground
  ambient, multiplied by AO. A ±10% per-voxel brightness jitter comes from a hash of the voxel cell.
  Exponential-squared fog blends to the sky color in the view direction, so the horizon matches the
  sky.
- **Water:** the top surface is lowered by 0.12, with Fresnel sky reflection and a sun specular
  highlight.

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
  the rig: player, villager (recolored) and training dummy (post, base, straw body and arms, sack
  head; never animated). All parts of all models live in one GPU buffer; each frame draws a list of
  `ModelInstance`s.
- **Tuning and flight:** walk/run/jump speed, gravity, fly and noclip live in `Player::tuning`, so
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
  - Punch: the fist shoots forward to shoulder height and the torso twists into it, alternating
    hands. The dummy wobbles back when hit and topples over when out of health.

## Stats and combat

- Five attributes (`Stats` in stats.h); every other stat derives from them. No classes, so no attack
  power or other in-between step:

  | Attribute | Effect |
  |---|---|
  | Stamina | +10 max health per point |
  | Strength | physical damage: `damage = baseDamage + strength * speed` |
  | Agility | +1% physical attack speed per point (haste: swing time = speed / (1 + 1% x agility)) |
  | Intellect | unused until magic |
  | Spirit | +0.5 health per second, out of combat only |

- `speed` in the damage formula is the attack's *base* speed (WoW's weapon speed), not the hasted
  swing time. Slow attacks hit harder per swing, and agility only adds damage per second.
- Punch (the only attack until equipment): 6–10 damage, speed 1.0 s. Base player: stamina 10,
  everything else 2, so 100 health and 8–12 damage every 0.98 s.
- Left mouse starts a punch toward where the camera looks, once the swing timer allows. Holding
  keeps swinging; a click during the cooldown is queued. Damage lands at the fist's impact frame
  (40% into a 0.35 s animation) on the nearest living, attackable entity within 2 blocks and
  60° of the facing direction.
- Dealing or taking damage puts a body in combat for 5 s; spirit regeneration only runs out of
  combat.
- At 0 health the dummy topples, can't be hit, and stands back up fully healed after 5 s. No reward.
- HUD: a damage number pops above the target and rises for a second (the latest hit only).

## Entities

- An `Entity` is a kind, a name, a level and a `Player` body (position, animation, health). Fixed
  traits per kind live in one table (`kTypes` in entity.cpp):

  | Kind | Name | Lv | Sta/Str/Agi/Int/Spi | HP | Attackable | Moves | Talks |
  |---|---|---|---|---|---|---|---|
  | Training Dummy | "Training Dummy" | 1 | 10/0/0/0/10 | 100 | yes | no | no |
  | Villager | random from a name list (seeded) | 5 | 25/5/5/5/5 | 250 | no | wanders | yes (E) |

- `damage()` only applies to living, attackable kinds and stops at 0 (see Stats and combat).
- **Villager AI:** alternates standing (2–6 s) and strolling at 2 b/s (2–5 s) in a random direction.
  Before each step it checks the block ahead: no walls it can't step up, no water, no drops of 2+.
  It turns back once more than 12 blocks from home. NPC bodies use the same `updatePlayer` physics as
  the player, driven by AI input.
- **Talking:** E picks the nearest talker within 3.5 blocks. It shows a random phrase (never the same
  twice in a row) for 5 s, while the villager stops and turns to face the player.
- **Separation:** the player is pushed out of an entity's 0.8-block radius (never into terrain).
- **HUD:** nameplates (level, name, health bar; amber/red for attackable, green for friendly) within
  40 blocks. They're hidden when terrain blocks the line of sight, using the camera's box march. The
  HUD always runs an ImGui frame; the UI render pass is skipped when there is nothing to draw.
- NPCs are respawned beside the player on world regeneration or from the Entities tab.

## Day/night

`timeOfDay` runs from 0 to 1 (10-minute day; hold T for 60×). The sun path is tilted so its peak is
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
| Meshing + upload | ~1.1 s |
| GPU memory for world quads | ~14 MB |
| Mean frame time (benchmark, vsync off) | ~2–3 ms |

## Known ceilings

| Ceiling | Upgrade path |
|---|---|
| Fixed-size world (192 MiB dense grid), meshed once at startup | Chunk streaming + per-chunk remesh jobs (M1) |
| Opaque meshes rebuilt as a whole; no block editing | Per-chunk buffer sub-allocation |
| One draw call per chunk | `SDL_DrawGPUPrimitivesIndirect` with a GPU-culled draw list |
| MSL-only shaders (macOS) | HLSL/Slang + SDL_shadercross for SPIR-V/DXIL (M1) |
| No shadows | Cascaded shadow map for sun/moon (M1) |
| Absolute world-space float positions (jitter far from the origin) | Camera-relative rendering: subtract the camera's chunk origin on the CPU (needed for M1 streaming) |
| Meshing is driven by `Renderer::uploadWorld` | Move meshing into a core job that produces `Mesh`; the renderer only uploads |
