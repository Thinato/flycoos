# Architecture Decision Records

## ADR-001: C++23 + CMake + SDL3 GPU (Metal) for platform and rendering

- **Status:** accepted (2026-11-02)
- **Context:** We need efficient voxel rendering on the dev machine (Apple M4, macOS 15, Command Line
  Tools only, no full Xcode). We also need a path to Windows and Linux, where most RPG/MMO players
  are.
- **Decision:**
  - Language and build: C++23 with CMake ≥ 3.28, Ninja and AppleClang.
  - Window, input and GPU: SDL3, using its GPU API. That API is a thin explicit layer (command
    buffers, render passes, storage buffers, MSAA) that runs on Metal here and on Vulkan/D3D12
    elsewhere.
  - Math: glm. Noise: FastNoiseLite.
- **Alternatives considered:**
  - Raw Metal (metal-cpp): fastest on Mac, but locks out Windows.
  - Vulkan + MoltenVK: about 3× the boilerplate, adds a translation layer on Mac, and MoltenVK has
    no mesh shaders.
  - WebGPU (Dawn / wgpu-native): clean API, but not installed; Dawn is a heavy build and
    wgpu-native means vendoring prebuilt binaries. Remains the fallback.
  - OpenGL: deprecated on macOS and capped at 4.1.
  - An engine (Godot, Unreal, Bevy): the goal is C++ with full control of the voxel pipeline.
- **Consequences:** the GPU API surface is small and explicit. SDL3 also covers windowing, input and
  audio later, so there is one system dependency (`brew install sdl3`).

## ADR-002: Runtime-compiled MSL shaders (for now)

- **Status:** accepted (2026-11-02)
- **Context:** without full Xcode there is no offline `metal` compiler. glslc and spirv-cross are not
  installed either.
- **Decision:** write shaders in MSL (`shaders/world.metal`) and let SDL compile them at load via
  `SDL_GPU_SHADERFORMAT_MSL`. Shader creation failure is logged and the app exits with code 1.
- **Consequences:** zero shader tooling, and compilation takes milliseconds. Shader errors only show
  up at runtime; the `--frames` smoke run catches them. The shaders are macOS-only.
- **Follow-up:** for Windows/Linux (M1), port to HLSL or Slang and compile to SPIR-V/DXIL/MSL at
  build time with SDL_shadercross.

## ADR-003: Greedy meshing + 8-byte packed quads + vertex pulling

- **Status:** accepted (2026-11-02)
- **Context:** voxel worlds are bandwidth- and vertex-bound. A classic float vertex costs about 32
  bytes × 4 vertices per face.
- **Decision:**
  - Greedy-merge faces that share block id and all 4 AO values.
  - Store each merged face as 8 bytes in one storage buffer for the whole world.
  - Draw with `SDL_DrawGPUPrimitives(first_vertex = 0)`, pass the draw's first quad index in its
    uniforms, and rebuild corners in the vertex shader from `vertex_id`. SDL_gpu.h states that a
    non-zero `first_vertex` combined with `vertex_id` is not portable across backends.
- **Alternatives considered:**
  - Per-face instancing: same idea, but more API surface.
  - Mesh shaders: unavailable through SDL GPU, and they would need a compute pipeline.
  - Binary greedy meshing (bitmask): faster meshing, but more complex. We'll revisit when streaming
    needs remeshing budgets.
- **Consequences:** about 16× less vertex data than float vertices, ~1.8M quads = 14 MB for the
  whole world, and frame times around 1 ms. Region size is capped at 64 per axis (6-bit fields);
  chunks use 32.

## ADR-004: Render to an owned resolve target, then blit to the swapchain

- **Status:** accepted (2026-11-02)
- **Context:** the swapchain texture cannot be read back, but we want screenshots for automated
  visual verification.
- **Decision:** resolve MSAA into our own texture, blit it to the swapchain, and download it on
  request (`--screenshot`).
- **Consequences:** one extra full-screen blit per frame (negligible on M4). Screenshots work
  headlessly in CI-like runs.

## ADR-005: Header-only dependencies through FetchContent, without their CMake

