# Architecture

Milestone 0 is about 1,200 lines of C++ (including tests) and 160 lines of MSL. It has two layers:

- **`flycoos_core`** (static library: `world`, `mesher`, `player`): pure CPU code with no GPU or SDL
  dependency, unit-tested by `tests/core_test.cpp`.
- **`flycoos`** (executable: `main`, `renderer` + `shaders/world.metal`): platform, input, camera
  and GPU.

```
main.cpp ──► generateTerrain() ──► Grid (512×128×512 bytes)
   │                                  │
   │                       Renderer::uploadWorld()
   │                          └─ parallelFor(chunks) ─► meshRegion() ─► packed quads ─► GPU storage buffers
   │
   └─ loop: events ─► Input ─► updatePlayer() ─► posePlayer() ─► camera ─► Renderer::render()
```

## Modules

| File | Responsibility |
|---|---|
| `src/world.h/.cpp` | `Block` ids, `Grid` (dense voxel volume, x fastest, out-of-bounds = Air), `parallelFor`, terrain/tree generation, `findSpawn` |
| `src/mesher.h/.cpp` | Greedy mesher with per-corner AO, `Quad` packing/unpacking |
| `src/player.h/.cpp` | Player physics and collision, character model (`buildPart`), procedural animation (`posePlayer`) |
| `src/renderer.h/.cpp` | SDL3 GPU device, pipelines, buffer uploads, render targets, frustum culling, day/night lighting, screenshots |
| `src/main.cpp` | CLI options, window, input, orbit camera, main loop, benchmark stats |
| `shaders/world.metal` | `voxel_vs` (vertex pulling), `voxel_fs`, `water_fs`, `sky_vs`, `sky_fs` |

## World

- `Grid` stores one `Block` byte per voxel: 512×128×512 = 32 MiB. There is no per-chunk storage.
  Chunks (32³) exist only as meshing and draw units.
- **Generation** (`generateTerrain`), parallel over z-rows:
  - Height comes from two FastNoiseLite OpenSimplex2 fBm fields: rolling hills (freq 0.0045) plus a
    cubed mountain mask (freq 0.0018).
  - The surface block depends on height: sand at the shore, grass, stone above 78, snow above 92.
    Below the surface come 3 blocks of subsoil, then stone.
  - Water fills up to `kSeaLevel` (34).
  - Trees are placed afterwards on a single thread: at most one per 6×6 cell, density driven by a
    third noise field. Each has a trunk of 4–6 blocks and an ellipsoid leaf blob.
- The world is deterministic per seed (tested). Generation takes ~0.02 s on an M4.

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
seamless across chunks. The full world (~560 non-empty chunks, ~480k quads) meshes in ~0.12 s using
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
     4. Water: alpha-blended, depth test on, depth write off.
  3. Blit `resolved_` to the swapchain. Owning the final image is what makes `--screenshot` possible.
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
  - Gravity is 28, jump speed 9.5. Walk is 5 b/s, run 9 b/s, with exponential velocity smoothing.
  - In water, gravity is weak and Space swims up.
- **Character:** six floating parts (head, torso, 2 hands, 2 feet), each a tiny `Grid` built by
  `buildPart` and meshed by the same mesher. One character voxel is 0.1 blocks.
- **Animation:** procedural, in `posePlayer`.
  - The gait phase advances with the *distance actually travelled* (stride 1.7 blocks), so the feet
    don't slide or keep walking against walls.
  - Feet swing ±3 voxels and lift on the forward half of the swing. Hands counter-swing. The torso
    bobs, leans and twists.
  - `gaitAmount` blends between idle breathing and walking/running, and `airAmount` raises the arms
    and tucks the feet in the air.
  - Body yaw eases toward the movement direction.

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
| World generation | ~0.02 s |
| Meshing + upload | ~0.11–0.22 s |
| GPU memory for world quads | ~3.7 MB |
| Mean frame time (benchmark, vsync off) | ~0.5–1.3 ms |

## Known ceilings

| Ceiling | Upgrade path |
|---|---|
| Fixed-size world, meshed once at startup | Chunk streaming + per-chunk remesh jobs (M1) |
| Opaque meshes rebuilt as a whole; no block editing | Per-chunk buffer sub-allocation |
| One draw call per chunk | `SDL_DrawGPUPrimitivesIndirect` with a GPU-culled draw list |
| MSL-only shaders (macOS) | HLSL/Slang + SDL_shadercross for SPIR-V/DXIL (M1) |
| No shadows | Cascaded shadow map for sun/moon (M1) |
| Absolute world-space float positions (jitter far from the origin) | Camera-relative rendering: subtract the camera's chunk origin on the CPU (needed for M1 streaming) |
| Meshing is driven by `Renderer::uploadWorld` | Move meshing into a core job that produces `Mesh`; the renderer only uploads |
