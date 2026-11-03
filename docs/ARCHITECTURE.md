# Architecture

Milestone 0 is about 1,200 lines of C++ (including tests) and 160 lines of MSL. It has two layers:

- **`flycoos_core`** (static library: `world`, `mesher`, `player`, `entity`, `item`, `edit`, `prefab`, `commands`): pure CPU
  code with no GPU or SDL dependency, unit-tested by `tests/core_test.cpp`.
- **`flycoos`** (executable: `main`, `renderer` + `shaders/world.metal`, `server`): platform, input,
  camera, GPU and the command server.
- **`tools/flycoos_mcp.py`**: the MCP server Claude Code starts; it forwards tool calls to the game.

```
main.cpp ──► loadWorld(--world) or generateTerrain() ──► Grid (1024×192×1024 bytes)
   │                                                     │
   │                                          Renderer::uploadWorld()
   │                                             └─ parallelFor(chunks) ─► meshRegion() ─► packed quads ─► GPU storage buffers
   │
   ├─ loop: events ─► SimInput ─► step() × N (fixed 60 Hz) ─► poseModel(alpha) ─► camera ─► Renderer::render()
   │
   ├─ editor (Tab): raycast() ─► applyEdit() / undoEdit() / redoEdit() ─► dirty Box ─► Renderer::updateWorld()
   │                saveWorld() on Ctrl+S
   │
   └─ CommandServer::take() ─► runCommand() ─► same edits/undo ─► updateWorld(); save, snapshot, goto
            ▲ socket thread, 127.0.0.1:47800 ◄── tools/flycoos_mcp.py (stdio MCP) ◄── Claude Code
```

## Modules