- **Status:** accepted (2026-11-02)
- **Decision:**
  - glm 1.0.3 and FastNoiseLite v1.1.1 are fetched as pinned release tarballs.
  - `SOURCE_SUBDIR` points to a missing directory, so only the headers are used. Their CMake
    projects, tests and install rules never run.
  - Both are marked `SYSTEM`, so their warnings don't mix with ours.
- **Consequences:** the first configure needs network access. There are no extra system packages.

## ADR-006: Dear ImGui for the debug UI

- **Status:** accepted (2026-11-02)
- **Context:** we need an in-game debug window with tabs, sliders, checkboxes, text input and
  graphs for tuning movement, inspecting meshes and controlling time and world generation.
- **Decision:** Dear ImGui v1.93.0, using its SDL3 platform backend and SDL_GPU renderer backend.
  It's compiled from a pinned, hash-checked tarball as a separate static library, so our warning
  flags don't apply to it. The UI is drawn in a second render pass into the resolve target, before
  the blit.
- **Alternatives considered:**
  - Hand-written UI on our renderer: weeks of work for widgets ImGui already has.
  - Separate tool windows or a web inspector: more moving parts, and no in-world context.
- **Consequences:**
  - ImGui's colors are authored in sRGB. Our swapchain is sRGB with linear-light output, so the
    style colors are converted to linear at startup.
  - Screenshots include the UI when it's open.
  - An ImGui frame runs every frame because the in-game HUD draws into it. The UI render pass is
    skipped when there's nothing to draw.

## ADR-007: Classless WoW-style attributes

- **Status:** accepted (2026-11-02)
- **Context:** combat needs stats, and the game has no classes.
- **Decision:**
  - Five attributes (stamina, strength, agility, intellect, spirit); everything else derives from
    them directly, with no attack power or other in-between step.
  - Stamina gives 10 health per point.
  - Physical damage is `baseDamage + strength * speed`, using the attack's base speed, as WoW
    uses weapon speed.
  - Agility is haste.
  - Spirit regenerates health out of combat.
  - Intellect waits for magic.
- **Consequences:**
  - All formulas and tuning constants live in `src/stats.h`.
  - Equipment can later add attributes and replace the punch's `Attack` without changing the
    formulas.

## ADR-008: Inventory as bags of fixed slots

- **Status:** accepted (2026-11-02)
- **Context:** the player needs an inventory that can grow, and equipment that changes combat.
- **Decision:**
  - A 12-slot backpack that is always there, plus 4 bag slots; each equipped bag adds its own
    slots (WoW-style). Items are plain ids into one table; no stacks yet.
  - Equipment and bags live on the body (`Player::inv`), so NPCs can carry gear later.
  - One move rule (`moveItem`) serves drag and drop, right-click and tests: both ends must accept
    what they'd receive, and a bag must be empty to leave the bar.
  - The game windows use Dear ImGui like the debug UI, with icons drawn from shapes rather than
    textures.
- **Consequences:**
  - The main-hand item's `Attack` replaces the punch; the damage formulas didn't change.
  - Gear doesn't add attributes yet. When it does, health, regen and swing time must read
    base + gear stats instead of `Player::stats` directly.
  - Each held item needs its own model slot (`ModelSword` today).

## ADR-009: NPC temper, territory and corpse loot

- **Status:** accepted (2026-11-02)
- **Context:** the first hostile NPCs: a passive boar and an aggressive bandit, both with loot.
- **Decision:**
  - One `Temper` per kind (harmless, passive, aggressive) and an `angry` flag per entity, instead
    of a state machine. Anger comes from being damaged, or for aggressive kinds from a player in
    aggro range.
  - Fighters own a territory: the leash is measured from their home to the player, not from home
    to the NPC. Aggro and calm-down use the same test, so an NPC can't flap at the boundary.
  - NPC attacks reuse the player's swing (`startPunch`, impact frame, `inReach`) and damage formulas.
  - Loot is rolled at the kill and kept on the corpse until it respawns; E takes it.
  - A knocked-out player respawns at the world spawn after 5 s, with no penalty yet.
- **Consequences:**
  - New fighters are table rows. A creature with a different shape needs its own part layout and
    animation in player.cpp, as the boar has.
  - Chasing walks straight at the player: water, cliffs and walls stop it (no pathfinding).
  - Only the player can be a target; NPCs don't fight each other.

