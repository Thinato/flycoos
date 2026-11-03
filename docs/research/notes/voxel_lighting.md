# Voxel lighting for flycoos: flood-fill block/sky light, per-vertex AO, occlusion color ramping, GPU fit

Context checked in the repo (2026-11-03): `shaders/world.metal` and `src/mesher.{h,cpp}` already implement 0fps-style per-vertex AO (2 bits x 4 corners) with the quad-flip trick in the vertex shader. Greedy merging keys on block + AO. The packed quad is 8 bytes: `a = x:6|y:6|z:6|(w-1):6|(h-1):6` (30 bits), `b = face:3|ao:8|block:8` (19 bits, so **13 bits are free**). `light()` in `world.metal` applies hemisphere ambient times AO plus sun/moon N·L diffuse. There are **no shadows and no sky-visibility term**, so a roofed interior or a barrow at noon gets the same sun diffuse as open ground. That is the core "dark interiors" problem. The world is a dense 1 byte/voxel grid, 1024x192x1024 = 201,326,592 voxels = 192 MiB (`src/world.h`, "ponytail: dense grid (192 MiB)"). It has 6,144 chunks of 32^3, and an `Ember` block (palette color 1.0, 0.42, 0.10) that does not emit light today.

---

## Q1. What is the simplest robust dynamic-light approach for a 1024x192x1024 dense world with 32^3 chunk meshes on Metal, and what does it cost in memory and CPU?

### Takeaway
Use Minecraft-style integer flood-fill light: two 4-bit channels (sky 0-15, block 0-15) per voxel, BFS add and remove queues on the CPU, and the value baked into the mesh per face. That is the proven, single-developer-sized design (Minecraft, Luanti, Veloren, Seed of Andromeda). For flycoos, the cheapest variant stores one flat (sky, block) nibble pair **per greedy quad** in 8 of the 13 free bits. AO stays per-vertex. A light change then costs a BFS of a few thousand cells plus a remesh of up to 8 chunks. The dense light array would add 192 MiB (1 B/voxel), so store it per chunk and allocate it lazily.