| File | Responsibility |
|---|---|
| `src/world.h/.cpp` | `Block` ids, `Grid` (dense voxel volume, x fastest, out-of-bounds = Air), `parallelFor`, region-based terrain, the natural shapes (`growTree`, `growRock`, `growPine`, `growBush`), `findSpawn`, `Instance`, `Marker` (`checkMarker`, `regionAt`, `playerSpawn`), world files (`saveWorld`/`loadWorld`) |
| `src/edit.h/.cpp` | World edits: `EditOp` → `applyEdit` → dirty `Box` + undo record; `undoEdit`/`redoEdit` (voxels, prefab instances and markers); `putMarker`/`removeMarker`; `raycast` (voxel DDA); `groundTop` |
| `src/prefab.h/.cpp` | Prefabs: the generated ones, the structure file parser and loader (`prefabs/*.txt`), `placePrefab`/`removePrefab`/`movePrefab` of instances |
| `src/commands.h/.cpp` | Remote commands: JSON request → `EditOp`s (one undo step), `path`, `heights`, `inspect`, `map` (PNG); returns what needs the game (remesh box, save, screenshot camera, goto) as data |
| `src/server.h/.cpp` | `CommandServer`: localhost TCP thread, newline-delimited JSON, one-slot mailbox to the main thread |
| `src/mesher.h/.cpp` | Greedy mesher with per-corner AO, `Quad` packing/unpacking |
| `src/player.h/.cpp` | Walking-body physics and collision (player and NPCs), character models (`buildPart`), procedural animation (`poseModel`) |
| `src/entity.h/.cpp` | NPC entities: per-kind traits and temper, spawning (`syncSpawns` from spawn markers, `defaultMarkers`), wander and chase AI, talking, damage both ways, loot |
| `src/session.h/.cpp` | `Session`: the simulation state (world, instances, markers, player, entities, the damage-roll RNG seeded from the world seed, time of day, the region the player is in); `startSession` (starting gear, spawns), `step(Session&, SimInput, dt)` (one `kTick`: interaction, swing, movement, NPCs and combat, time of day, region enter, the knocked-out player's respawn) |
| `src/item.h/.cpp` | Item table, `Inventory` (equipment, bag bar, item slots), moving/equipping rules, the main-hand attack |
| `src/inventory_ui.h/.cpp` | In-game Character (C) and Bags (B) windows: slots, drag and drop, right-click equip, tooltips, attributes |
| `src/hud.h/.cpp` | In-game overlay on ImGui's background draw list: nameplates, speech balloons, damage numbers, talk/loot hint, player health, knocked-out message, poi labels and the region name (`drawPlaces`) |
| `src/renderer.h/.cpp` | SDL3 GPU device, pipelines, buffer uploads, render targets, frustum culling, day/night lighting, screenshots |
| `src/editor_ui.h/.cpp` | Editor state (`Editor`), the Editor panel, crosshair and brush preview (ring on the ground, or Fill's box), marker pins and region circles |
| `src/debug_ui.h/.cpp` | Dear ImGui debug window (`.` toggles): Render, Player, Items, Entities, Camera, World, Stats tabs. Owns the ImGui frame (`begin`/`end`; the HUD draws in between), `GameSettings` and `DebugActions` |
| `src/main.cpp` | CLI options, world load/save, window, input routing (game vs. debug UI vs. editor) into `SimInput`, orbit camera, main loop around `step`, remote commands, benchmark stats |
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
  loads with no instances; older builds skip it.
- The `MARK` section (always written; empty when there are no markers) holds the markers sorted by
  id: `u32 count`, then per marker `u32 id | u8 kind | u8 name length | name | f32 x, y, z | f32
  radius | u8 entity | u8 count`. A file without it is older: it loads with `markers` = nullopt.
- The `LINE` section (written after `MARK`, only when a marker has dialogue lines) holds them by
  ascending marker id: `u32 count`, then per marker `u32 id | u8 line count | lines (u8 length |
  text)`. Each marker must exist and pass `checkMarker` with its lines; a second `LINE` section is
  rejected. A file without it (all
  older ones) loads with no lines; older builds skip it and drop the lines.
- Later content gets its own sections; old readers skip them, so adding one needs no version bump.
- Loading rejects, with a message, anything that doesn't parse exactly: wrong magic or version,
  checksum mismatch (any corruption or truncation), bad sizes, block ids or run lengths. A
  `--world` file that exists but fails to load stops the game rather than generating a world that a
  save would then write over it.
- Saving writes `PATH.tmp` and renames it over `PATH`, so a failed save never destroys the last
  good file.
- Real world (seed 1337): save 22 ms, load 13 ms, 2.78 MB.

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
   lowered ground gets the old surface block on its new top. Smooth moves toward the 5×5 average
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

- **Generated prefabs** (`tree`, `pine`, `bush`, `boulder`, `spire`) call the same `grow*`
  functions as `generateTerrain` (terrain stays bit-identical per seed; `core_test` pins hashes).
  They work on the live grid, so their rules depend on it: trunks grow down to the first solid
  voxel (≤ 3 below the ground), canopies and needles only fill Air, rocks overwrite. Jitter is
  seeded by the instance's seed at positions relative to its anchor, so a moved tree keeps its
  shape. They ignore rotation.
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
- **Editor:** tool 7 places `editor.place` on the aimed voxel or selects the instance owning it.
  `instanceAt` scans every instance cell, so the result is cached until the aimed voxel changes or
  the world is edited (`editor.edits`). The panel's instance list draws only its visible rows.

## Markers

Story markers say where things are and what places are called (ADR-015). A `Marker` is an id, a
kind, a name, a position (feet: x, z at a column center, y the top of the ground voxel), a radius,
and for spawns an entity kind, a count and dialogue lines. Kinds:

| Kind | Radius | Effect |
|---|---|---|
| `player_spawn` (at most one) | – | The player starts and gets back up here (`playerSpawn`); without one, `findSpawn` as before |
| `spawn` | wander area | `count` (1–8) NPCs of `entity` (see "Entities"); they wander within the radius and respawn at their home by the marker |
| `poi` | label range | Its name floats above it while the player is within the radius (fading in over the outer quarter) |
| `region` | the circle | Walking into it shows its name for 4 s; `regionAt` picks the smallest containing circle, so nested regions work |

- **Validation** (`checkMarker`, used by commands, the editor and the loader): known kind and
  entity, count 1–8, name ≤ 48 bytes of UTF-8 without control, bidi-override or zero-width
  characters (required for poi and region), finite position inside the world (y up to the world
  height), radius 0–512, and up to 8 lines of 1–160 bytes (checked like names) only on spawn
  markers, not the dummy's (`takesLines`; deliberately not tied to NPC traits, so saved worlds keep
  loading when a kind's traits change). The list holds at most 4096 markers, one player spawn, and spawn counts
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
  count) gets them all respawned; NPCs of a removed marker go; the rest keep their state. The first
  NPC stands on the marker, the others on a small spiral around it, each on the first dry ground
  within a few blocks of the marker's height. A spot inside terrain (a marker built over since) is
  lifted until the body is free (`clearOfTerrain`), for the player's spawn and for NPCs, whose
  update also frees one buried by a later edit. A rename or new lines only update the NPCs' copy of
  the marker (new lines start over from the first).
- **Lines** (ADR-016): talkers say the marker's next line on each E (cycling) instead of a random phrase;
  anyone that fights (guards too) says the next one when it turns angry (`anger`), at most every
  8 s, so hits from outside its territory (where it calms down at once) don't make it shout per hit;
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
  circles are drawn. The panel lists the markers with Go to and Remove; the debug window's Player
  tab shows the current region.

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
- **Marker commands:** `markers` (optionally of one kind and in a region, a region's circle
  counting), `mark`, `edit_marker`, `unmark`. Markers stand at the center of column `x`, `z` on
  ground voxel `y` (default the column's top). `map` draws them as dots labelled with their id
  (white player spawn, red spawn, yellow poi, cyan region with its circle) and lists the drawn ones
  (at most 500) with their name and pixel, since the map has digits only. A circle that misses the
  image or covers all of it isn't drawn, and one has at most as many points as the image's outline
  has pixels, so 1024 huge regions don't slow a small map. Default ground `y` is under any tree;
  ids must be whole numbers.
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
  hour). Lines that aren't UTF-8 JSON get -32700, non-objects and requests without a method -32600.

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
  from a single storage buffer holding every world quad (one for opaque, one for water). The chunk's first quad index arrives in its
  Draw uniforms (`firstQuad`), and draws always use `first_vertex = 0`, because SDL_gpu.h
  documents `first_vertex` + `vertex_id` as non-portable.
  `voxel_vs` reads `quads[firstQuad + vertex_id / 6]` and takes the corner from `vertex_id % 6`.
  It flips the triangle diagonal when AO is anisotropic and reverses winding for negative faces.
- **Chunk slots and edits:** every chunk (empty ones too) owns a slot `{first, count, capacity}` in
  each world buffer; `uploadWorld` packs them back to back and leaves ~1/8 + 64k quads of headroom at
  the end. After an edit, `updateWorld(dirty)` remeshes only the chunks overlapping the dirty box
  grown by 1 voxel (faces and AO read neighbours), in parallel. A chunk's new quads overwrite its
  slot if they fit; otherwise it gets a new slot with 25% spare at the end of the buffer. All writes
  of one edit go through one transfer buffer and copy pass. When the headroom runs out, every slot is
  repacked from the CPU copy of the meshes (no remeshing). See ADR-012.
  Measured (M4, release): an edit touching 1–8 chunks applies and re-uploads in 1–2 ms, a radius-32
  brush (18 chunks) in ~2.7 ms; 400 random brushes averaged 2.7 ms with one repack, worst ~10 ms.
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
  the pose. The rig has no legs, so greaves don't show.
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
  - Sword slash (main-hand weapon equipped): the right hand winds up to the right side, sweeps an arc
    at chest height through the front (blade pointing ahead at the impact frame), follows through to
    the left and settles back. The torso turns with the sweep. At rest the blade tilts forward.

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
- **Armor** is the sum of the equipped items' armor (`armor(inv)`); items in the bags don't count.
  Each landed hit is reduced by `armorReduction` (stats.h): `armor / (armor + K x attacker level)`,
  capped at 80% (`kMaxArmorReduction`), rounded, never below 1. K (`kArmorK` = 40) is the armor that halves a level 1 attacker's
  damage, and a slider in the debug Player tab (`Session::armorK`, passed to `updateCombat`).
  The player has no level yet, so its hits count as level 1. The full Iron set (37 armor) takes
  48% off a level 1 hit and 32% off a level 2 hit: the bandit's knife does 3–5 instead of 5–8.
  Debug "Hit for 10" / "Kill" bypass armor. The damage number shows what landed.
- Left mouse starts an attack toward where the camera looks, once the swing timer allows. Holding
  keeps swinging; a click during the cooldown is queued. Damage lands at the fist's impact frame
  (40% into a 0.35 s animation) on the nearest living, attackable entity within 2 blocks and
  60° of the facing direction (`inReach`). NPC swings land the same way, on the player.
- `hurt()` damages any body; `damage()` is hurt() for attackable entities and also angers fighters
  and rolls loot on a kill.
- Dealing or taking damage puts a body in combat for 5 s; spirit regeneration only runs out of
  combat.
- At 0 health a body falls over (the dummy topples off its base) and can't be hit. NPCs come back
  at home, fully healed, after their kind's respawn time (dummy 5 s, others 30 s). The player is
  knocked out: no movement or attacks, then back at the player spawn (marker, else the world spawn) with full health after 5 s.
- HUD: a damage number pops above whoever was hit and rises for a second (the latest hit only):
  gold over NPCs, red over the player.

## Items and inventory

- Items are ids into one table (`kItems` in item.cpp): name, equip slot, weapon `Attack`, bag size,
  armor.

  | Item | Slot | Notes |
  |---|---|---|
  | Iron Shortsword | Main Hand | one-handed, 6–10 damage, speed 1.0 |
  | Canvas Satchel | Bag | +6 item slots |
  | Rusty Knife | Main Hand | one-handed, 3–6 damage, speed 0.75; dropped by bandits |
  | Boar Skin | – | crafting material later; dropped by boars |
  | Iron Helm / Cuirass / Gauntlets / Greaves / Sabatons | Head / Chest / Hands / Legs / Feet | 6 / 12 / 4 / 10 / 5 armor (37 for the set) |

  Chest and legs pieces always give more armor than any head, hands, feet or hand item; a test
  checks the whole table.

- `Inventory` lives on the body (`Player::inv`): 7 equip slots (head, chest, hands, legs, feet,
  main hand, off hand), a bar of 4 bag slots, and the item slots: the 12-slot backpack plus one
  container per equipped bag. The player starts with a shortsword and a satchel in the backpack.
- `moveItem` moves or swaps between any two slots when both ends accept what they'd receive: equip
  slots take their own slot's items, bag slots take bags, item slots take anything. A bag leaves the
  bar only when empty, and never into its own slots, so items are never lost. `quickMove`
  (right-click) equips from the item slots or unequips into the first free one.
- No stacks, no item attributes (beyond armor), no dropping or destroying yet.
- **UI:** C opens Character (equipment around an attribute sheet with damage, speed, damage per
  second, regen, armor and its damage reduction against a level 1 enemy); B opens Bags (backpack,
  each bag, the bag bar, free slots). Drag to move, right-click to equip/unequip, hover for a
  tooltip (with armor pieces' "N Armor"). Icons are drawn with ImGui shapes. Any open window frees
  the cursor and pauses mouse look and attacks (a click queued before it opened is dropped, and a
  press that began in a window doesn't attack after it closes); the keyboard still moves the
  player. Esc closes the
  debug window first, then these.

## Entities

- An `Entity` is a kind, a name, a level and a `Player` body (position, animation, health). Fixed
  traits per kind live in one table (`kTypes` in entity.cpp):

  | Kind | Name | Lv | Sta/Str/Agi/Int/Spi | HP | Temper | Weapon | Drops | Talks |
  |---|---|---|---|---|---|---|---|---|
  | Training Dummy | "Training Dummy" | 1 | 10/0/0/0/10 | 100 | harmless (attackable, static) | – | – | no |
  | Villager | random from a name list (seeded) | 5 | 25/5/5/5/5 | 250 | harmless (not attackable) | – | – | yes (E) |
  | Boar | "Boar" | 1 | 10/2/2/2/2 (the player's) | 100 | passive | tusks (= punch, 3–6) | Boar Skin 75% | no |
  | Bandit | "Bandit" | 2 | 12/3/2/2/2 | 120 | aggressive, aggro 10 | Rusty Knife (5–8) | Rusty Knife 40% | no |
  | Wolf | "Wolf" | 1 | 8/2/4/2/2 | 80 | aggressive, aggro 12 | bite (= punch, 3–6) | Wolf Pelt 60% | no |
  | Guard | "Guard" | 4 | 16/5/3/2/4 | 160 | passive | Iron Shortsword (11–15) | – | yes (E) |
  | Bandit Chief | "Bandit Chief" | 3 | 11/1/2/2/2 | 110 | aggressive, aggro 10 | Iron Shortsword (7–11) | Iron Helm 100% | no |
  | Barrow Ghoul | "Barrow Ghoul" | 3 | 7/2/2/2/2 | 70 | aggressive, aggro 8 | claws (= punch, 3–6) | Bone Charm 30% | no |

  The story's foes are tuned for a level-1 player (there's no leveling yet): in the whole Iron set
  with the sword, the chief alone or a pair of wolves or ghouls is won with about a fifth to two
  fifths of the player's health left, a pack of three wolves is lost, and bare-handed without armor
  the chief wins (`storyNpcs` simulates these fights). Higher levels get through more armor, so the
  chief and the ghouls pay for theirs with less health. Per kind there's also a wander radius (villager 12, boar and wolf 10, guard
  and chief 6, bandit 5, ghoul 4), a chase speed (ghoul 4, bandit and chief 5.5, boar and guard 6,
  wolf 7.5 b/s; the player walks 5 and runs 9) and a respawn time (30 s; guard 60, ghoul 45, chief
  120). Against the whole Iron set (armor 37) a wolf hits for 2–3, a ghoul 2–5, the chief 5–8.
- **Models:** the wolf is a quadruped on the boar's animation with its own part sizes (longer
  legs, snout and ears, a tail in the torso's grid); the guard wears the iron helm and gauntlets
  and a tabard over the cuirass; the chief is the bandit's rig drawn 1.25× wide and 1.12× tall; the
  ghoul is a skull-headed humanoid drawn thinner. Only existing palette colors (it's full at 32).
- **Wandering:** alternates standing (2–6 s) and strolling at 2 b/s (2–5 s) in a random direction.
  Before each step it checks the block ahead: no walls it can't step up, no water, no drops of 2+.
  Beyond its wander radius it heads home. NPC bodies use the same `updatePlayer` physics as the
  player, driven by AI input.
- **Temper:** harmless kinds never fight. Passive ones get angry when damaged; aggressive ones
  also when a living player comes within their aggro range. An angry NPC runs at the player and
  swings once within reach (same `startPunch` as the player; the boar's swing is a head-down gore).
  Territory: they only fight a player within 20 blocks of their home (`kLeashRadius`). Once the
  player leaves it, or is knocked out, they calm down and walk back. No line-of-sight check yet.
- **Loot:** a kill rolls the kind's drop into `Entity::loot`. The corpse lies where it fell until it
  respawns; E within 3.5 blocks moves the loot into the bags (it stays on the corpse if they're full).
- **Default spawns:** dummy and villager just in front of the spawn, the boar about 14 blocks out, the
  bandit's camp about 22 blocks out: outside its leash from the spawn, and even at the edge of its
  wander radius more than its aggro range away.
- **Talking:** E picks the nearest talker within 3.5 blocks. It shows its spawn marker's next line,
  or else a random phrase (never the same twice in a row), for 5 s, while it stops and turns to
  face the player.
- **Separation:** the player is pushed out of an entity's 0.8-block radius (never into terrain).
- **HUD:** nameplates (level, name, health bar; red name for aggressive, amber for other attackable,
  green for friendly; red health bar if attackable) within
  40 blocks. They're hidden when terrain blocks the line of sight, using the camera's box march. The
  HUD always runs an ImGui frame; the UI render pass is skipped when there is nothing to draw.
- **From markers:** the NPCs come from spawn markers (see "Markers"); a world without markers
  gets the four above as default markers. Regeneration resets the markers to those defaults; the
  Entities tab respawns every NPC at its marker.

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
| Meshing + upload | ~0.35–1.1 s |
| World file save / load (2.8 MB) | ~22 ms / ~13 ms |
| One brush stroke: edit + remesh + upload | ~1–3 ms |
| Remote screenshot (offscreen render + readback) | ~8–20 ms (960×540 to 1600×900) |
| Command server when idle | one atomic load per frame |
| GPU memory for world quads | ~14 MB |
| Mean frame time (benchmark, vsync off) | ~2–3 ms |

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
| No shadows | Cascaded shadow map for sun/moon (M1) |
| Absolute world-space float positions (jitter far from the origin) | Camera-relative rendering: subtract the camera's chunk origin on the CPU (needed for M1 streaming) |
| Meshing is driven by `Renderer::uploadWorld` | Move meshing into a core job that produces `Mesh`; the renderer only uploads |