## ADR-010: Armor as flat mitigation by attacker level

- **Status:** accepted (2026-11-02)
- **Context:** worn gear should protect the player, more for some pieces than others, and
  protection should matter less against stronger enemies.
- **Decision:**
  - Each item has a flat armor value; a body's armor is the sum over its equipped items. Chest and
    legs pieces always give the most (a test enforces it over the item table).
  - A hit is reduced by `armor / (armor + K x attacker level)`, capped at 80% (rounded, at least 1
    damage). The cap keeps stacked armor from making the player near-immune; the reduction never reaches 100%, and the same armor is worth less against higher levels without a
    separate level-difference rule.
  - K is one global constant (`kArmorK` = 40), tunable live from the debug UI and passed into
    `updateCombat`, not stored in a mutable global.
  - Armor is applied where a swing lands (`updateCombat`), so debug damage bypasses it.
  - Armor shows on the model: worn pieces swap in plated parts per part (`ModelInstance::parts`).
- **Consequences:**
  - Tuning K rebalances every armor value at once.
  - The player has no level yet and counts as level 1; NPCs wear nothing, so armor only protects
    the player today.
  - Gear still adds no attributes; when it does, health, regen and swing time must read
    base + gear stats (see ADR-008).
  - Each visible armor piece needs plated parts in a model; greaves have no part to show on.

## ADR-011: World files and data-driven edits with snapshot undo

- **Status:** accepted (2026-11-02)
- **Context:** the story world is hand-authored: generated once, then sculpted in the running game
  by the user and, later, by Claude through remote commands. Edits must persist, be undoable and
  share one entry point.
- **Decision:**
  - One versioned file per world (`.flyw`): header (magic, version, size, seed), tagged sections,
    and a trailing FNV-1a checksum. Voxels are one section of per-chunk run-length encoding with
    varint run lengths. Unknown sections are skipped, which leaves room for prefab instances and
    story markers. Saving goes through a temp file and a rename.
  - Edits are plain data (`EditOp`): one function applies any op and returns the exact box of
    changed voxels; the caller remeshes that box.
  - Undo records are snapshots of the dirty box before the op. Undo/redo swap them with the grid.
  - Caps (brush radius 32, strength 64, Fill 128³ voxels, 256 undo steps) keep one op to
    milliseconds and history memory bounded.
- **Alternatives considered:**
  - zlib/zstd compression: a new system or built dependency for little gain; the RLE file is
    already ~2.8 MB, and git compresses objects itself.
  - Per-voxel undo deltas or inverse ops: smaller for sparse edits, but inverses of brushes are not
    exact (clamping, dither, water fill); a cropped snapshot is exact by construction.
  - Diffing the whole world for the dirty box: simple but ~200 MB of compares per op.
- **Consequences:**
  - The dirty box is computed by diffing the snapshot, so it is tight and correct for any op, and
    a test can check that nothing changed outside it.
  - A remote-command dispatcher (Phase 2) parses into `EditOp` and calls `applyEdit`, `undoEdit`,
    `redoEdit`, `saveWorld`, then `Renderer::updateWorld(dirty)`.
  - A corrupted or truncated file is rejected with a message; a `--world` file that doesn't load
    stops the game instead of generating a world that a save would write over it.

## ADR-012: Per-chunk slots in the world buffers for incremental remeshing

- **Status:** accepted (2026-11-02)
- **Context:** `uploadWorld` meshed and uploaded the whole world (~0.4–1.1 s). Edits need to show
  within a few milliseconds without changing how frames are drawn.
- **Decision:**
  - Keep one opaque and one water storage buffer. Every chunk owns a slot (first quad, count,
    capacity) in each; draws still pass `firstQuad` in their uniforms, so shaders and draw calls
    are unchanged.
  - After an edit, remesh the chunks touching the dirty box grown by 1 voxel. New quads overwrite
    the slot when they fit, else go to a new slot (+25% spare) in headroom at the buffer's end.
    All uploads of one edit share one transfer buffer and copy pass.
  - When the headroom is used up, repack every slot from a CPU copy of the chunk meshes into fresh
    buffers (~10 ms, rare).