### Cited Findings
**Light model (Minecraft):**
- Minecraft has 16 light levels (0-15) and two types, sky light and block light. The game usually uses the higher of the two.
  - Block light spreads by flood fill and drops 1 per block of Manhattan distance. A torch (14) lights adjacent floor to 13 and diagonals to 12.
  - Sky-exposed blocks have sky light 15. Level-15 sky light passes **straight down through transparent blocks without loss**, and drops 1 per block sideways or upward.
  - Water and leaves filter sky light by 1 in Java Edition. In Bedrock, filtering varies (water 1, leaves 2). — [Minecraft Wiki: Light](https://minecraft.wiki/w/Light)
- **Stored sky light is not reduced at night.** Minecraft computes a separate time/weather-dependent sky value for gameplay, and rendered brightness is a separate calculation from light level. — [Minecraft Wiki: Light](https://minecraft.wiki/w/Light)

**Per-voxel storage in shipped engines:**
- Minecraft: 1 byte per voxel (4 bits torch, 4 bits sky). Seed of Andromeda: 2 bytes (4 bits each R, G, B, sky). Lysenko's variant: 4 bytes (RGB plus 5x4-bit directional sky). — [0fps: Voxel lighting (2018)](https://0fps.net/2018/02/21/voxel-lighting/)
- Lysenko credits Ben Arnold (Seed of Andromeda, "Fast flood fill lighting in a blocky voxel game", parts 1 and 2) with the first detailed write-up of BFS flood-fill lighting.
  - Lysenko frames flood-fill lighting as "an approximation of ambient occlusion".
  - The original SoA links are dead, and a reader reports they now redirect to a phishing page. Do not visit them. — [0fps: Voxel lighting](https://0fps.net/2018/02/21/voxel-lighting/)
- Lysenko processes all channels packed in one machine word at once (SWAR). He uses mask constants like `0xf0f0f0f` for component-wise less-than, max, and decrement-with-saturate, so channels need no per-channel loops. — [0fps: Voxel lighting](https://0fps.net/2018/02/21/voxel-lighting/)
- Luanti keeps light in `param1`: "light with and without sun in its lower and upper 4 bits respectively".
  - `LIGHT_MAX` = 14 (the brightest emitter); `LIGHT_SUN` = 15.
  - `sunlight_propagates = true` means "sunlight will go infinitely through this node". — [Luanti lua_api.md](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md); [Luanti src/light.h](https://raw.githubusercontent.com/luanti-org/luanti/master/src/light.h)

**BFS add:**
- Pop a cell. For each of the 6 neighbours that is not solid, if the neighbour's level + 2 <= the current level, set it to current - 1 and enqueue it. Each RGB channel is handled independently. The cell is stored as `u8vec4` (R, G, B, Sun). — [JuanDiegoMontoya gist (citing SoA part 1)](https://gist.github.com/JuanDiegoMontoya/383bf2cc2ad3d1b8717a42b1f0bc6a7b)

**BFS remove (two-phase, the standard Arnold-style scheme):**
- Phase 1: BFS out from the removed source carrying `old_level`.
  - A neighbour with `light != 0 && light < old_level` was lit by this source. Zero it and enqueue it with its own old level.
  - A neighbour with `light >= old_level` is lit independently. Collect it as a "boundary source".
  - Other emitters found while zeroing are also re-seeded.
- Phase 2: run the normal add-BFS from all boundary sources. — [voxel-light crate, remove.rs](https://docs.rs/crate/voxel-light/latest/source/src/remove.rs)
- Placing an opaque block is handled as a removal, because it can cut light paths. — [voxel-light remove.rs](https://docs.rs/crate/voxel-light/latest/source/src/remove.rs)
- A voxelize design note recommends a **FIFO** removal queue, because LIFO "can starve nodes and leave artefacts". — [voxelize notes/three_pass_lighting.md](https://docs.rs/crate/voxelize/latest/source/notes/three_pass_lighting.md) (seen via search summary only, not fetched in full)
- Sky in voxel-light: level 15 starts at the sky height and travels down without decay until it reaches an opaque block. — [voxel-light](https://docs.rs/crate/voxel-light/latest/source/src/sky.rs) (via search summary)
- Voxelworld's variation: sky light at 15 does not attenuate downward, but once it is <= 14 it attenuates in all directions.
  - Light is not saved to disk. It is regenerated when chunks load.
  - Colour channels propagate independently.
  - The author reports occasional lag spikes and calls it slower than Minecraft and Luanti. — [Voxelworld lighting engine blog (2025)](https://jli69.github.io/blog/2025/03/25/voxelworld-lighting-engine.html)

**Veloren (bakes light at mesh time):**
- Veloren computes light **at mesh time, per chunk**. `calc_light` runs twice, once for sun and once for glow.
  - Levels go up to `SUNLIGHT = 24`.
  - The working map is padded by 24 horizontally around the chunk.
  - Sun rays are cast top-down per column with per-block `(min_light, attenuation)`, then a `VecDeque` BFS spreads to the 6 neighbours at -1 per step.
  - Results are packed with colour and AO into per-face texels in a greedy-mesh atlas (`TerrainAtlasData`, `make_col_light`).
  - A TODO in the code says glow-source scanning is expensive. — [Veloren voxygen/src/mesh/terrain.rs](https://gitlab.com/veloren/veloren/-/raw/master/voxygen/src/mesh/terrain.rs)

### Inferences
- **Memory.**
  - A dense 1 B/voxel (sky + block nibbles) light grid adds another 192 MiB, doubling world RAM. 2 B/voxel for RGB + sky adds 384 MiB.
  - Better: allocate a 32 KiB light array per chunk lazily. Chunks entirely above the column heightmap are implicitly sky 15, block 0. Chunks entirely solid are implicitly 0.
  - On mostly-heightmap terrain, roughly 1-2 chunk layers per column straddle the surface. That is about 1,024-2,048 chunks x 32 KiB = 32-64 MiB. This is an estimate, not measured.
- **Recomputing like Veloren instead of storing.** Recomputing light from the block grid at mesh time, the way Veloren does, avoids storing light entirely. But each chunk then floods a halo of up to 14 cells, so the work is roughly (32+28)^3/32^3, about 6.6x the chunk volume (a geometry estimate). Stored light plus incremental updates is cheaper for frequent edits. Recompute-at-mesh is simpler and has no add/remove bookkeeping. **This is a legitimate lazy option**, because flycoos remeshes edited chunks anyway.
- **CPU per edit.** A level-14 point light reaches every cell within Manhattan radius 13: (2r+1)(2r^2+2r+3)/3 = 3,303 cells. That is a trivially small BFS. Removal is about twice that.
  - The real cost is the remesh. Light radius 14 is smaller than 32, so a torch touches at most 2 chunks per axis, which means at most 8 chunk remeshes.
  - Placing a roof block over open sky is the worst case. The sky column below loses its free downward 15, and a removal and re-flood can sweep an area up to about 15 blocks around. That is still bounded and still at most about 8 chunks.
- **Initial sky fill for the world.**
  1. Write sky 15 in every column top-down until the first opaque voxel.
  2. Seed the BFS only from lit cells that border unlit air (cave mouths, overhangs, under roofs).
  This is O(voxels) for the column pass. Timing over 201M voxels is not measured, so benchmark it.
- **Cheapest mesh integration for flycoos.** Sample the light of the air cell in front of each face. Put the sky and block nibbles (8 bits) into the 13 free bits of `Quad.b`, and add them to the greedy merge key. AO stays per-vertex.
  - This gives Minecraft's "smooth lighting off" look: stepped, per-block light and per-vertex AO.
  - It fits flycoos' blocky style and changes no vertex size.
  - Expect more quads near light gradients, because merges break at each level change. Measure the quad-count delta with the existing perf A/B.
- **Smooth (per-corner) light** needs 4 corners x 8 bits = 32 more bits. That means a third `uint` per quad (8 to 12 bytes, or 16 with alignment), which is more vertex-fetch bandwidth on TBDR.
  - The shader comment already notes that each extra vertex output costs about 15% on tile-based GPUs.
  - Prefer the 3D-texture route (Q1 alternatives below) if smooth light is wanted.

### Alternatives (GPU-side), cited where possible
- **Clustered / tiled forward shading.**
  - Clustered shading groups view samples by 3D position (and optionally normal) instead of 2D tiles. It maps lights to samples better than tiled shading, and supports up to about one million lights in real time. — [Olsson, Billeter, Assarsson, "Clustered Deferred and Forward Shading", HPG 2012](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf)
  - Clustered forward outperforms tiled forward by more than 2x in the authors' tests. — [Olsson et al., "Tiled and clustered forward shading: supporting transparency and MSAA"](https://research.chalmers.se/en/publication/161726)
- **Teardown.**
  - Teardown stores each object as a 3D volume texture at 1 byte/voxel.
  - It uses ray tracing against voxels (no hardware RT; OpenGL 3.3) for AO, soft shadows and specular occlusion, and skips full GI.
  - It is described as a physically-inspired deferred renderer. — [80.lv interview](https://80.lv/articles/teardown-developer-breaks-down-multiplayer-and-voxel-destruction-tech); [Teardown breakdown (J. D. Montoya)](https://juandiegomontoya.github.io/teardown_breakdown.html); [Acko: Teardown Frame Teardown](https://acko.net/blog/teardown-frame-teardown/)
  - These details come via search summaries. Read the full pages before quoting specifics.
- **Veloren** placeable lights: voxel colours and light colours are converted sRGB to linear in shaders. A later update added a depth-buffer-based halo around point lights. — [This Week in Veloren items via planet.freegamedev.net](https://planet.freegamedev.net/?p=149557) (search summary only)

### Inferences on alternatives
- **Shadowless point lights** (forward, a small fixed array of nearest N lights in `Frame` uniforms, e.g. 16 embers or torches) light the scene without remeshing. But they **leak through walls**: an ember inside a house lights the outside wall too. That is fine for a held torch or a dynamic spell flash, and wrong for static lights.
  - Full clustered culling is unnecessary at flycoos' light counts. A uniform loop over at most 16 lights with a distance-squared cull is the lazy rung.
- **Light volume (3D texture) sampled in the fragment shader.** This is the middle ground and the best upgrade path if per-face flat light looks too steppy.
  - Upload the CPU flood-fill result into an RG8 3D texture (R = sky, G = block). For coloured light use RGBA8: sky plus RGB block.
  - Sample at `worldPos + normal*0.5` with hardware trilinear filtering. You get smooth lighting for free, and light edits become a sub-box texture upload with **no remesh**.
  - A full-world RG8 texture is 384 MiB, too much. Use a player-centred toroidal window, e.g. 384x192x384 RG8 = 54 MiB, or per-chunk 34^3 bricks.
  - Trilinear sampling can bleed light through 1-voxel walls. Mitigate by sampling at the face's air cell with a half-voxel inset, or by using nearest filtering plus the existing per-vertex AO.
  - SDL3 GPU supports 3D textures (`SDL_GPU_TEXTURETYPE_3D`), so this is within the current API. Unverified against flycoos' SDL version; check before building.
- **Voxel cone tracing / ray-traced GI** (Teardown-class) is overkill for a single developer and a TBDR GPU budget. It needs a voxelized scene in GPU memory plus per-pixel tracing. No flycoos-specific measurements exist. Skip it.

### Gaps
- I could not retrieve Ben Arnold's original SoA articles: the links are dead and reportedly redirect to phishing. The removal algorithm is sourced from the voxel-light crate and the gist instead.
- I found no benchmark numbers (ms per BFS, ms for an initial world fill) from any primary source. flycoos must measure.
- I found no reliable source for Minecraft's exact level-to-brightness table. The Minecraft Wiki text available to me omitted it. A commonly quoted formula is `f/(4-3f)` with `f = level/15`, but it is unverified here, so treat it as a tunable curve.

---

## Q2. How is per-vertex AO computed and packed into the mesh, and how do you avoid anisotropy artifacts?

### Takeaway
Each face vertex looks at the 3 voxels in front of the face around that corner: side1, side2 and corner. AO = 0 if both sides are solid, else 3 - (side1 + side2 + corner). Pack 2 bits per corner. When the AO values are not coplanar, triangulate along the diagonal that matches the AO gradient (the "quad flip"). Greedy-merge only faces whose 4 AO values are identical. **flycoos already does all three correctly.** The remaining improvements are a non-linear AO curve and combining AO with light.

### Cited Findings
- **Formula.**
  - `vertexAO(side1, side2, corner)` is 0 when both sides are solid, and otherwise `3 - (side1 + side2 + corner)`.
  - That gives 4 values, 0 (fully occluded) to 3 (open).
  - The both-sides case prevents light leaking through the diagonal corner. — [0fps: Ambient occlusion for Minecraft-like worlds](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/)
- **Anisotropy fix.**
  - Interpolating non-linear corner values across two triangles depends on the chosen diagonal.
  - Fix: `if (a00 + a11 > a01 + a10)` emit the flipped quad, else the normal one. The article's text has a typo repeating a01. — [0fps AO](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/)
- **Greedy meshing.** Merge only faces whose AO matches at every vertex. AO stays constant along merged edges longer than 1 voxel, so merging does not break the shading. — [0fps AO](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/)
- **Smooth lighting and AO in Minecraft.**
  - Smooth lighting came from averaging light levels at vertices. Solid blocks hold light 0, so the averaging yields AO for free.
  - Lysenko (in the comments) says averaging light and AO occlusion agree when all non-zero light levels are equal, but the diagonal-corner check is still needed to avoid leaks through cracks. — [0fps AO, Edit 2 and comments](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/)
- A commenter reduced interpolation banding by remapping AO levels 0..3 through a curve, e.g. `{0, 0.6, 0.8, 1.0}`. — [0fps AO comments](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/)
- The simple 3-neighbour lookup breaks for non-cube shapes, which need per-vertex weights. — [0fps AO comments](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/)
- Minecraft "smooth lighting" blends light across faces and uses AO to darken corners where diagonal blocks are solid. — [Minecraft Wiki: Light](https://minecraft.wiki/w/Light)
- Veloren's mesher passes a per-block `get_ao` (0 if opaque, 1 otherwise) into its greedy mesher and stores AO with light and colour in per-face atlas texels, not in vertices. — [Veloren terrain.rs](https://gitlab.com/veloren/veloren/-/raw/master/voxygen/src/mesh/terrain.rs)

### Inferences
- **What flycoos already has.**
  - `faceAO` in `src/mesher.cpp` implements the exact 0fps formula: `s1 && s2 ? 0 : 3 - (s1 + s2 + corner)`.
  - `voxel_vs` picks the diagonal with `ao[0] + ao[2] < ao[1] + ao[3]`, which is the flip trick in flycoos' corner order (0=(0,0), 1=(1,0), 2=(1,1), 3=(0,1)).
  - The greedy key is `block + AO`.
  - AO is linear today: `mix(0.35, 1.0, ao/3)` for ambient. A cheap tweak is a lookup curve, e.g. `{0.35, 0.62, 0.82, 1.0}` from a constant array. Tune it by screenshot A/B.
- **If smooth per-corner light is added later**, the flip decision should use the combined value, e.g. `ao*K + light`, not AO alone. Otherwise light gradients reintroduce the anisotropy. Per-face flat light needs no change, because the light is constant across the quad.

### Gaps
- I found no measurements of AO curve choices on TBDR hardware. The fragment cost is negligible, since it is a 4-entry constant lookup in the VS.

---

## Q3. How do games produce dark interiors with skylight propagation, and torch/ember light at night?

### Takeaway
Keep two channels. Sky light is "how open to the sky is this cell" and does not change with time. Block light comes from emitters. At render time:
- Scale **everything sun- or sky-derived** (hemisphere ambient and sun diffuse) by `curve(sky)`, and multiply that by a global daylight factor.
- Add block light with a warm colour that does **not** follow the day/night factor.

Interiors and barrows then go dark because sky light decays by 1 per block away from openings and reaches 0 deep inside. Torches and embers stay constant at night. In flycoos this sky term also stands in for the missing sun shadows indoors.

### Cited Findings
- **Minecraft.** Sky light at 15 falls straight down, and drops 1 per block sideways, so enclosed space darkens with distance from openings. Stored sky light is not reduced at night; a separate time-based value is used. — [Minecraft Wiki: Light](https://minecraft.wiki/w/Light)
- **Minecraft brightness targets** (lowest brightness setting):
  - Full daylight is about 98% brightness.
  - Night is about 17%, tinted blue.
  - Full darkness is about 5%.
  - Block-lit surfaces tend toward orange; daylight outdoors is white. — [Minecraft Wiki: Light](https://minecraft.wiki/w/Light)
- **Luanti light banks.**
  - Luanti stores a "day" bank (with sun) and a "night" bank (without sun) per node. `override_day_night_ratio(0..1)` controls how much sunlight counts.
  - At mesh time, `encode_light` writes the per-vertex sunlight ratio into the vertex colour alpha: `r = 255 - 255*night/day`, assumed 0 when the night bank is brighter.
  - The vertex shader then computes `color.rgb *= (a * dayLight.rgb + (1-a) * artificialLight) * 2`, with `artificialLight = (1.04, 1.04, 1.04)`.
  - So time-of-day changes are a **uniform change, with no remesh**. — [Luanti mapblock_mesh.cpp](https://raw.githubusercontent.com/luanti-org/luanti/master/src/client/mapblock_mesh.cpp); [Luanti nodes_shader/opengl_vertex.glsl](https://raw.githubusercontent.com/luanti-org/luanti/master/client/shaders/nodes_shader/opengl_vertex.glsl); [lua_api.md](https://raw.githubusercontent.com/luanti-org/luanti/master/doc/lua_api.md)
- **Luanti light curve.**
  - Light level maps to brightness through a user-tunable curve: a polynomial with coefficients from `lighting_alpha` and `lighting_beta`, a Gaussian "boost" (`lighting_boost`, `_center`, `_spread`), and a gamma clamped to 0.33-3.0. The result is baked into a 16-entry decode table. — [Luanti src/light.cpp](https://raw.githubusercontent.com/luanti-org/luanti/master/src/light.cpp)
  - Emissive light is added to the night bank multiplied by 2.5 before encoding. — [mapblock_mesh.cpp](https://raw.githubusercontent.com/luanti-org/luanti/master/src/client/mapblock_mesh.cpp)
- **Veloren** computes `light` (sun, default `SUNLIGHT` outside the area) and `glow` (from `Block::get_glow()` emitters, default 0) as separate fields, and bakes both into face texels so the shader can weight them independently. — [Veloren terrain.rs](https://gitlab.com/veloren/veloren/-/raw/master/voxygen/src/mesh/terrain.rs)
- **Vintage Story.**
  - Its colored-light system "can seamlessly blend 16k colors in any combination", and light colours are set per block in JSON (2016 blog). — [Vintage Story blog: New plants and colored lights](https://www.vintagestory.at/blog.html/news/new-plants-and-colored-lights-r35/)
  - A community analysis (not official) says every light, including sun, is HSV. Block light is about a third as bright as sunlight and falls by 1 per block. — [VS forum: questions about lighting](https://www.vintagestory.at/forums/topic/5611-questions-about-lighting)
- **Held lights without a world update.** Voxelworld lights the surroundings of held light-emitting items visually without changing stored block light. — [Voxelworld blog](https://jli69.github.io/blog/2025/03/25/voxelworld-lighting-engine.html)

### Inferences (flycoos-specific shader shape)
Proposed `light()`, with `sky`, `blk` in 0..1 after a curve:
```
skyVis  = curve(sky)                 // 0 in a barrow, 1 under open sky
ambient = hemi * ao * skyVis + caveAmbient * ao          // caveAmbient ~0.02-0.05, Minecraft's "5% full darkness"
sun     = lightColor * NdotL * mix(0.5,1,ao) * step-ish(skyVis)   // sky light as a crude sun-shadow proxy
torch   = emberColor * curve(blk) * ao                    // constant across day/night
color   = albedo * (ambient + sun + torch)
```
- Sun diffuse **must** be gated by sky light (e.g. `smoothstep(0.75, 1.0, skyVis)`). Otherwise house interiors stay sunlit, because flycoos has no shadow map. It is a hack with a known ceiling: shadows have no direction, so the shaded side under an overhang is wrong at low sun angles. The upgrade path is a sun shadow map.
- The day/night factor is already a uniform (`lightColor`, `ambientSky`, `fog.y` night factor). Sky light just multiplies it, so day/night needs **no remesh**, as in Luanti.
- **Emitters.** Give `Ember` an emission of about 12-14 in the block-light BFS, and use a warm `emberColor` (the palette's 1.0, 0.42, 0.10 boosted). Barrow torches and campfires are then Ember blocks inside prefabs.
  - Monochrome block light plus one warm colour covers the "torch/ember at night" brief.
  - RGB block light (3 nibbles, as in SoA) is only needed for differently-coloured magic lights. Skip it until a design needs it.
- **Barrow and cave darkness falls out automatically.** A barrow entrance passage of 15 or more blocks reaches sky 0. A shorter one fades to dark. To make a specific place darker or brighter, tune the curve, not the propagation.

### Gaps
- I found no primary documentation of how Cube World handles night or interiors. Cube World appears to have used no flood-fill sky light, but that is unverified.
- Teardown's lighting is ray-traced, and I found nothing about how it darkens interiors beyond "AO and soft shadows via voxel ray tracing".

---

## Q4. How do you do occlusion-based colour ramping in the fragment shader with a 32-colour palette (the Cube World-style look)?

### Takeaway
Do not just multiply albedo by a darkness scalar. Compute an occlusion/light factor `o` from AO, sky and block light. Then pick colours from a per-palette-entry ramp:
- Darker, cooler (blue/purple-shifted) and slightly more saturated in occlusion.
- Warmer toward highlights or torch light.

Two lazy implementations:
- **(a) Analytic:** `mix(shadowTint * albedo, albedo, o)` with a global cool tint.
- **(b) Ramp table:** 32 entries x N steps in the `Frame` uniforms or a tiny 2D texture, indexed by block id and quantised `o`.

The analytic version fits the existing shader in a few lines and is the first rung. A shipped engine (Luanti) does a simple version of this: it boosts blue in dark areas.

### Cited Findings
- Luanti's final colour blend adds blue in dark areas: a 32-entry `emphase_blue_when_dark` table indexed by brightness/8 adds up to 6/255 blue to the darkest ranges ("Emphase blue a bit in darker places"). — [Luanti mapblock_mesh.cpp, final_color_blend](https://raw.githubusercontent.com/luanti-org/luanti/master/src/client/mapblock_mesh.cpp)
- Minecraft tints night toward blue and block-lit areas toward orange. — [Minecraft Wiki: Light](https://minecraft.wiki/w/Light)
- **Pixel-art ramp practice.**
  - Shift shadows toward a cooler hue (blue/purple) and highlights toward a warmer hue (yellow/orange), e.g. green toward teal in shadow and toward yellow in highlight. — [Wayline: Color & Palettes for Pixel Art](https://www.wayline.io/learn/color-palettes/2); [Pixnote: Shading & Lighting](https://pixnote.net/en/learn/shading)
  - Typical steps are 10-20 degrees of hue per ramp step. Value comes first, then the hue shift. — [Wayline](https://www.wayline.io/learn/color-palettes/2); [Pixnote color guide](https://pixnote.net/en/learn/color-palette)
  - Use less hue shift for soft surfaces and more for hard ones. — [PixelJoint forum](https://pixeljoint.com/forum/forum_posts.asp?TID=15138)
- Veloren converts voxel and light colours from sRGB to linear in shaders, so the ramp maths runs in linear space. — [TWiV via planet.freegamedev](https://planet.freegamedev.net/?p=149557) (search summary)

### Inferences (shader recipes for flycoos)
- **(a) Analytic, a few lines in `light()`.**
  - `o = ao_curve * mix(caveFloor, 1, skyVis)`.
  - `shade = mix(albedo * shadowTint, albedo, o)` where `shadowTint ≈ (0.55, 0.6, 0.9)` in linear space.
  - Optionally add saturation in shadow: `mix(luma(c), c, 1.15)`.
  - Apply it to the albedo **before** lighting, so lighting still controls value and the tint only controls hue. Keep the tint weaker at night, because the moonlight `lightColor` is already blue (`0.35, 0.45, 0.75` in `renderer.cpp`); otherwise night double-blues.
  - Warm side: add block light with a warm colour, which automatically pushes torch-lit areas toward orange.
- **(b) Per-entry ramp table**, for exact art control as in Cube World's hand-tuned feel.
  - Precompute on the CPU per palette entry a 4-step ramp (AO levels 0..3) or an 8-step ramp. Rotate hue in OKLCH/HSV by about 10-20° per step toward blue, and drop lightness.
  - Store `float4 ramp[32][4]` = 2 KiB in `Frame` next to `palette[32]` (512 B today), or as a 32x8 RGBA8 2D texture with linear filtering along the light axis.
  - Index it with the block id (already flat in `blockAndQuad`) and `o*3`, lerping between neighbouring steps.
  - This keeps the vertex format unchanged. Colour ramps can be edited live from the debug UI.
- **Quantise or not.** A Cube World / pixel look comes from **stepped** ramps: `floor(o*N)/N`, applied after AO interpolation. Flat per-face light is stepped by nature. Per-vertex AO is interpolated, so the floor applies to that.
  - Hard steps along AO gradients can alias at distance. Fade the quantisation out with fog distance.
- **Order with fog and tonemapping.** Ramp, then light, then `applyFog`, then `tonemap`, as today. The ramp must come before fog, or fogged distant terrain gets the shadow tint and looks muddy.
- **Perf.** Option (a) adds a handful of ALU ops. Option (b) adds one constant-buffer or texture fetch. Neither adds vertex outputs, which matters given the existing note that each extra varying cost about 15% on TBDR. A/B both with the existing benchmark flags anyway.

### Gaps
- **Cube World:** I found no primary or technical source (Picroma/Wollay blog, talks, or reverse-engineering) describing its shading. Search returned only that it uses a custom engine, with a Vulkan rewrite for Cube World Omega. Any claim that Cube World uses palette ramps or hue-shifted AO is visual impression, not sourced. Verify against screenshots with the visual A/B before treating it as a reference.
- I found no published per-entry ramp-table implementation from Veloren or Vintage Story. Their shading details beyond the points above are unverified.
- I found no source quantifying the "right" shadow hue shift for 3D voxel games; the pixel-art guidance (10-20° per step) is from 2D practice.