- **Alternatives considered:**
  - One GPU buffer per chunk: simplest allocation, but ~2,500 storage-buffer binds per pass.
  - A general free-list allocator: reuses holes, but more code than a rare repack.
  - Re-uploading the whole buffer per edit: ~14 MB per brush application.
- **Consequences:**
  - An edit touching a few chunks applies in 1–3 ms; frame rendering is unchanged.
  - The CPU keeps a copy of every chunk mesh (~14 MB) for repacking.
  - The chunk list holds all chunks, empty ones too; empty ones are skipped before culling.

## ADR-013: A localhost command server in the game and a dependency-free MCP bridge

- **Status:** accepted (2026-11-03)
- **Context:** the user and Claude shape the story world together: Claude (Claude Code) edits and
  looks at the world while the user watches the running game and edits alongside it. Claude's edits
  must land in the live world, in the user's undo history, and show up immediately.
- **Decision:**
  - The game listens on 127.0.0.1 (default port 47800) for newline-delimited JSON commands. A
    background thread does only socket I/O; commands run on the main thread at one point of the
    frame, through the same `applyEdit`/`undoEdit`/`updateWorld` path as the editor. One command
    (including `apply` and `path`) is one undo step (`applyEdit`'s `join`).
  - The dispatcher (`runCommand`) is core-library code: JSON in, JSON out, plus the work only the
    game can do (remesh box, save, screenshot camera, goto) returned as data. Tests run it without
    sockets or a GPU.
  - `tools/flycoos_mcp.py` is a stdio MCP server in plain Python (no packages): it implements the
    JSON-RPC subset that tools need and maps each tool to one game command. `.mcp.json` registers it.
  - nlohmann/json 3.12.0 and stb_image_write (pinned commit) are added as header-only dependencies
    the ADR-005 way.
- **Alternatives considered:**
  - MCP inside the game (stdio or HTTP transport): Claude Code would start and own the game process
    over stdio, so the user couldn't launch, restart or keep it across Claude sessions; an HTTP MCP
    transport means sessions, SSE and protocol upgrades in C++. A socket plus a 250-line bridge keeps
    the game's side to one small line protocol that any script can drive.
  - The official MCP Python SDK: a pip install and a virtualenv for every user, for four methods.
  - Running commands on the server thread with a world lock: the renderer, the editor and the
    simulation would all need the lock; one handoff per frame needs none.
  - A screenshot of the window: shows the HUD and moves the user's camera; an offscreen pass doesn't.
- **Consequences:**
  - No authentication: anything on the machine can edit the open world while the game runs. It's a
    development tool, bound to localhost only, and `--no-serve` turns it off.
  - Web pages are the remaining remote reach (a cross-origin `text/plain` POST needs no preflight),
    so the server closes a connection at the first line that isn't a JSON object, and `save` writes
    only `.flyw` files in `worlds/` or the world file's folder.
  - Hostile input must not take the game down with unsaved edits: invalid UTF-8 is replaced when
    serializing, nesting is capped at 64 levels, every exception becomes an error response, and one
    edit command has a work budget (≲50 ms). Undo history is capped at 256 MB as well as 256 steps.
    A silent client is dropped after 5 s so it can't hold the single slot.
  - At most one command per frame (~120 per second), and none while the window is minimized.
  - The idle cost per frame is one atomic load; a screenshot stalls the frame for its render.


## ADR-014: Prefabs as recorded instances; structures as text files

- **Status:** accepted (2026-11-03)
- **Context:** the user and Claude author a story world by hand and want ready-made trees, rocks,
  houses and camps that can be placed, moved and removed later, by either of them, without
  undoing everything placed since.
- **Decision:**
  - Two prefab sources behind one place/remove/move interface: generated prefabs call the same
    `grow*` functions as terrain generation (so terrain stays bit-identical, pinned by a test);
    structures are hand-writable text files in `prefabs/` (header, legend, layers; `.` clears,
    space keeps), parsed strictly with line numbers and rescanned on demand.
  - Placing stamps voxels into the grid and records an instance: id, prefab, anchor, rotation,
    seed, params and every voxel it changed with its old and new value. Removing restores the old
    value only where the voxel still holds the new one. Instances are saved in an `INST` section.
  - Undo records carry instance changes next to their voxel parts and swap both, so one step
    always restores voxels and the instance list together.
  - Four blocks appended after `Plate` (Planks, Cobble, Thatch, Ember), filling the renderer's
    32-entry palette exactly; tents use the existing Burlap as canvas.
- **Alternatives considered:**
  - Baking prefabs into the voxels only: no move or remove after later edits.
  - Re-stamping instances from their definitions on load (store only the placement): smaller
    files, but a changed prefab file or generator would silently change the world, and removal
    couldn't know what was under it.
  - One world-independent voxel buffer for generated prefabs too: their rules (trunks reach the
    ground, canopies only fill air) depend on the world, and terrain would no longer be
    bit-identical.
  - JSON or an image format for structures: harder to write and diff by hand than a text grid.
- **Consequences:**
  - An instance costs 8 bytes per changed voxel in memory, in the file and in undo history.
  - Editing a prefab file changes only later placements and moves; placed instances keep their
    voxels and can always be removed, even when the file is gone.
  - Another block needs a bigger palette (shader and uniform change, with a perf A/B).

## ADR-015: Story markers in the world file; NPC spawns derive from them

- **Status:** accepted (2026-11-03)
- **Context:** the user and Claude write a story and shape its world together. Where the player
  starts, where NPCs live and what places are called were hard-coded offsets from the world spawn.
- **Decision:**
  - A marker is plain data (id, kind, name, position, radius, entity, count) of four kinds:
    player spawn, NPC spawn, point of interest, region. They live in a `MARK` section of the world
    file and in the undo history next to voxels and instances, so one undo restores both.
  - NPCs are derived state: each remembers the marker it came from, and `syncSpawns` respawns a
    marker's NPCs when the marker changed. Undo, redo, commands and the editor never touch NPCs
    directly.
  - Worlds without a marker section (all earlier files) and new worlds get the four old spawns as
    ordinary markers, seeded so the same NPCs appear; from then on they're editable like any other.
  - Region and poi names are drawn on ImGui's background list (HUD and editor), not in the world
    renderer; the map image marks them with ids, and the JSON carries the names.
- **Alternatives considered:**
  - Keep the hard-coded spawns when a world has no spawn markers: the first spawn marker the user
    adds would make the old NPCs vanish.
  - Patch NPCs from each edit's result (ids changed): undo and redo would need the same plumbing;
    comparing against the markers after any change is one function and can't drift.
  - Markers as prefab instances: they have no voxels, and instances can't carry kind-specific data.
- **Consequences:**
  - Editing a spawn marker resets its NPCs (health, loot, position); the others keep their state.
  - Spawn counts add up to at most 256 NPCs per world, since every NPC is simulated each frame.
  - A marker edit remeshes the chunks around one voxel (~1 ms) because that is how the game learns
    the world changed; the cost is negligible next to a brush.
  - Removing every spawn marker leaves the world without NPCs, by design.

## ADR-016: Story NPCs: dialogue lines on spawn markers, new kinds as table rows

- **Status:** accepted (2026-11-03)
- **Context:** the first story act needs named characters who say specific things (an elder, a
  smith, a bandit leader who taunts), and enemies beyond the boar and bandit, without a quest or
  dialogue system yet.
- **Decision:**
  - A spawn marker carries up to 8 lines (1–160 bytes, validated like names). Talkers say the next
    one on each E, cycling; anyone that fights (guards too) says the next one when it turns angry,
    at most every 8 s. Lines are refused only on other marker kinds and the dummy, a fixed rule
    rather than one derived from NPC traits, so saved worlds keep loading when traits change.
  - A spawn with lines and count 1 is a named character: its NPC's display name is the marker's
    name. The character is one marker; there's no separate NPC record.
  - Lines live in their own `LINE` world-file section keyed by marker id, written only when some
    marker has lines. `MARK` keeps its layout, so files stay loadable both ways (older builds skip
    `LINE`) and no version bump is needed.
  - New lines, like a new name, update the NPCs' copy of the marker without respawning them;
    undo works because lines are part of `Marker`.
  - Wolf, guard, bandit chief and barrow ghoul are rows in `kTypes` plus models built from the
    existing rigs and palette. Armor, damage and the player's stats are unchanged.
  - With no leveling yet, the story's foes are tuned so a level-1 player in the Iron set with the
    sword wins: the chief 1v1 with 15–35% health left, a pair of wolves or of ghouls; bare-handed
    and unarmored they lose to the chief, and a pack of three wolves still wins. A test simulates
    these fights. Levels show danger and armor penetration, health and strength set the balance:
    wolf Lv 1 (80 HP), chief Lv 3 (110 HP, his sword does the damage), ghoul Lv 3 (70 HP, claws).
    The guard (Lv 4, passive) isn't meant to be fought.
- **Alternatives considered:**
  - Lines inside `MARK` behind a version bump: every older build would refuse the new files.
  - Stats on a strict level curve (+2 stamina, +1 strength per level): a level-4 chief and
    level-3 ghouls with knives beat a geared level-1 player every time.
  - Ghouls with knives: the knife's quick swings made any pair of level-3 ghouls unwinnable.
  - Lines only for kinds that talk: a bandit leader couldn't be named or taunt.
  - A dialogue tree or quest state: not needed for one act told through lines, regions and labels.
  - A new palette color for the wolf: the palette is full (32), and growing it needs a shader
    change and a perf A/B. Plain greys read blue in the sky light, so the wolf wears brown on top.
- **Consequences:**
  - Dialogue is linear and stateless: no branching, no memory of what was said across respawns.
  - Lines cost at most 8 × 160 bytes per marker (~5 MB in the worst case of 4096 markers); there's
    no total cap.
  - Once the player can level up, these foes need retuning (the fight test will say by how much).
  - Lower stats than their levels suggest: a level-3 chief has less health than a level-2 bandit.

## ADR-017: A fixed 60 Hz simulation tick with interpolated drawing

- **Status:** accepted (2026-11-03)
- **Context:** `step()` took the frame's variable `dt`. Fights, AI and physics then differ with the
  frame rate, and a replay or scripted test can't reproduce a play session. Quests, saves and
  balance tests (ADR-016's fight test) need "N ticks of input X → exact state".
- **Decision:**
  - The game calls `step(session, input, kTick)` with `kTick` = 1/60 s, as many times per frame as
    an accumulator of frame time covers. Frame time is clamped to 0.25 s first, so a stall costs
    at most 15 ticks.
  - Bodies keep `prevPos`/`prevYaw`, copied at the start of each tick. Drawing (models, nameplates,
    damage numbers, the camera target) interpolates by the accumulator's leftover fraction.
    `Player::teleport()` sets both for moves that mustn't be drawn as a slide.
  - One-shot inputs (attack click, E) are latched in `main.cpp` until a tick consumes them.
  - Camera smoothing, editor brushes, the command mailbox (ADR-013) and the UI stay on frame time.
    Animation state stays inside the bodies and so advances per tick.
  - Benchmarks run exactly one tick per frame (game and camera), so runs are deterministic and
    comparable across machines and builds.
- **Alternatives considered:**
  - Variable `dt` with a cap: simple, but nothing is reproducible, and fast frames (e.g. 1000 Hz)
    stress the physics edge cases.
  - Fixed tick without interpolation: at 120 Hz displays every other frame repeats, which reads
    as judder.
  - Extrapolating from velocity instead of interpolating: no lag, but overshoots on every stop,
    hit and wall.
  - 30 Hz or 120 Hz: 30 shows in the punch timing (impact frames 33 ms apart); 120 doubles the cost
    of AI and combat for NPC counts we want to grow.
- **Consequences:**
  - What's drawn trails the simulation by up to one tick (17 ms).
  - Animation poses update at 60 Hz even on faster displays; only position and facing are smoothed.
  - Below 4 fps the game runs slower than real time.
  - Benchmark screenshots changed once: 300 frames now cover 5 s of game time at any frame rate.
  - Anything that moves a body outside the simulation must use `teleport()`, or it may be drawn
    partway for up to one tick.
