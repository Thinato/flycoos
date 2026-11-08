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

- **Status:** accepted (2026-11-02); amended by ADR-022 (gear and later talents add modifiers on top
  of the attributes; the formulas take the computed stat sheet)
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

- **Status:** accepted (2026-11-02); amended by ADR-022 (a Trinket equip slot; gear adds attributes,
  and health, regen and swing time read base + gear)
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

- **Status:** superseded by ADR-020 (2026-11-03); accepted (2026-11-02)
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

- **Status:** accepted (2026-11-02); amended by ADR-022 (armor is a flat modifier in the stat
  pipeline, applied in `resolveHit`; the player has a level, 1 for now)
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

## ADR-018: Data definitions in JSON, keyed by string ids, with append-only save ids

- **Status:** accepted (2026-11-03)
- **Context:** adding an item or NPC kind meant editing a compiled-in table (`kItems`, `kTypes`)
  plus the enum, the UI and the tests. Quests, skills and talents will add more tables that refer
  to each other, and tuning should not need a recompile. Inventories, spawn markers and future
  saves store numeric ids, so moving tables to data must not renumber anything.
- **Decision:**
  - One JSON file per definition type in `data/` (`items.json`, `creatures.json`), each
    an array of records with a required snake_case string `id`. nlohmann/json is already a
    dependency of the core.
  - A two-pass loader (`loadDefs` in `defs.cpp`): pass one parses every file into `id → json`;
    pass two resolves each type in reference order (items before creatures), keeping an
    `id → save_id` map per type so later types can reference earlier ones, and range-checks every
    field. Unknown or duplicate ids, duplicate JSON keys (rejected through the parser callback;
    nlohmann would keep the last), unknown fields, wrong types, out-of-range values and display
    names with control, bidi or zero-width characters are hard errors naming file, record and
    field. The game won't start on one; runtime code never sees strings.
  - `copy_from` names one parent; its fields (all but `save_id`) are merge-patched with the
    record's own (RFC 7386: objects merge deeply, `null` removes a field). Chains stop at 16, which
    also catches cycles.
  - Each record has an explicit `save_id` equal to its old enum value. It is the handle code holds
    (`ItemId`) and what saves store: append-only, unique, no gaps, never renumbered or reused. A
    retired item keeps its record. The `ItemId` constants code names (`Shortsword`, `IronHelm`)
    stay as save ids, so callers didn't change; data may add ids after them. A table in
    `defs.cpp` pairs each constant with its string id, and loading fails on a mismatch.
  - Creatures (step 6) work the same way. Their save ids are the `EntityKind` values, from 0
    (the `u8` spawn markers store), so every `.flyw` loads unchanged; the constants code names
    keep their string ids in `kEntityKindNames` (the names markers and MCP already used), which
    loading checks. A creature names its `weapon` and `loot` by item id (resolved through the item
    pass), its `model` by name (painters stay code; an unknown name is an error) and its temper by
    name; `name: null` means random villager names. `checkMarker` accepts any loaded kind, so data
    may add kinds after `Count`.
  - The MCP bridge stops duplicating game data: a `catalog` command returns the loaded kinds with
    their traits and the material names, and the bridge builds its enums and kind descriptions
    from it, fetched on every tools/list. Without an answer it lists the tools without those
    enums and sends `tools/list_changed` after the next tool call that reaches the game.
  - Data is found in `data/` in the working folder (`dataDir()`), like `prefabs/`, else in the
    source tree (`FLYCOOS_SOURCE_DIR`, a compile definition of the core), so a build runs from any
    folder. A `core_test` case loads every
    shipped file and asserts zero errors (the CI-less stand-in for a schema), next to the pinned
    table values.
  - The debug window's Data tab reloads the files between ticks (`reloadDefs`). It keeps the old
    registry when loading fails, when the new files have fewer items or creatures than are loaded,
    or when a loaded save id would get a different string id: live inventories and spawn markers
    hold those ids.
  - `Block` stays a C++ enum: the 32-entry palette is full, the shader duplicates `kWater`, and
    generated terrain is pinned by hashes. Model painters and item icons stay code, referenced by
    id, until voxel model files are worth authoring.
- **Alternatives considered:**
  - A JSON Schema validator (pboettch/json-schema-validator): not header-only, "work in
    progress", and the loader's checks already cover references, types and ranges.
  - Dense indices assigned in file order: reordering the file would scramble every saved inventory.
  - Lua definition tables (Luanti-style) with behavior callbacks: an interpreter is a large step
    that only quests or abilities with real scripting would justify.
  - `relative`/`proportional`/`extend` on `copy_from` (CDDA): no record needs them yet; merge
    patch covers overriding one nested field and removing one.
  - `std::string` names in the type structs instead of pointers into the registry: the pinned
    table tests read `name` as `const char*`, so the registry fixes the pointers up once per load.
- **Consequences:**
  - Tuning an item is a JSON edit and a Reload defs; adding one needs an icon and model in code to
    look right (it works without them).
  - Adding an NPC kind is one record when it reuses a body model and existing items: the editor,
    the `mark` command and the MCP schemas read the loaded kinds. A new body still needs a `Model`
    and painter in code, and a kind code refers to needs an `EntityKind` constant.
  - Living NPCs copy some traits at spawn (name, level, stats, wander radius, chase speed, weapon),
    so a reload changes those only for NPCs spawned after it.
  - The registry is a global the item functions read; swapping it mid-tick would be unsafe, so only
    the startup and the debug UI set it.
  - Reordering records changes nothing; deleting one leaves a gap that fails loading instead of
    shifting ids.

## ADR-019: Voxel light: stored sky and block nibbles, relit by whole columns, one value per quad

- **Status:** accepted (2026-11-03)
- **Context:** `light()` had hemisphere ambient × AO plus sun N·L and no shadow or sky-visibility
  term, so a roofed cottage or the barrow at noon was lit like an open field, and `Ember` (barrow
  torches, campfires, hearths) lit nothing at night. The research notes
  (`docs/research/notes/voxel_lighting.md`) point to Minecraft-style flood-fill light and left the
  memory, fill time and quad-count costs unmeasured.
- **Decision:**
  - Two 0..15 channels per voxel (`light.h`): sky light (15 with only air above, spreading at a
    cost of 1 per voxel of air or leaves and 2 of water; only air passes 15 straight down) and
    block light (`Ember` emits 13, same costs). Light never depends on the time of day; the shader
    scales it, so day/night stays a uniform change with no remesh.
  - **Stored**, one byte per voxel in lazily allocated 32³ chunk arrays (a chunk with one value
    holds just that value). Measured on the story world: 47.3 MiB in 1,515 of 6,144 chunks
    (dense: 192 MiB), filled in 0.16–0.20 s on all cores, ~19 MiB transient peak.
  - **Relit by whole columns**: an edit recomputes the columns within 15 of it, every height, from
    the voxels, seeded by the unchanged light around them. Farther light changes by ≥ 16 on the way,
    so the result is exact (tested against a full fill), and the fill and the update are one code
    path with no removal bookkeeping. It returns the box whose light changed; `updateWorld`
    remeshes it with the edit. Small edits relight in ~1.1–1.5 ms.
  - The mesher packs the light of the voxel in front of each face into 8 of `Quad.b`'s 13 free bits
    and keys the greedy merge on it. AO stays per vertex, the quad stays 8 bytes, and the light
    reaches the fragment shader inside the existing flat output (block id now 5 bits): no new
    varyings. Models get one light value per instance from where they stand.
  - Shader: `o = AO × curve(sky)` scales ambient and the cool tint; sun/moon diffuse fades in over
    sky light 0.6..1 (`smoothstep`), the shadow stand-in; ember light adds `emberColor ×
    curve(block)` on the untinted albedo, faded where the sky's light is brighter, so it glows at
    night and indoors without tinting sunlit ground (a per-channel max of the two turned firelit
    floors purple). Defaults were picked by screenshot A/B; the Render tab tunes all of them.
  - Perf A/B (story world, fixed look, 4 alternating runs): opaque quads 1,772,666 → 1,863,601
    (+5.1%), mean frame 1.88 → 1.99 ms (+6%), p99 2.73 → 2.86 ms, process RSS +~60 MiB. The old
    shader on the new meshes runs as fast as the new one, so the cost is the extra quads, not the
    shading.
- **Alternatives considered:**
  - Recomputing light at mesh time per chunk with a 15-voxel halo (Veloren): no storage, but each
    chunk floods (32+30)³, ~7.3× its volume, the full mesh would take several times longer, and an
    edit must remesh every chunk within 15 (up to 27) to avoid seams.
  - BFS add plus two-phase removal on stored light (Minecraft, the 0fps article): touches only the
    lit voxels, but two algorithms and their edge cases (opaque placement as removal, sky columns)
    for edits that already take milliseconds to remesh.
  - A dense light grid: simplest indexing, but another 192 MiB.
  - Smooth per-corner light: 32 more bits per quad (a third word) or a player-centred 3D light
    texture sampled per fragment. Stepped light reads fine on blocks; the texture is the upgrade.
  - Shadowless point lights in a uniform array: no remesh, but they leak through walls.
  - Additive ember light (the plan's first sketch): sunlit stone around a shrine turned pink.
- **Consequences:**
  - Interiors, the barrow, cave mouths and the ground under trees are darker and cooler by day; the
    village's hearths glow through windows at night.
  - Sun light has no direction: under an overhang at low sun the gate is wrong. A shadow map
    replaces the gate, not the sky light.
  - Light is derived data, never saved: loading or generating a world fills it, and every edit path
    (brushes, fill, prefabs, markers, undo/redo, remote commands) reaches it through
    `Renderer::updateWorld`.
  - Light lives in the renderer today because only meshing reads it; gameplay that needs it (spawn
    rules by darkness) moves it next to the grid.
  - Changing `lightCost`, `emission` or the curve changes meshes and screenshots; `core_test` pins
    the falloff and the lit seed-7 quad count.
  - Later (2026-11-03): the palette grew to 64 entries (1 KiB of the per-frame uniforms) and the flat
    output's block id to 6 bits (light bits 6-13), leaving the greedy-quads view an 18-bit quad index
    (its colors repeat past 256K quads; debug only). `Quad.b` already held 8 bits of block. Seven
    building blocks were appended (Brick … Slate, and `Lantern`, an emitter of 11). Perf A/B against
    the 32-entry build, story world, fixed look, 7 quiet alternating pairs: mean 2.00 vs 2.01 ms,
    p99 median 3.1 vs 3.3 ms, identical quad counts: no measurable cost.

## ADR-020: NPC AI as one explicit state machine with per-kind numbers

- **Status:** accepted (2026-11-03); supersedes ADR-009's `angry` flag, its shared aggro/calm test and
  its global leash
- **Context:** `updateEntities` was a state machine encoded in an `angry` bool, two timers and an
  if-chain. Aggro and calm-down tested the same player-to-home distance, so a player standing on
  the leash edge flipped `angry` every tick. A chase stalled by terrain waited forever. Debugging
  "why did it stop chasing?" meant reading code. Quests, time-of-day spawns and schedules will
  hang off NPC behavior, so it needs named states first.
- **Decision:**
  - One C++ enum FSM for every kind: `Idle` (doesn't wander), `Wander`, `Talk`, `Chase`, `Windup`,
    `Recover`, `Return`, `Flee`, `Dead`. `setState` records every transition with a static
    `reason` string and the time spent in the old state in an 8-entry ring per entity, runs the
    exit action (leaving `Windup` early cancels its hit) and the enter actions (entering `Chase`
    from calm shouts the spawn marker's next line, at most every 8 s: ADR-016's rule; entering
    `Return` drops pending events). A state forced from the debug window (reason `forced`) skips
    the enter actions. One update may chain transitions (a hit from beyond the leash is `Chase`
    and straight back to `Return`) before the final state produces the input; a chain that doesn't
    settle in 8 is a bug (an assert in debug builds). The graph is code; only numbers are data.
  - Per-kind numbers in `creatures.json`, all optional and only for creatures that fight:
    `leash_radius` (20, the old constant), `reaggro_radius` (0.8 × leash, must be smaller),
    `reaggro_cooldown` (3 s), `give_up_seconds` (5), `attack_windup` (0.14 s, the old impact frame),
    `attack_recover` (0.2 s), `attack_cooldown` (0) and `flee_health` (0: never). No shipped kind
    sets any, so the defaults are the shipped tuning. The windup must be shorter than 0.4 × the
    kind's swing time (where the swing animation puts its impact), and a kind that fights must
    wander (`Idle` kinds never move, so they only die and come back: reasons `respawned`, `stands`).
  - Hysteresis: `Chase` ends when the player is `leash_radius` from home, knocked out, or the NPC
    couldn't step toward an out-of-reach player for `give_up_seconds`. Out of reach is the swing's
    test: too far, or a body height or more above or below (a player up a pillar), so a stalled
    chase always ends. `Return` walks home ignoring the player and hits, heals fully on arrival
    (within its wander radius) if it had to walk there (one that gives up at home keeps its
    wounds, so poking it from beyond the leash doesn't heal it), and starts the re-aggro cooldown. Attacking on sight needs the player within `reaggro_radius` of home, so the band
    between the two radii never re-aggroes. A hit always does (passive until hit, as before).
  - `Windup` is committed: started by `startPunch` with the kind's windup (the swing animation
    stretches to put its impact frame there), no moving or turning, and the hit lands once at its
    end in `updateCombat` only if the target is still in reach and in front: the player's dodge
    window. Cadence and damage are unchanged: the weapon's swing time still gates the next swing.
  - Inputs arrive as an event bitmask (`EventDamaged`) set by `damage()` and read once by the
    entity's next update, so "passive until hit" is a single transition.
  - Targets: the AI reads its target as a `const Player&` body (any combatant: position, health),
    and hits land through the same `inReach`/`hurt` the player uses. "Only the player can be a
    target" is no longer a design rule, but the target is still always the player: NPC-vs-NPC
    fights need a target handle that survives `syncSpawns` and a target choice, and aren't built.
  - The debug window's Entities tab shows state, time in it, target distance, the kind's AI
    numbers and the last 8 transitions with reasons, and forces a state from a combo (the AI
    leaves an impossible one at once, e.g. `Dead` while alive).
- **Alternatives considered:**
  - Behavior trees or utility AI: they pay off for multi-step routines and graded choices, not for
    one melee loop shared by every creature. Villager schedules (M4) can be a separate layer that
    combat preempts.
  - A data-driven graph (states and guarded transitions in JSON): no two kinds need different
    topologies yet; a boolean trait per kind covers the first that does.
  - Leashing on the NPC's distance from home as well: the chase follows a player inside the leash,
    so the player's distance bounds the NPC's; `give_up_seconds` covers the stuck cases.
  - An `ally_attacked` event (pack wolves, camps joining in): cheap to deliver, but deciding who
    joins changes Act 1's fights; it waits for a kind that wants it.
  - FOV and line of sight for aggro: no LOS ray yet (perception at 2–5 Hz belongs with it).
- **Consequences:**
  - The leash-edge flapping is gone (a test holds a player on the edge for 10 s: at most three
    transitions). A player between the re-aggro and leash radii is safe from aggressive kinds
    that didn't already chase them, a 4-block band the old shared test didn't have.
  - A hit is read one tick later than before (the shout too). Tests that checked `angry` right
    after `damage()` now run one update first.
  - The committed windup changed one characterization value: `boarReplay`'s chase distance
    (13.75 → 13.65; the boar no longer runs during a swing). Every hit frame and damage in
    `fightReplay` and `boarReplay` stayed the same. Act 1's fight balance moved within its band
    (ghouls 30 → 27 health left on average; chief 28 and wolves 36 unchanged).
  - A returning NPC can be hit freely, and a ranged or kiting player could farm it. Accepted for
    now (the classic evade, paid back by the full heal at home); revisit with ranged attacks, e.g.
    with damage immunity or a faster walk while returning.
  - Adding behavior means a state or a transition in `think()` (`entity.cpp`) plus its reason;
    tuning means a JSON number and Reload defs.

## ADR-021: Prefab-placed markers, owned by their instance; brushes keep natural ground on top

- **Status:** accepted (2026-11-03)
- **Context:** three editor gaps from authoring the first act. A camp should bring its own
  bandits instead of needing a spawn marker placed by hand next to it and kept in step when the
  camp moves. Flattening under a canopy spread Leaves over the flattened ground, because the brushes
  took the column's top block as its surface. And `fill`/`paint` over MCP (and the panel) offered
  only the ten natural blocks, though prefab files could already use every block.
- **Decision:**
  - A structure file may declare markers in its header: `marker: <spawn|poi|region> x=N z=N
    [entity=] [count=] [radius=] [name=<rest>]` at a cell of the layers, with `line:` lines for
    dialogue, checked by `checkMarker` when the file is parsed. No `player_spawn` (one per world).
  - `Marker::owner` is the instance whose placement made the marker (0: placed by hand).
    `placePrefab` makes the prefab's markers (standing on the bottom layer, turned with the
    footprint, new ids) in the same undo step as the voxels and the instance; `movePrefab` carries
    the owned ones rigidly and `removePrefab` takes them out, both in the same step. Undo and redo
    already swap marker entries, so ownership comes back with them. `putMarker` never changes an
    owner: hand-placed markers stay independent, and an owned marker edited by hand (renamed, given
    lines, moved) stays its instance's.
  - Ownership is saved in a new `OWNR` section (marker id, instance id), written only when some
    marker is owned. `MARK` keeps its layout, so files load both ways without a version bump
    (ADR-015/016 precedent): older files load with every marker hand-placed, older builds keep the
    markers and drop the ownership.
  - Markers are made only by a new placement, never on load: loading doesn't read prefab files. The
    story's camp (instance 65 in `worlds/story.flyw`) was placed before `camp.txt` declared its
    bandits, so it brings none, and its NPCs stay the 28 the story was balanced for.
  - Lowering a column topped by a tree or structure takes the surface material from its highest
    natural voxel (Grass, Dirt, Stone, Sand, Snow, Rock) under it, and recolors the new top only
    where it is natural ground: cutting into a canopy leaves leaves, cutting it down to or below the
    ground leaves grass. A top lying right on natural ground is paint (a Cobble road) and stays the
    surface; raising keeps the top block as before (raising under a canopy raises the canopy).
  - `fill` and `paint` (MCP and panel) take every block; the catalog lists them all. Kept: Paint and
    path need a solid block (an Air or Water top would dig a hole or leave a column whose ground
    the brushes can't see), and checkEdit still rejects ids past `BlockCount`.
- **Alternatives considered:**
  - Re-making markers from the prefab file on load or on move: the story's existing camp would
    suddenly spawn two more bandits (30 NPCs, a fight the balance test never saw), and a hand edit
    of an owned marker would be lost on the next move.
  - Ownership inside `MARK` behind a version bump: every older build would refuse the new files.
  - Markers as part of the instance record (`INST`): the marker list is what spawns NPCs, gets
    edited and undone; a second home would drift.
  - Recomputing owned markers' positions from the declaration on move: drops a hand move of the
    marker; carrying each marker's current spot on the footprint keeps it.
  - Skipping only Wood and Leaves for the surface: roofs and planks would still spread.
  - Always the natural ground's surface, raising too: raising under a tree stacked dirt on the
    canopy, and sculpting a Cobble road turned it back into dirt.
- **Consequences:**
  - A placed camp adds 2 NPCs toward the 256 cap; placing fails (changing nothing) past the marker
    caps.
  - Editing a prefab file's markers changes only later placements, like its voxels.
  - A one-voxel bush or anything else lying right on the ground counts as paint: lowering it
    spreads it, as before.
  - A move over the work budget has to be a remove and a place, which makes the instance's markers
    afresh from the file (new ids, hand edits lost).
  - Model-color blocks (Skin, Eye...) can now be placed in the world; they are plain colors there.

## ADR-022: A stat pipeline of modifiers, and one function that resolves every hit

- **Status:** accepted (2026-11-03); amends ADR-007, ADR-008 and ADR-010
- **Context:** gear could only add armor: health, regen and swing time read `Combatant::stats`
  directly in six places, so an attribute on an item would have been ignored by some of them (the
  debt ADR-008 and ADR-010 noted). Talents and buffs (refactor step 11) need the same hook, and
  the damage formula was split between `roll()` in `entity.cpp` and `mitigate()`, with the player's
  attacker level hard-coded to 1. The Bone Charm, the barrow ghouls' drop, did nothing.
- **Decision:**
  - A body's final stats are a `StatSheet` from a pure `compute(base, span<const Modifier>)`
    (`stats.h`): per stat `(base + Σflat) × (1 + Σincreased) × Π(1 + more)`, never below 0.
    `Modifier {stat, kind, value, source}`; increased and more are fractions (0.1 = 10%), and a
    more factor below −100% counts as −100%. Every value is capped at 10⁶ so the integer consumers
    can't overflow; haste alone may go down to −0.9 (a slow, at most 10× the swing time). Two
    +50% increased give ×2.0, two +50% more give ×2.25 (Path of Exile's rule, not Unreal GAS's
    summed multipliers). The sheet holds the five attributes and the derived stats: max health
    (from final stamina), damage (a hit multiplier, 1 = none), haste (from final agility), regen
    (from final spirit), armor (0 + gear) and damage taken (a multiplier on hits received). A
    derived stat starts from the final attributes, then takes its own modifiers.
  - `collect(combatant)` rebuilds the modifier list from the equipped items on every call: each
    item's `armor` as a flat Armor modifier, then its `mods`, sourced by the item's name. Talents
    and buffs will append theirs there. Because the list is rebuilt, unequipping needs no undo
    code and restores the base exactly. `sheet(c) = compute(c.stats, collect(c))`, uncached (a
    handful of modifiers per body; add a dirty flag only if profiling asks).
  - Everything that read the base reads the sheet: max health (HUD, the regen cap, respawns, the
    Return heal, fleeing), regen, swing time, and both UIs. Creature listings (the Data tab, the
    MCP `catalog`) and the loader's windup check use the base plus the creature's weapon mods. The `Stats` overloads of `maxHealth`,
    `swingTime`, `strengthBonus` and `rollDamage` remain for creature types and tests: they are
    the sheet of the base alone.
  - One `resolveHit(attacker, defender, attack, ctx)` (`entity.cpp`) for every swing that lands,
    in the order it always had: the attack's roll, plus round(strength × attack speed), × the
    attacker's damage, less `armorReduction(armor, attacker level)`, × the defender's damage
    taken, rounded once, at least 1; 0 only when the hit before armor is 0 (nothing rolled, or a
    damage multiplier of 0). `HitContext` carries the RNG
    and K, and is where conditional effects ("vs beasts", "below 30% health") will be read, not in
    the sheet. Debug damage still bypasses it.
  - `level` moved from `Entity` to `Combatant`, so the player has one (1, a debug slider) and it
    feeds `resolveHit`; NPCs take their kind's.
  - Items may have `"mods": [{"stat": "spirit", "flat": 2}]` in `items.json`: up to 8, each a
    known stat id and exactly one of `flat` (−1000..1000), `increased` or `more` (−1..10), on
    items worn in an equip slot only; anything else fails loading with file, record and field.
  - A Trinket equip slot, appended after Off Hand. Equip slots aren't saved anywhere yet (data files
    name them, and there is no player save), so `SlotBag` and `SlotNone` moved up one; the
    characterization test was re-pinned on purpose. Once a save stores them they're append-only.
  - The Bone Charm is the first trinket: +2 spirit (regen 1 → 2 per second). No other shipped item
    has mods yet, so every pinned replay and the Act 1 balance means are unchanged.
- **Alternatives considered:**
  - Caching the sheet behind a dirty flag set by equip, level-up and buffs: a stale-cache bug class
    for no measured cost.
  - Applying gear imperatively (add on equip, subtract on unequip): drifts with percentages and
    rounding, and every source needs its own undo.
  - Rounding at each step of `resolveHit`: double rounding biases small hits; the float flows
    through and rounds once, which is exactly the old result when every multiplier is 1. Without
    modifiers every step adds 0 or multiplies by 1, so the sheet reproduces the old swing time,
    regen and health; the replays pin that bit-for-bit for the shipped agility, spirit and
    stamina values.
  - Keeping `armor(inv)` beside the sheet: two answers to "how much armor" once talents raise it.
- **Consequences:**
  - Stat effects are data: a JSON edit and Reload defs. A new derived stat is an enum value, its
    base in `compute` and a consumer.
  - Armor and max health are floats in the sheet; consumers round (`maxHealth` ≥ 1).
  - When the player levels (step 11), its level pierces NPC armor through `resolveHit`, and gear
    armor per tier has to grow about linearly with level to keep the reduction constant; the
    debug Level slider shows the effect today.
  - The debug Player tab shows the sheet: every stat's base and final value, and each modifier with
    its source. The Character window shows final attributes with what gear adds in green.

## ADR-023: Time-of-day spawns: an active window on spawn markers

- **Status:** accepted (2026-11-03)
- **Context:** Act 1's barrow ghouls should walk only at night ("the dead walk at the treeline"),
  and later content wants the same for other kinds (night wolves, day-only traders). The clock is
  already one number, `Session::timeOfDay` (0..1, 0 midnight), which the debug slider, `--time`
  and the MCP `time`/`time_of_day` fields use.
- **Decision:**
  - A spawn marker may carry an active window, `Marker::activeFrom`/`activeTo`, in the same 0..1
    times of day: its NPCs are out in `[from, to)`, past midnight when from > to (`activeAt`).
    Equal means always; both 0 is the default, and `checkMarker` refuses other equal pairs (so a
    window round-trips exactly), times outside [0, 1) or NaN, and windows on other kinds.
  - `syncSpawns` takes the time of day. An inactive marker spawns nothing. Its living NPCs leave
    once they aren't fighting (the ADR-020 states Chase, Windup, Recover, `Entity::angry()`); one
    in a fight finishes it (wins, gives up at the leash, loses the player) and then leaves. Its
    corpses stay while they hold loot, and leave once looted or when they get back up (the
    respawn timer), so nothing respawns out of the window; an empty corpse lies `kCorpseLinger`
    (3 s) first, so a kill made out of the window still shows its death pose. When the window opens only the
    marker's missing slots (`Entity::slot`, ADR-025) spawn: one still out keeps its state. Every
    marker missing slots fills them that way (a removed marker undone too); only a changed marker
    (place, radius, kind, count) respawns all of its NPCs, as before.
  - A player save keeps the time of day it was made at, so its NPCs load as they were. Where the
    save's time is outside a marker's window (the window changed since), its saved NPCs are
    dropped with the note "its marker is out of its active window at this time of day".
  - `step()` calls `syncSpawns` every tick, after combat, but only while some marker has a window:
    worlds without one run exactly as before.
  - Saved in a new `TIME` section (marker id, from, to), written only when a marker has a window;
    `MARK` keeps its layout (ADR-015/016/021 precedent). Older files load with every spawn always
    active; older builds skip the section and drop the windows.
  - Set from the editor's Marker tool (a checkbox and From/To sliders with the clock time), MCP
    `mark`/`edit_marker` `active: [from, to]` (`[]`: always; each time in [0, 1), kept below 1 after
    the float cast; `markers` lists it, rounded), and
    prefab `marker:` lines (`active=0.8-0.2`). The debug window's World tab lists the timed
    markers, active or not, with how many of their NPCs are out.
- **Alternatives considered:**
  - Hours on a 24 h clock: a second time unit next to the one Session, the renderer and MCP use.
  - A window on the creature kind (data/creatures.json): every ghoul everywhere would be night-only,
    and a story can't place a day ghoul; the marker is where authored placement already lives.
  - Despawning the whole group only once none of them fights: simpler counting, but calm members
    would linger by day next to a fight; leaving one by one is what the story wants.
  - "Return home, then vanish": walking back first looks nicer but needs a new AI state and reason
    in `think()`; vanishing once out of combat is enough while nobody watches the barrow at dawn.
  - A separate per-tick pass beside `syncSpawns`: it would duplicate the marker lookup and the
    count rules; one function keeps edits and time changes on the same path.
- **Consequences:**
  - With a timed marker the world pays one `syncSpawns` per tick: a binary search per NPC and one
    small per-marker tally buffer, no hashing, and lines and names are copied only when they change.
  - A saved corpse with loot of such a marker is dropped with the rest, loot included; restoring it
    would need spawning a single slot from the loader, not worth it for a window edit.
  - Act 1: the two ghoul markers (#34, #35) get `[0.8, 0.2]`; by day the act has 24 NPCs, at night 28.
    The balance tests fight ghouls directly and don't depend on the clock.

## ADR-025: A separate `.flys` player save, keyed by stable ids

- **Status:** accepted (2026-11-03)
- **Context:** the game had no player save: quitting lost the character, its gear and every NPC's
  fate. The world file (`.flyw`) is authored data that only the editor writes, and a designer
  keeps editing it after players have played on it. Quests, flags and progression (refactor steps
  10 and 11) need somewhere to persist. Bethesda's split is the model: an immutable master and a
  save of deltas keyed by stable ids, whose failure mode is bloat from deltas never dropped.
- **Decision:**
  - A player save is its own file next to the world, `worlds/story.flyw` → `worlds/story.flys`
    (`savePathFor`), in the `.flyw` container style: `"FLYS" | u32 version | tagged sections |
    FNV-1a checksum`. Sections: `HEAD` (required: world file name, the world file's checksum,
    seed, simulated seconds, time of day), `PLYR` (position, yaw, health, seconds knocked out,
    level, the five attributes), `INVT` (equipment by `EquipSlot`, bag bar, every container, all
    by item `save_id`), `NPCS` (per NPC: spawn marker id, slot, kind, health, seconds dead —
    the respawn timer —, loot, position, yaw). `*.flys*` is git-ignored.
  - NPCs are keyed by **(spawn marker id, slot)**, never by vector index: `Entity::slot` is the
    NPC's index among its marker's, set by `spawnAt`. Adding, removing or reordering markers in the
    world doesn't scramble NPC state. Marker ids are never reused, so a removed marker's NPCs can't
    land on a newer one: `EditHistory::markerIds` is the highest id ever handed out (`pushRecord`
    raises it, so undo-then-edit doesn't free one), `nextMarkerId` stays above it, and the world
    file keeps it in a new `MKID` section (written only when it's above every live id; `MARK` is
    unchanged; older files fall back to the highest live id + 1).
  - Loading parses and bound-checks the whole file first (size ≤ 1 MiB, counts ≤ the caps: 256
    NPCs, `EquipCount` slots, 4 bags, 5 containers, finite numbers, the data files' level and
    attribute ranges); any failure rejects the file and leaves the session untouched, then the
    loader tries `PATH.bak`. Item and creature ids the data doesn't know (a save from a build with
    more) are not a failure: those items and NPCs are dropped with a note. A good file sets the time
    of day first (spawns may depend on it), resets the session as `startSession` does (keeping the
    player's movement tuning) and applies each section on top.
    Unknown sections are skipped, missing ones keep `startSession`'s state; duplicate sections are
    rejected. The version is checked; a semantic change gets a chained `vN → vN+1` step then.
  - A save made on a different world file (checksum or seed) loads what still matches and says
    so: an NPC entry whose marker is gone, spawns another kind or fewer NPCs now is dropped with a
    note; an NPC whose home moved away (beyond its wander or leash range) starts at home; player and
    NPC positions are clamped into the world and lifted out of terrain. Items no longer fit for their slot, or
    past a bag's new size, go into a free slot, else are lost with a note. Nothing crashes.
  - The AI state isn't saved: living NPCs start calm (a chase resumes on sight), dead ones load
    in Dead with their timer. The damage-roll and NPC RNG streams aren't saved either.
  - Writing reuses the world file's path through one `replaceFile` (`world.cpp`): temp file, then
    rename over the target. For saves it also tries to make it durable: `F_FULLFSYNC` on the file
    on macOS (plain `fsync` may sit in the drive cache), the old save renamed to `PATH.bak` first,
    and a best-effort sync of the folder after the rename (`F_FULLFSYNC`, else `fsync`; not every
    file system supports it, and POSIX doesn't promise a rename survives a crash without it).
    World saves keep the plain temp-and-rename. If both the save and its `.bak` fail to load (e.g.
    a crash between the two renames with a bad `.bak`), nothing is loaded and saving is locked as
    below; the files stay for a look.
  - F5 saves, F9 loads (both unbound before), quitting saves, and the game loads the save on start
    when one exists for its world; `--new-game` skips that (quitting still writes it). Benchmarks
    never read or write saves (keys and the Save tab's buttons too). Quitting doesn't save:
    - when a save exists but didn't load (on start or F9): F5 refuses too, until the player saves
      over it or deletes it in the Save tab, so a file that might be rescued isn't overwritten;
    - after Delete, until the next save, so the next start really is a new game;
    - when the session opened the editor (its flight and teleports aren't play); F5 still saves.
    The log and the Save tab say which. The editor's Ctrl+S and the MCP `save` write only the `.flyw` (the
    e2e test checks no `.flys` appears); there is no MCP save/load command.
  - **Rule from here on:** every new runtime state (quests, flags, skills, a voxel change made by
    gameplay...) lands with its own section in the player save and a round-trip test in
    `core_test`.
- **Alternatives considered:**
  - The player state in the `.flyw`: every editor save would overwrite a play, and every play would
    dirty the authored file.
  - JSON saves: easy to read, but a second format to bound-check, and the container already exists.
  - NPCs keyed by vector index: any marker edit would hand one NPC's state to another.
  - Saving the FSM state and timers: a mid-swing Windup or a chase against a player who loaded
    elsewhere needs more restore code than it is worth; calm NPCs re-aggro on their own.
  - A hard link or copy for the `.bak` instead of a rename: no moment without `PATH`, but more
    code; the loader's fallback to `.bak` covers that moment.
- **Consequences:**
  - The story world's save (28 NPCs) is ~1 KB and takes ~5 ms with the full fsync.
  - The debug window's Save tab shows the path, the file's time and size, Save / Load / Delete
    buttons, the last result and the last load's notes (world changed, NPCs dropped, items moved);
    the HUD shows "Game saved" / "Game loaded" for a moment.
  - `EquipSlot` values are saved now: append only (amends ADR-022's note).
  - World files gain the `MKID` section; older builds skip it (and may reuse a removed marker's id).
  - A save on a world without a file (generated, never saved) records checksum 0 and the seed.

## ADR-026: Quests as data, one event drain per tick, and condition-gated dialogue

- **Status:** accepted (2026-11-03)
- **Context:** Act 1 tells "bring Hakon's helm to Torvald" only through fixed lines (ADR-016).
  A quest needs progress from gameplay (talking, kills, loot, places), lines that react to it, a
  journal, and state that survives a save (ADR-025). Combat, the inventory and talking shouldn't
  call into quests, and a quest's effects (giving an item) mustn't re-enter the code that raised
  the event. Research: Bethesda's stage/objective model, the Morrowind/Creation Kit dialogue rule
  (top to bottom, first line whose conditions all pass), Nystrom's observer and event-queue
  pitfalls (docs/research/notes/quests_dialogue_saves.md).
- **Decision:**
  - **Events.** `GameEvent` is a variant carried by value: `Killed{kind, spawnMarkerId}`,
    `ItemCountChanged{item, count}`, `TalkedTo{spawnMarkerId}`, `RegionEntered{markerId}`. One
    `std::vector<GameEvent>` per tick in `Session`. Combat pushes the player's killing hit, E pushes
    a talk, the region check an entry; item counts are *derived* at the end of `step()` against
    last tick's, so every inventory change (loot, quest actions, the UIs, a load) counts without
    each path pushing. `updateQuests` drains the vector once, at the end of `step()`; what handling
    raises lands in the next tick's batch. Each event meets the quests as they were before it (a
    quest another started on it hears the next, so quest order doesn't matter); an item's count
    change counts once per batch and has_item reads the live count. `RegionEntered` is for the
    innermost region only, when it changes. No subscribers, so no dangling listeners; a request
    with an answer ("does this line pass?") stays a direct call.
  - **Quests** are `data/quests/*.json` (arrays of records, file name order) loaded by `loadDefs`
    after items and creatures: `{id, title, objectives: [{id, text}], stages: [{id, journal,
    actions, on: [triggers]}], lines}`. Stage 0 is "not started" and holds only the triggers that
    start the quest. Actions are a closed set: show/complete/fail objective, give/take item, set
    flag, set stage, start/complete/fail quest. Triggers: `talked_to` / `killed` (spawn marker
    name), `killed_creature` (creature id), `entered` (region marker name), `has_item` (item id,
    count), each with optional conditions. "Has item" is a state: entering a stage queues an
    `ItemCountChanged` for each of its has_item triggers. Done and failed quests enter no stages,
    and the current stage isn't entered again.
  - **A reward is never lost.** `give_item` stows (`stowItem`: an item slot, else the item's empty
    equip slot or bag bar slot). Before a trigger runs, its actions and the stages they enter are
    played out on a copy of the inventory; if a give wouldn't fit, the trigger doesn't run, a
    "Make room in your bags" toast shows, and the next matching event (talking again) retries.
    `take_item` refuses bags (they may hold items). Loot stows too, so full bags don't lock the
    quest's helm on Hakon's corpse, and Ctrl+right-click destroys an item to make room (quest
    items too: Hakon respawns and always drops the helm, so the quest stays completable). Quest, objective, stage, item and
    creature references resolve to handles at load (pass 2a collects quest, stage and objective
    ids so references may point forward); a bad one is a hard error naming file, record and field.
  - **Markers by name stand in for aliases.** Marker names aren't unique ids, but they are what
    designers and the MCP bridge already use, and they survive the editor (ids too, but a quest
    file naming "#13" can't be read). On world load `checkQuestMarkers` warns about every name
    that doesn't name exactly one marker of its kind; at run time a name matches any marker of it.
  - **Flags** are one `std::map<std::string, int32_t>` in `Session` (the CK's global variables);
    quest stages stay their own conditions, not mirrored into flags.
  - **Dialogue.** Lines are picked top to bottom; the first whose conditions all pass (AND, with a
    per-condition `not`) is said. Vocabulary: `stage op n`, `done`, `failed`, `objective is`,
    `flag op n`, `has_item op n`, `in_region`, `npc_dead`. Quest-owned lines live in the quest file
    keyed by spawn marker name; the world's `LINE` section stays the fallback that cycles as
    before and stays editable in game and over MCP. On E, `pickLine` asks the evaluator and `talk`
    says the quest line, unless it said that same one last time and the marker has lines: then the
    marker's next line, so the two take turns (the markers' route hints stay reachable) and a new
    quest line, such as Torvald's hand-over, is said first. Shouts say only the marker's lines.
    A talk is answered from the state the player walked up in; the quest reacts after, so Act 1
    gives stage 0 its own lines for the talk that starts it. The loader warns when a line without
    conditions shadows a later one.
  - **Saves.** `QUST` (per quest: stage, done/failed, objective states) and `FLAG` sections in the
    `.flys`, keyed by string ids (quests and objectives), so data edits don't scramble them; an
    unknown quest or objective is dropped with a note, a stage the quest no longer has is kept with
    a note. Bounds as for the other sections; a bad section rejects the file.
  - **UI.** J opens the Journal (free key); quest news shows in the notice line; the debug window
    gets a Quests tab (stages, objectives, flags, warnings, the last 32 events).
  - The first quest, "Torvald's Helm", replaces the Act 1 follow-up; Torvald rewards the helm with
    the Iron Cuirass (docs/STORY.md). A player who brings the helm before being asked finishes it in
    one talk (a first trigger in stages 0 and 10).
  - `reloadDefs` keeps quest handles and objective indices: quests and objectives may be appended
    (or dropped from the end), not renamed or reordered. QUST rejects a quest both done and failed.
- **Alternatives considered:**
  - Ink (inkcpp) or Yarn Spinner: both bring a variable store and save blob to keep in sync with
    quest state and an external compiler, for branching choices E-to-talk NPCs don't have.
    Reconsider inkcpp if conversations gain choices.
  - Observer callbacks (combat calls quest listeners): re-entrancy and listener lifetimes, and
    harder to trace than one drain.
  - Every inventory change pushing `ItemCountChanged`: loot, quest actions, the debug window's
    give and empty, a load, each kept honest by hand; the derived count is a scan of the ~40
    item slots per item per tick.
  - OR in conditions (the CK's binding rule): no Act 1 line needs it; `not` and line order cover it.
  - A stage-0-less model with a separate `start` list: one more concept for what stage 0's
    triggers already say.
  - Quest lines in a new world-file section, editable over MCP: possible later as its own tagged
    section; `LINE` keeps its meaning.
  - Unique alias ids on markers: a new marker field and editor UI; the load-time check is enough
    while names are hand-picked.
- **Consequences:**
  - A quest's progress may lag the event that caused it by one tick (17 ms) when it chains through
    a re-check; not visible.
  - A talk that advances a quest is answered with the line of the stage it was in (the line is
    picked before the tick's events), hence the stage-0 lines.
  - Alternation means a marker's own lines come back after a quest is done; lines that only make
    sense before it (Torvald's "bring it back") should be reworded in the world.
  - A trigger refused for room has already shown its line (the line can't know about bag space).
  - `reloadDefs` may append or drop quests at the end, never reorder them (`Session::quests` is by
    handle; the save is by id).
  - `pickLine` scans every quest line on each talk; fine for tens of quests.

## ADR-027: Use-based skills, a character level and talent trees, all through the stat pipeline

- **Status:** accepted (2026-11-03); amends ADR-022 (the player's level, conditional modifiers) and
  ADR-025 (a `PROG` section)
- **Context:** the player wants Skyrim-style skills ("the more you use your one-handed sword, the
  better you get at it"), a level from fights, and a talent point per level spent in a tree per
  ability. Skyrim's system is three formulas over small tables; its known failures are farming
  (hitting an unkillable companion, crafting spam) and cheap skills pushing a level that scales the
  world. flycoos' NPCs have fixed levels, so there is no world scaling to outrun. Research:
  docs/research/notes/progression.md and the architecture report's progression section.
- **Decision:**
  - **Formulas** are pure functions in `stats.h` with Skyrim's shape: a use earns
    `use_mult × base XP + use_offset`; a skill goes from L to L + 1 for `improve_mult × L^1.95 +
    improve_offset`; reaching skill level L adds L character XP; level N → N + 1 costs
    `25 × (N + 3)` (`levelXpTotal(N) = 12.5N² + 62.5N − 75`). `core_test` reproduces UESP's values
    with Skyrim's constants first: 87,000 XP to level 81, 4,930 from one skill 15 → 100, 88,085
    from all 18 skills maxed (the old level-81 cap), 528,804 and 91,600 skill XP for a combat skill
    and Smithing 15 → 100.
  - **Skills** are a closed set code awards: `Skill { OneHanded, Unarmed, Armor }` (one-handed:
    landed hits with a weapon, every weapon so far being one-handed; unarmed: landed punches; armor:
    hits taken while wearing armor). `data/skills.json` holds their names and four numbers, and
    `per_level` modifiers times the levels gained. Skills run 1..100 and start at 1, so a new
    player's sheet is unchanged. Shipped: `use_mult` 1 / 3 / 2 (a sword hit earns 8, a punch 7.5,
    a knife hit taken 9), `improve_mult` 1, `improve_offset` 200 (about 25 sword hits a level at
    first): clearing Act 1 once is about One-Handed 6 and Armor 5. Per level: +2% damage with
    one-handed, +3% with unarmed, +2% increased armor.
  - **Character XP** also comes from kills: creature records get an optional `xp`, by default
    `10 × level` for hostile, attackable ones and 0 otherwise (an `xp` on one that can't be hurt is
    an error). Hakon gives 60. Act 1's hostiles add up to ~380 XP, so clearing it is about level 4
    and 3 talent points. A level up gives one talent point and nothing else. The level is
    `Combatant::level` (ADR-022), capped at 100, so it is the attacker level of the player's hits;
    NPCs wear no armor yet, so today it changes nothing in a fight.
  - **One award on the damage path.** `awardHitXp(player, npc, playerHit, killed)` in `entity.cpp`,
    called by `updateCombat` right after `resolveHit` lands a swing either way, is the only source
    of skill and character XP besides the debug window. Its guards: the NPC must be attackable
    (the Skyrim companion exploit); XP scales with the attack's *average base damage*, never the
    final hit (gear, strength and talents don't speed it up); harmless NPCs (the training dummy)
    teach at 25%, so it stays useful; per NPC, skill XP halves every 20 counted hits and the wear
    fades at one hit per 10 s, kept through respawns. Two counters: the player's hits on the NPC
    (`Entity::xpWear`, weapon skills and kills) and the NPC's hits on the player (`armorWear`,
    armor), so standing in a wolf's bites doesn't cheapen hitting it. Ten minutes of punching the
    dummy earns ~1% of the full rate. Kill XP is the creature's `xp` times the same wear factor,
    once per kill (the killing hit): killing one guard as it keeps respawning gives 25, 18, 13, 10
    of its 40.
  - **Talents** are `data/talents.json`, one tree per skill: `{id, name, tree, tier 0..4, max_rank
    1..10, requires, per_rank}`. Tier N needs `3 × N` points spent in its tree; `requires` names a
    talent of the same tree and a lower tier that must be at its max rank. Every rule is in one
    pure `canLearn(progress, level, talent, &why)`; points are derived (`level − 1 − spent`), never
    stored. Respec is `talents.clear()` (a debug button for now). Thirteen talents with original
    names: Keen Edge, Quick Wrist, Steady Hand, Deep Cuts, Whirling Steel; Hard Knuckles, Quick
    Jabs, Heavy Hands, Haymaker; Worn In, Hearty, Catch Your Breath, Stand Fast.
  - **Never applied by hand.** Skills and talents emit `Modifier`s from `collect()` (sourced by
    their names), next to the gear's, so a respec or a lost level restores the base exactly.
    A modifier may say `"with": "one_handed" | "unarmed"` (items too): `compute(base, mods, with)`
    counts it only for that weapon skill, and the plain sheet never does. `resolveHit` and
    `startPunch` (swing time) use `sheet(c, skillOf(swingWeapon))`, so "+3% damage with one-handed
    weapons" is evaluated per hit, not baked into the sheet (ADR-022's conditional effects). Per-rank
    and per-level values multiply the modifier's value (rank 2 of +10% more is +20% more).
  - **State** is `Progress` on `Combatant` (character XP, each skill's level and XP, talent ranks by
    handle); NPCs carry an empty one. Saves: a `PROG` section (character XP, skills and talents by
    string id). Unknown skill or talent ids are dropped with a note, a rank above the talent's
    max now is lowered with a note; character or skill XP at or past the next level's cost (the data
    made it cheaper) is lowered to just short of it with a note; the ranks are replayed through
    `canLearn` tier by tier (`replayTalents`), and any that fails (points, tier, prerequisite)
    resets the talents with a note. A level outside 1..100, a negative or non-finite XP, rank 0 or a
    duplicate id rejects the file. `reloadDefs` refuses a talents file that renames or reorders
    loaded talents, and the Data tab replays the ranks after a reload; `progressModifiers` clamps a
    rank to the talent's max anyway. A `WEAR` section keeps both wear counters per NPC by (spawn
    marker id, slot), beside `NPCS` (its layout unchanged). Loading warns (`Defs::warnings`) when
    `skills.json` or `talents.json` is missing.
  - **UI.** K (free) opens the Progress window: level and XP bar, unspent points, each skill with
    its bar and per-level effect, and the three trees by tier, learnable talents highlighted green,
    maxed gold, a click learns, the tooltip says why not (`canLearn` checks the tree's rules before
    the budget, so a locked talent says why it's locked), and "N points over budget" when the
    debug window lowered the level. A multi-level toast counts the points. Skill and level ups toast in the notice
    line (`step` compares before and after combat). The debug window gets a Progression tab: the
    level slider (moved from Player), +XP buttons, skill sliders and +100 XP per skill, Respec, and
    the formulas' current numbers; the Data tab lists creature XP and the talents.
  - **Level band.** A `core_test` invariant: a level-N player in the Iron set with the sword, its
    N − 1 points in the blade tree and the skill levels Act 1's pace gives (level 4: One-Handed 6,
    Armor 5) needs 6–16 swings to kill each fighter of level N, and kills it sooner than it downs
    the player (swings × the time between them: the player's swing time; the NPC's swing time or
    windup + recover + cooldown). The one named exception is the guard: the village watch is meant
    to be tougher than a player of its level (it's passive and never starts the fight).
    **NPC armor isn't modeled yet** (creatures wear none), so the player's level, its attacker
    level, pierces nothing today; the band is what gear tiers and armored NPCs must keep. The Act 1
    balance tests are for a level-1 player and unchanged; one test fight never earns a skill level.
- **Alternatives considered:**
  - Skills starting at 15 like Skyrim: a fresh player would need per-level effects counted from 15
    anyway; starting at 1 keeps "levels gained" and the shown number the same.
  - A stored talent-point pool: it can disagree with level and ranks; deriving it can't.
  - Skill XP from the rolled or final damage: rewards strength and gear, the Skyrim tempering bug.
  - Wear reset on respawn, or a hard cap per NPC: resetting re-opens dummy farming every 5 s; a cap
    makes the hundredth bandit worthless instead of cheaper.
  - XP from killing only (WoW): misses the "use it to improve it" the player asked for.
  - Talents applied on learn: an undo per talent and drift with percentages (ADR-022).
  - Data-defined skills: code decides what counts as a use, so a skill only data knows would never
    improve.
- **Consequences:**
  - A new skill is an enum value, its id, the code that awards it and a record; a new talent is a
    record (and a stat, if it needs one). Effects that aren't stat modifiers (abilities) need a
    new kind of talent output.
  - Gear tiers must keep the level band: once NPCs wear armor, the player's level pierces it.
  - Kill XP doesn't fall off for low-level foes; farming respawns is slow (30 s+) but possible.
  - Debug "Hit for 10" and "Kill" give no XP (they bypass `resolveHit`, as before).

## ADR-028: One colour per voxel: smooth material patches instead of per-voxel jitter

- **Status:** accepted (2026-11-03)
- **Context:** `voxel_fs` multiplied every voxel's palette colour by a random ±10% from a hash of its
  cell. Every surface, models included, looked sandy: salt-and-pepper speckle with no structure. The
  user's art rules (docs/ART.md) are one colour per voxel, gradients instead of random noise, sharp
  edges between materials, soft patches inside a material, and flat models.
- **Decision:**
  - **Tones as data.** `kTones` in `world.h`, next to `kPalette`, gives each block two variant
    colours (sRGB), a patch size in voxels and a strength 0..1: natural blocks clear (0.6–1),
    man-made faint (0.25–0.35), plain 0 (Water, Ember, Lantern, model-only blocks). The renderer
    uploads them linearised in two new Frame arrays beside the palette, `toneA` (w: size times the
    global size) and `toneB` (w: strength times the global strength); the globals are baked on the
    CPU, so the shader reads no extra uniform.
  - **Smooth noise per voxel.** `albedoOf` reads two independent gradient-noise (Perlin) fields
    (one lookup over a lattice `size` voxels apart, quintic fade; one PCG hash per lattice corner,
    seeded by the block, picks a gradient for each field from its low and high bits) at the voxel's
    cell centre `floor(worldPos - normal / 2) + 0.5`, maps each through
    `smoothstep(0, 0.4, n) * strength` and fades the palette colour toward variant a, then variant
    b. The main colour is the most common one, in round patches (measured on a CPU copy: ~25% of voxels pure
    main, ~45% including those within a 0.2 mix of it). Colour is per block, so material edges
    stay sharp.
  - **Stone soft, Rock crisp** (user's call after review). Ground Stone (terrain, mountains,
    subsoil) fades softly: size 10 and lower contrast keep the brightness step between neighbouring
    voxels at most ~26% (p99 ~13%; it was 62% / 32% at size 6). Rock (pillars, spires, boulders)
    keeps small high-contrast spots. Builders pick Rock vs Stone by the look they want.
  - **World space.** Chunks draw with a translation, so `worldPos` is the world voxel position and
    patches run on across chunk borders. The `voxelPos` vertex output and the per-draw jitter
    offset (`hashOffset.xyz`) are gone; the Draw uniform's vec4 is now `modelLight` (x: light + 1,
    0 for chunks).
  - **Flat models.** Model draws set bit 31 of the flat `blockAndQuad` output (the greedy-quads
    debug index keeps bits 14-30); `albedoOf` returns the palette colour for them. Every model,
    held item and creature goes through `drawModels`, which sets the flag.
  - **Tuning.** Render tab "Colour patches": global strength 0..2 (0: the flat palette) and size
    0.25..4, and per block its variants, size and strength (runtime only; `RenderSettings::tones`).
    `--patches S` sets the global strength from the command line.
- **Alternatives considered:**
  - Value noise (trilinear blend of hashed values), the first version: where two lattice corners
    are alike the blend is constant along that axis, so a field reads as axis-aligned bands, and
    one colour per voxel turns them into long stripes along the view (seen in review on the
    meadow). Gradient noise has round features at the same cost.
  - Two separate scalar noise lookups (16 hashes): a second lattice would decorrelate the two
    fields' grid, but doubles the cost; one lattice with two gradients per corner looks the same.
  - Per-vertex or per-quad colour: greedy quads span many voxels, so it can't give one colour per
    voxel.
  - Terrain-aware patches (dry on hilltops, lush near water): wanted later, not now (docs/ART.md).
  - A fragment-side Draw uniform for the model flag: another push per draw; a spare bit is free.
- **Consequences:**
  - Patched fragments pay 8 integer hashes, 16 float3 dot products and 7 float2 lerps; plain
    blocks and models branch out early. Fewer vertex outputs (no `voxelPos`) save bandwidth on the tile-based GPU.
  - The greedy-quads debug colour wraps at 128K quads instead of 256K.
  - A new block needs a `kTones` entry in its class (`core_test` `tones()` checks the classes).

## ADR-029: Five tree species, shaded crowns, a 128-block palette, compact instances and replant

- **Status:** accepted (2026-11-04)
- **Context:** There was one tree: a 1- or 2-wide trunk under a single ragged ellipsoid of Leaves
  (edges from a per-voxel random `jitter`), plus a thin pine. The user wants a stylised fantasy
  forest (docs/ART.md "Trees"): oak, maple, birch, conifer and Araucária; massive trunks; thick
  branches where real trees have them; leaves darker low in the crown and lighter on top. The story
  world's forest is ~3,000 trees baked into its voxels by the old generator, and the world is made
  by hand.
- **Decision:**
  - **Plan, then draw.** `planTree` (src/trees.cpp) turns a species, height, crown radius, colour
    and seed into a `TreeShape`: bark capsules (trunk, root flare, limbs) and leaf clumps
    (ellipsoids whose surface smooth OpenSimplex noise pushes in and out). `growShape` draws it
    (bark into Air and Water, leaves into Air only, then removes leaves with no solid face
    neighbour) and `shapeBox` bounds it. Prefab boxes and tests read the same shape. Trees no
    longer use `jitter`.
  - **Shades are blocks.** Each of 7 leaf kinds (oak, birch, pine, Araucária, maple red, orange,
    gold) has 4 shade blocks, chosen per voxel row by height in the crown. Their colours are
    generated: the kind's colour times `kLeafRamp`. Oak's shade 2 is the old `Leaves` id. Three
    barks are new (birch, pine, Araucária); oak and maple keep `Wood`. 39 + 30 = 69 blocks, so the
    shader's block field grows to 7 bits (light moves to bits 7–14, the debug quad index to
    15–30, 16 bits) and `palette`/`toneA`/`toneB` hold 128. `kLeafOf` maps a block to its kind
    and shade; `isTreeBlock` replaces the Wood/Leaves checks (ground finding, light cost).
  - **Prefabs.** `tree` (now the oak: height, crown) and `pine` (height) keep their names and
    parameter counts with wider ranges; `maple`, `birch` and `araucaria` are appended; `bush`
    is a few small oak clumps.
  - **Generator.** `plantTrees` puts one candidate per 13×13 cell (crowns are about 3× wider than
    before) and picks the species from height, region and flatness (docs/ART.md).
  - **INS2.** Big trees as instances would have cost 8 bytes per changed voxel (the story world
    would grow from 3 MB to ~35 MB). The writer now writes `INS2`, which run-length codes each
    instance's cells over its box when they are changes in box order (every placement), otherwise
    keeps the old list. `INST` still reads.
  - **Replant.** A `replant` command (MCP tool) removes the old baked trees (26-connected clusters
    of Wood and Leaves, holding both, not owned by an instance, not touching today's tree blocks),
    regrows the old tree, pine and bush instances (those whose cells are only Wood and Leaves;
    `tree`/`pine` full grown by their seed; not where the bigger tree would meet a structure or
    cover a marker) and plants the generator's trees as instances, skipping boxes that meet an
    instance already there or hold a spawn or POI marker. Today's trees are left alone, so a second
    run changes nothing. Every sub-step folds into one undo record as it's made (the local history's
    256-step cap would drop the oldest). It may take a second or two for the whole world, so it has
    no work budget.
- **Alternatives considered:**
  - Shade from the leaves above each voxel (follows the crown's shape, works on hand-painted
    leaves): the user chose height in the crown, calmer and the same along each row.
  - Shading in the shader: it would need each tree's crown span per fragment; blocks are free at
    draw time and let builders paint shades by hand.
  - 3 shades in a 64-colour palette (62 blocks, no shader change): bands 4 rows tall and no room
    for later blocks.
  - Replanting as baked voxels: smaller files, but the user wants to move and remove trees.
  - Ragged per-voxel edges kept on the new shapes: the user chose clumps with smooth edges.
- **Consequences:**
  - The story world after `replant`: 466 old clusters (837k voxels) removed, 11 instances
    regrown (old trees #31, #49, #52 and bush #61 keep their shape: the bigger tree would meet a
    structure or a marker), 826 trees planted; 4.4 MB, instance cells 13% of `kMaxInstanceCells`;
    undo restores it byte for byte (scratch copy, 2026-11-04).
  - Generated worlds (seeds 1337, 42): oak 33–37%, birch 26%, maple 20%, conifer 11–13%,
    Araucária 5–8% of ~750–800 trees.
  - Until `replant` runs, an old `tree {10, 4.5}` reads as a young oak (height 10, crown 4.5): moved
    by hand first, it regrows small. Replant first.
  - `generation`/`regions` checksums were re-recorded; the greedy-quads debug colour wraps at
    64K quads.

## ADR-030: Grass and flowers as plant blocks drawn as instanced voxel models

- **Status:** accepted (2026-11-04)
- **Context:** Debug note 3: the plains look empty; the user wants tall grass and flowers, not
  bushes. They chose small voxel models (like the characters), a mixed meadow (mostly ankle-high,
  knee-high patches, bare gaps, flowers in clusters), plants saved in the world and paintable, and
  the bushes removed from the story world.
- **Decision:**
  - **Plants are blocks.** `ShortGrass`, `TallGrass` and four flowers (`isPlant`) stand in the cell
    above a grass voxel. To everything but drawing they are air: `isOpaque` is false, they cost 1
    light like air and don't end a column's open sky, and `isClear` (air or a plant) replaces the
    `== Air` checks for ground finding, surfaces, spawn room and leaf growth. They save, load and
    undo like any voxel; the world file only gains block ids.
  - **They go with their grass.** Brushes that move a column's top carry its plant along
    (`setColumnTop`); after every edit, plants in the touched box (and the layers just above and
    under a fill) that no longer stand on Grass, or have water over them, are cleared; clearing also
    reaches a plant left under a tree's crown. Placing a block in a plant's cell replaces it.
  - **Models.** The block says what grows; the model (3 short tufts, 3 tall tufts, daisies,
    buttercups, poppies, bellflowers), a quarter-turn and an offset of up to 0.15 come from a hash
    of the cell (`plantHash`, the same in C++ and the shader). Models are 10³ grids of model-only
    blocks (`Blade`, `TallBlade`, petals, `Stem`, `FlowerHeart`; a plant block can't be meshed as a
    cube), meshed by `meshRegion` without the faces on the ground. Blades are straight with their top
    quarter stepped one voxel out, so they merge into few quads.
  - **Instanced drawing.** `meshRegion` lists a chunk's plants (`PlantInstance`: cell, light,
    model) instead of making faces, so edits and light changes refresh them with the chunk. Each
    frame the renderer counting-sorts the plants of the visible chunks within `plantRadius` (64) by
    model and detail, uploads them (one cycled transfer buffer) and draws each model once with
    instancing. Within `kPlantNear` (24 blocks) plants use the detailed model, beyond it a far one
    with half the blades, all straight. `plant_vs` sways the upper voxels in the wind and grows
    plants in over the radius' last 12 blocks; lighting uses a normal tilted up toward the ground's,
    and blades take the Grass colour patch under their root (`plant_fs`), so a tuft matches its ground.
  - **Placement.** `meadowPlant` (soft noise: lush stretches and bare gaps, tall-grass patches,
    flower clusters of one kind per area) is shared by the generator, the editor's Plant brush
    (`Tool::Plant`: density, a kind or the mix, or clear) and the `meadow` command (a region, one undo
    step recorded in 64×64 tiles, no work budget). Plants grow only on grass that is the column's
    highest opaque voxel with air above: not under trees, roofs or water. The meadow's noise has
    fixed seeds, so every world gets the same pattern: the brush and the generator agree, and
    terrain makes worlds differ anyway.
- **Alternatives considered:**
  - Plants derived from the terrain at draw time (no saved data, edits follow for free): the user
    chose saved and paintable.
  - Plants baked into the chunk meshes: quads are whole voxels (6-bit positions), and sub-voxel
    models would need a second quad format and far more memory.
  - One draw per plant, like characters: thousands of draw calls.
  - Bent blades in the near model too: 80 quads per model on average; at 45% uniform density
    (357k quads) they cost +0.7 ms.
- **Consequences:**
  - Prototype (story world, a plant on 45% of open grass, benchmark view): 357k quads +0.7 ms;
    with straight-kinked blades, far models and no ground faces 130k quads, +0.29 ms.
  - The story world after removing the 8 bush instances and `meadow` over the whole map:
    147k short, 81k tall grass and 32k flowers; 4.35 → 4.92 MB. In the benchmark view about 2.8k
    plants (97k quads): +0.2–0.3 ms on 2.3 ms (M4, A/B against e100595).
  - Generated worlds grow the meadow last (22% of all columns of a 512² seed-42 world); the
    `generation`/`regions` checksums were re-recorded (without plants they are the old ones).
  - Placing a prefab clears the plants inside its box that lost their grass (e.g. under a bridge deck
    flush with the ground); one just above the box, on a cell its top layer covered, stays.

## ADR-031: The endless world: a wrapping window streamed around the player

- **Status:** accepted (2026-11-07); revises M1's "a fixed story world replaces an infinite one"
- **Context:** The user wants a procedurally generated world with procedural quests, monsters and
  dungeons, so play doesn't depend on the story's progress. The first step is a world that goes on
  as the player travels. The hand-made story world must keep working.
- **Decision:**
  - **One wrapping window.** Without `--world` the world is `Grid::streamed`: the usual 1024×192×1024
    grid (192 MiB, as before), but in world coordinates. `lo` is its low corner, and storage wraps in
    x and z (power-of-two sizes; `index` masks). Moving the window only rewrites the 32-wide chunk
    columns that left it. `get`/`set` keep their meaning, so physics, NPCs, edits and the mesher
    didn't change. Code that used `size` as the world's edge uses `lo`/`hi()`. Rows that cross the
    wrap seam are split (`rowRun`). Any other grid (the story world, models, prefabs, tests) is fixed
    at `lo`, usually 0, and behaves as before. `Light` wraps the same way.
  - **Generation in any order.** `generateChunkColumn` builds one chunk column of the world of a seed.
    It works in a scratch grid padded by 24 blocks, more than any tree or rock reaches, so the trees
    and rocks that touch the column grow whole and only the column is copied out. The column is a
    pure function of (column, seed). Terrain was already a function of the column. Trees no longer
    read the grid: a candidate (one per 13×13 cell) yields to a neighbouring cell's candidate within
    10 blocks that has a higher roll, instead of to a crown planted before it. Rocks stand on the
    ground, not on crowns. A 48-block pad gives exactly the same voxels as 24, and tree density is
    within 4% of before. Generated worlds changed (the pinned hashes too); saved worlds didn't.
  - **Stages.** `Stream` (core, tested) keeps the window centred on the player:
    - worker threads generate the columns that came in, nearest first;
    - each frame the main thread installs them, then lights columns whose 8 neighbours are in
      (`lightColumns`, nearest first, within a time budget: 2 ms, a debug slider);
    - it hands out a column's 6 chunk slots to mesh once its 8 neighbours are lit.

    So a chunk is meshed only when every voxel and light value its faces and AO read is final. Light
    only ever rises toward the exact value, so lighting in any order ends up exact, the same as
    `fillLight` (tested). A column that leaves is cleared to air and dark, and its chunk slots are
    emptied. The two outer rings of the window are never meshed: what's drawn reaches about 448
    blocks around the player, inside the fog.
  - **Renderer.** `streamWorld` starts empty with 2M quads of headroom. `updateChunks` meshes and
    empties slots. Packing leaves each chunk a quarter more room, so the next column in a slot
    usually fits without a repack. Plant cells are packed modulo 1024 and unpacked against the
    window (`plantCell`, and the plant shader through the draw's uniform).
  - **NPCs** in columns that aren't in yet wait (`Session::loaded`). The player save stores any
    position, and loading recentres the window there, lifting the player out of the terrain.
  - **No editing yet.** The editor (Tab) and every remote command but `state`, `screenshot` and
    `goto` refuse the endless world. Their coordinates, prefab instance cells (u16) and the world
    file all assume a fixed world at 0. A streamed grid is never written to a world file
    (`writeWorldFile` refuses it), and `relight` ignores edits outside the window.
- **Alternatives considered:**
  - A hash map of chunk columns: unbounded and sparse, but every voxel read hashes, and all the code
    that walks rows of `cells` would change. The window keeps reads O(1) and the code as it was.
  - Moving the data when the window moves (memmove): a 192 MiB hitch every few chunks.
  - Lighting on worker threads: lighting a column raises its neighbours' light, which the mesher
    may be reading, and light chunks allocate as they change. Kept on the main thread with a budget;
    at 60 blocks/s it costs ~3 ms a frame.
- **Measured** (M4, release, `--fly`, 1800 frames): standing still, 8.4 ms mean, p99 9.9 ms; flying at 60 blocks/s, 8.5 ms
  mean, p99 10.8 ms (light 1.9 ms + mesh and upload 1.9 ms a frame, 1 repack). The first view
  (5 columns around the spawn) comes in ~0.9 s, and a column takes ~4 ms to generate on a worker.
  The story world is unchanged (same frame times, and a byte-identical screenshot).
- **Follow-ups:**
  - Editing the endless world (editor, commands, instance cells) and saving its edits (only the changed columns).
  - Lighting off the main thread.
  - LOD beyond the window.
  - Procedural quests, monsters and dungeons on top.

## ADR-032: Water from the land's shape: seas, rivers and lakes; soft stone and snow lines

- **Status:** accepted (2026-11-07)
- **Context:** Generated terrain switched to stone at height 111 and to snow at 131 everywhere, so a
  mountain showed level lines. Every lake was a dip filled to sea level, so lakes looked alike and
  sat at one height, ringed by the same band of sand. There were no rivers or seas.
- **Decision:** `Terrain` stays a pure function of (x, z) (ADR-031), built in three steps.
  - **Seas:** a coast noise (0.0005, 4 octaves, domain-warped 250 blocks for capes and bays)
    sinks the land to 18 below sea level where it is low. The transition spans ~150 blocks, so
    coasts are gentle and their beaches wide.
  - **Rivers:** along the zero line of a meandering noise. The bed is 1-4 below sea level, with a
    width that varies along the river. The valley rises smoothly over ~6 river widths. Rivers wane
    above height 60-95 (the valley rises dry into the hills), and a mask noise breaks the network
    up.
  - **Lakes:** at most one per 224-block cell, at a hashed spot, with radius 10-60 (mostly small),
    stretched up to 1.8 and turned at random. Its level is the lowest ground around it (8 samples at
    1.3 radii), so it fills a hollow at its own height. Cells where the middle stands above the
    rim (a hillside or a top), next to a river, at sea level or above 120 get none, and of two
    neighbours that come near each other the higher roll keeps its lake. Each lake is a bowl
    (deepest 2-11 in the middle), and a shoreline noise wobbles the shore into bays and points. The
    ground around eases toward the shore over up to 30 blocks, and within 0.3 radii it is raised
    to the water's level at least, so water is never open to the air at its sides (tested).
  - **Blocks** come from height, slope (the largest step to a neighbour) and noise.
    - Snow above ~128 and stone above ~106. Both lines wander ±10 with a low-frequency noise and
      are mottled ±12 at two smaller scales.
    - Snow needs flatter ground (2 heights a step) and stone takes steep ground (4 heights a step).
      Cliffs (steps of 4) are stone at any height and steps of 3 are bare earth.
    - Beaches: sand on gentle ground up to 1.5-4 above the sea, or 0.5-2.5 above a lake.
    - Under water: sand in the shallows and the sea, silt (dirt) deep in lakes, stone on steep
      beds.
- **Alternatives considered:**
  - Flowing water (simulated rivers, erosion): not a pure function of the column, so the endless
    world couldn't generate in any order.
  - Lakes as dips in a noise filled to a per-region level: the water would spill wherever the
    dip's rim is lower than that level. Basing the level on the rim avoids this.
- **Consequences:**
  - Generated worlds changed (pins updated); saved worlds, the story world included, didn't.
  - A column takes ~6 ms to generate on a worker instead of ~4, and the first view comes in ~0.8 s
    instead of ~0.65 s. Frame times are unchanged: flying at 60 blocks/s, 8.35 ms mean, p99 9.4 ms,
    the same as before.
  - Over the 8×8 km around the spawn, 17% of columns are under water instead of 3%, most of it sea.

## ADR-033: Lock-on in the style of the soulslikes

- **Status:** accepted (2026-11-07)
- **Context:** Combat aimed where the camera looked, so circling an enemy meant steering the
  camera and the character at once. The user asked for lock-on as Elden Ring has it: R3 (Tab on a
  keyboard) locks onto the enemy nearest the middle of the view, and the D-pad picks the next enemy
  in that direction.
- **Decision:** `lockon.h/.cpp` (core, tested) holds the rules; main.cpp wires input, camera and HUD.
  - **Lock:** R3 or Tab locks onto the alive, attackable entity within 20 blocks, on screen and in
    the camera's line of sight, nearest the middle of the view (nearer ones win close calls).
    Pressing it again lets go. With nothing to lock onto, the camera swings behind the player.
  - **Holding the lock:** the lock holds `Entity::id`, a per-run counter from `makeEntity`, because
    the entity list is erased from and appended to. It lets go past 28 blocks or after 2 s out of
    sight. A kill moves it to the next pick, if there is one (setting "Next target on a kill").
  - **Switching:** the D-pad, the arrow keys, a right-stick flick (past 0.7 tilt from under 0.3) or
    a quick mouse flick (60 px) moves the lock to the next enemy that way on screen, within 60
    degrees of the direction, scored by screen distance and straightness. While locked, the mouse
    and right stick don't turn the camera, and the D-pad doesn't open windows.
  - **Camera:** turns to look past the player at the target's chest, from a little above, and
    pitches with the height difference. It rises by up to 0.35 rad as the target comes within 12
    blocks and looks a quarter of the way toward it, so a near target shows over the player's head
    instead of behind it. The player stays the orbit's centre, so terrain collision is unchanged.
  - **Player:** `Input::face` keeps the body facing the target while walking (strafing, and a
    backwards gait when backing off). Running and rolls face where they go. Attacks aim at the
    target. The HUD draws a white dot on the target's chest.
  - **Keys:** the world editor moves from Tab to F1.
- **Alternatives considered:**
  - Mouse movement switching targets only, as Elden Ring does on PC: kept, but the arrow keys were
    added because the user asked for direction keys.
  - Holding the lock by index into the entity list: syncs erase and append, so the lock would jump
    to another NPC.
- **Consequences:** Lock-on with no enemy near also works as a "camera behind me" button. Up and
  down switch to enemies farther or nearer on screen. Anything attackable can be locked, the
  dummy, boars and guards included (as Elden Ring lets you lock onto any NPC you can hit);
  villagers can't be.

## ADR-034: Fighters on the player spread around it

- **Status:** accepted (2026-11-07)
- **Context:** Two NPCs chasing the player ran the same straight line and stood inside each other,
  so two of the same kind looked like one. The user wanted them aware of each other, coming at the
  player from different sides, and asked that it stay cheap with many enemies.
- **Decision:**
  - **Sides:** each tick, before the AI, the fighters on the player (alive, wandering kinds in
    `Chase`, `Windup` or `Recover`) are sorted by their angle around the player and given sides
    `2π/n` apart. The ring is turned to the mean of their offsets, so each keeps its place in the
    order and none crosses the others. Two take opposite sides, three a third of a turn apart.
  - **Approach:** from beyond 6 blocks a fighter runs straight in, so one that's behind doesn't
    lose the player by circling. Within 6, one more than 0.4 rad off its side heads for a point up
    to 0.8 rad round toward it, 1.5 blocks nearer than it is (at least 2.5): a spiral. At its side it
    closes in to 1.5 blocks. In reach, it swings wherever it stands. A lone fighter comes straight in.
  - **Spacing:** after moving, living NPCs closer than 1 block (and within a body height) push
    apart, half each, never into terrain. Ones that stand where they're put (the dummy) aren't moved.
  - **Cost:** sorting the pack is O(n log n) and spacing is O(n²) over the NPCs, both every tick.
    For the tens of NPCs an area holds that is microseconds, so there is no need to spread it over
    frames. A grid of cells would replace the all-pairs pass if areas grow to hundreds.
- **Alternatives considered:**
  - Body collision only: they'd stop overlapping, but would still queue up on one side.
  - Recomputing sides a few times a second: no measurable saving, and the sides would lag.
- **Consequences:** Fights against two or more are harder: one can come from behind, and lock-on
  helps to keep track. The Entities tab shows each fighter's side and pack size.
- **Follow-ups:** a cap on how many swing at once (the rest circle and wait, in the soulslike way);
  ranged kinds that keep their distance and back off from melee, once there are projectiles.

## ADR-035: Melee after Elden Ring's one-handed sword

- **Status:** accepted (2026-11-07)
- **Context:** Combat was a WoW-style swing timer: hold the button and the same two swings repeat,
  NPCs hit 0.14 s after deciding to, and nothing can be blocked, dodged on purpose or broken. The
  user asked for Elden Ring's one-handed sword: blocking, staggering enemies, a power attack on a
  held button, different animations for repeated swings, and generally what makes its combat good.
  Researched from the Elden Ring wikis (stance, poise, guard boost, attack types) and soulslike
  design notes (windup, active and recovery frames; input buffers; commitment).
- **Decision:**
  - **Moves:** every attack is a `Move` with a windup (to a hold point), an impact frame, a chain point
    (the next attack may start), a cancel point (a roll, the guard or walking may cut it), and its
    damage, stance damage, stamina and lunge (`kMoves`, scaled by the weapon's swing time). It commits:
    no walking, a step forward into the hit, turning toward the aim only through the windup.
  - **Buttons (the user picked "both"):** a tap of LMB / R1 is the next of four lights (diagonal,
    backhand, sweep, thrust finisher); held 0.16 s it's a heavy (lunging thrust, then overhead chop),
    which R2 starts at once. A held heavy holds its windup and is charged after 0.55 s. Presses wait
    0.5 s for the body (the input buffer). RMB / L1 guards.
  - **What the body was doing picks the attack:** running, late in or just after a roll, in the air
    (strikes on landing), just after a block (guard counter: fast, stance damage like a charged
    heavy) or at a broken stance in reach (riposte: x3, invulnerable, knocks it down).
  - **Stamina (the user picked a bar named Stamina; the max-health attribute shows as Vitality, its
    data and save id unchanged):** 100, spent by attacks, rolls, blocked hits and running in combat
    (free out of combat, as in Elden Ring); refills at 45/s after 0.5 s without acting.
  - **Guard:** covers 70° in front: 75% of a hit stopped by a sword, 40% by bare arms; stamina damage
    10 + 2.5 per point of damage, 35% less with a sword (guard boost). Too little stamina breaks the
    guard (1.1 s reeling).
  - **Stance and poise:** NPCs have a hidden stance (data `stance`) that hits wear down and that
    recovers as Elden Ring's (after base/13 s, 13/s); at 0 they kneel 2.5 s, open to a riposte. A hit
    of at least their `poise` makes them flinch, cancelling their swing (AI state `Stagger`). The
    player flinches from unblocked hits unless its poise (0.4 x armor) is above the hitter's
    `hit_stance`, or a heavy's windup gives hyperarmor. Rolls can't be hit from 0.04 to 0.34 s in.
  - **Enemy timing:** an NPC's swing holds its windup pose until `attack_windup` (0.85..1.35 x, so
    it can't be counted) has passed, tracking the player until 0.12 s before a fast strike; it then
    stands `attack_recover` (default 0.5 s, the opening). In reach and waiting it keeps a sword's
    length. Hit-stop freezes both bodies a moment on every landed hit.
  - **Story balance:** re-tuned so a careful player (the test bot guards a foe about to strike,
    attacks when its light lands first, ripostes) keeps the old bands: chief alone ~25 health left,
    two wolves ~40, two ghouls ~30, three wolves lose. Wolves became quick and light-biting.
- **Alternatives considered:**
  - Hold-to-power on the attack button only: every light waits for the release. Elden Ring's own
    layout (a separate heavy button): lights start at once, but no held power on the attack button.
    The user chose both.
  - No stamina, a guard meter only: faster fights, but no economy behind rolls and chains.
  - A visible stance bar: Elden Ring hides it; the debug window shows it.
- **Consequences:** Standing and trading loses to packs; reading windups, guarding and rolling win.
  Armor matters beyond damage (poise). Characterization replays were re-pinned. New data fields:
  `stance`, `poise`, `hit_stance`; `attack_windup` lost its cap against the swing time.
- **Follow-ups:** parries and shields (an off-hand guard with more guard boost), backstabs, enemy
  combos and delayed attacks, enemies guarding or dodging, a sound and a spark on hit, weapon-specific
  movesets (the knife and fists reuse the sword's), an attack-token cap for packs (ADR-034).

## ADR-036: Shields and the parry

- **Status:** accepted (2026-11-08)
- **Context:** The roadmap's next defense items (docs/COMBAT_ROADMAP.md): shields in the off hand
  and a parry. Researched on the Elden Ring wiki: shield classes (small 74–89% physical negation,
  guard boost 42–51; medium 83–100% / 42–64; great 91–100% / 52–82, and greatshields can't parry),
  the parry's frames (small and medium shields about 4 startup, 6 active, 11–13 recovery at 30 fps:
  ~0.13 / 0.2 / 0.4 s; 10 stamina), a parry hit early or late is a weak guard, a parry stuns the
  attacker open to a critical, bosses may need 2–3 parries that don't wear off, and greatshields
  bounce weak attacks.
- **Decision:**
  - **Shields are data:** an off-hand item's `shield` `{size, negation, boost, parry}`; three new
    items (save ids 12–14): Oak Buckler (small, 80% / 46%, parries 10% faster), Heater Shield
    (medium, 100% / 56%, parries), Tower Shield (great, 100% / 72%, no parry). The guard uses the
    shield's numbers instead of the sword's; the model raises the shield in front and holds the
    sword forward at the side.
  - **Bounce:** a guard of at least 65% boost makes an attack of under 20 stance damage recoil
    (0.6 s): bandits, wolves, boars, ghouls; not the guard or the chief.
  - **Parry (Q / L2):** a left-arm swipe across the body and out, a `Move::Parry` that commits through
    its recovery (0.75 s x the shield's `parry`), for 10 stamina, needing a shield that parries.
    From 0.1 to 0.33 s in, a hit from the front is deflected (no damage) and the attacker's stance
    breaks for 1.5 s: the existing riposte follows. Outside that window, or against a kind that
    can't be parried, it guards at half the shield's negation and boost (no guard counter after it:
    only a real guard opens one). Without a shield that parries, the button does nothing.
  - **Creatures:** `parryable` (default true; the boar's charge isn't) and `parries` (default 1;
    the chief needs 2, the first ones only rock it back 0.6 s; they don't wear off).
- **Alternatives considered:**
  - Parry on a guard tap (no new button): one button fewer, but a late guard would parry by
    accident and guarding would get a delay. Elden Ring has its own button for it (the skill, L2).
  - Parrying with the sword or bare hands: Elden Ring gives that to some weapons (daggers, fists)
    through a skill; left for the weapon classes (roadmap), with the dagger's own moveset.
  - Weight for the bigger shields: there's no equip load yet; the roadmap has it.
  - A per-shield guard counter: Elden Ring's guard counter is the main-hand weapon's; kept.
- **Consequences:** Defense has three textures: block (cheap and safe with a big shield, bounce
  openings against weak foes), parry (risky, opens a riposte) and roll. New data: the items'
  `shield`, the creatures' `parryable` and `parries`. `Move` and `Want` gained `Parry` (not saved).
  Tests build their item records after the code-named items, whatever data adds after them.
- **Follow-ups:** perfect guard (decide), backstep, damage types with per-shield negation, weight
  (equip load), NPCs with shields that guard and parry, a parry flash and sound.

## ADR-037: Sound

- **Status:** accepted (2026-11-08)
- **Context:** The game had no sound. The goal: every action and every combat event is
  recognizable by ear alone, so the fight reads even where the picture doesn't, plus ambience so the
  world isn't silent. The user had bad experiences with sounds generated in code: use free recordings
  that need no licence from anyone (public domain, or attribution in CREDITS.md). What shipped games
  do, from their sound designers' write-ups (Conan / The Force Unleashed and Ryse on Designing Sound,
  Saints Row IV via Wwise, God of War's hit-stop): several takes per sound picked at random and never
  the same twice running; a few percent of random pitch and volume on each; layering (a hit is its
  impact, its weight and a voice); separate sounds per material (wood, metal, flesh); distance
  falloff; the sound in sync with the hit-stop frame.
- **Decision:**
  - **What happened, from the sim:** each body sets `Noise` bits (`Combatant::noises`) where things
    happen: a jump, a landing (from more than a step up), a roll, a splash into water, the swing (as
    it leaves its windup pose, so the whoosh leads into the hit), an NPC's windup (the tell), a full
    charge, raising the guard, hurt, death, a critical, a stance break, a parry, a block, a guard
    break, a bounce, loot and talk. `step()` gathers them into `Session::sounds` (`Sound`: what,
    where, which kind, the ground, the move, the weapon or what hit it, how much), adding a
    footstep (or a swim stroke) where the gait passes a quarter turn, as the walk's feet land. The
    sim stays silent and testable; main.cpp plays them.
  - **What it sounds like:** `layersOf` maps a noise to sounds played together. Steps by surface
    (grass, dirt, stone, sand, snow, wood, wading). Swings: light, heavy, fast (a knife or fists), the
    shield's swipe; beasts lunge with their voice instead. Hits: blade, blunt or wood (the dummy), a
    heavy layer from 20 stance damage, a crunch on a critical. Blocks: sword (metal), shield (wood;
    deeper off a greatshield), arms. The moments that must stand out have their own: a parry rings,
    a stance break tolls, a guard break splinters, a bounce thuds. Voices come from the creatures'
    new `voice` and `voice_pitch` fields (human, boar, wolf, ghoul; the chief a deeper human), and
    `body` (flesh or wood).
  - **Files:** `data/sounds/<name>_<n>.ogg`, the takes of one sound (stb_vorbis decodes them at
    start). 223 takes of 65 sounds, 6 MB: cut, trimmed, faded and levelled from
    CC0 packs (Kenney, OpenGameArt) and Freesound (all CC0 but two CC-BY bird calls); CREDITS.md
    lists every source.
  - **Mixer** (`audio.h`): SDL3's audio stream pulls a float stereo mix at 48 kHz on its own thread;
    up to 48 voices with linear resampling (pitch), a smoothed gain per voice, three buses (effects,
    ambience, interface) and a tanh soft limiter. Sounds are heard at the player, panned by the
    camera's right; full volume within 4 blocks, then falling off as 1 / distance, silent past 45.
  - **Ambience:** looping beds, each faded in and out by what's around (a sparse look at the voxels
    twice a second): wind (stronger up high), birds by day where there are trees, insects at night,
    waves by open water and a stream by a little, fire near embers; single bird calls by day and an
    owl at night.
  - **Interface:** loot, talking, the game windows opening and closing, the lock-on, equipping, quest
    news and level-ups.
  - Debug window, Audio tab: mute, volumes per bus, the spreads, hearing distance, the voices
    playing, the ambience's readings, the latest sounds, sounds with no takes, and a button per
    sound to play it.
- **Alternatives considered:**
  - Synthesizing sounds in code: no files, but it tends to sound cheap; the user asked for
    recordings.
  - A sound middleware (FMOD, Wwise) or a mixing library (miniaudio, SDL_mixer): more features, a
    dependency and a licence for what is a few hundred lines on SDL3's stream.
  - Deriving sounds in main.cpp from state changes between frames: no sim changes, but a parry and
    a block look alike from outside; only the combat code knows which happened.
  - Data-driven sound tables (a sounds.json): the mapping is code for now, like the melee tables;
    creatures pick their voice in the data.
- **Consequences:** Every combat outcome has a distinct sound. New data: the creatures' `voice`,
  `voice_pitch`, `body`. `Combatant` and `Body` gained a few fields (not saved). Benchmarks run
  silent. Tests check the noises the sim makes, what each sounds like, the mixer, and that every
  sound the game asks for has takes.
- **Follow-ups:** a settings menu (volumes, mute, saved); reverb and muffling (caves, underwater);
  music; per-weapon-class sounds with the weapon classes (roadmap); armored-body hits; NPC
  footsteps by kind (hooves, paws).

## ADR-038: The backstep

- **Status:** accepted (2026-11-08)
- **Context:** The roadmap's defense list: a backstep (the roll button with no direction) and its
  retreat attack. In Elden Ring the dodge button standing still is a backstep: a short hop back with
  only minor i-frames (the official combat guide; the wikis and players say it has few or none), less
  ground than a roll, and its own attack out of it. Until now a roll standing still went the way the
  body faced.
- **Decision:**
  - The roll button with no direction, on the ground, is a **backstep**: the body hops back about 2
    blocks in 0.35 s (`backstepSpeed` 7, `backstepSeconds`), keeping its facing (it doesn't turn, its
    feet don't walk; nor while it slides to a stop after, unless steered), for 8 stamina (a roll: 14), with i-frames only from 0.02 to 0.14 s in (a roll:
    0.04 to 0.34). In the air no direction still rolls ahead. It's a roll as far as the rest of the code
    is concerned (`rolling()`, `Body::backstep` tells which).
  - **Backstep attack** (`Move::Backstep`): attacking late in a backstep or just after it, as the
    rolling attack does after a roll: a lunging sweep (7 blocks/s into the hit) that closes the gap
    the hop opened. One per backstep.
  - Pose: a small hop, leaning back, the hands up in front, landing in a stance. Sound: two quick
    footfalls and a little effort (`Noise::Backstep`).
- **Alternatives considered:** a backstep with no i-frames (some players' reading of Elden Ring): the
  official guide says minor ones, and with none it would be a worse roll; a backstep that keeps the
  rolling attack: its own attack reads better and covers the gap.
- **Consequences:** Standing still and pressing roll no longer rolls forward. Tunable in the debug
  window (Player tab, Equip load and the backstep).

## ADR-039: Equip load

- **Status:** accepted (2026-11-08)
- **Context:** The roadmap's shield weight and equip load (light, medium and heavy rolls). Elden Ring's
  equip load (eldenring.wiki.gg, Equip Load): what's worn against a maximum from an attribute; under
  30% light (the longest roll), 30–70% medium (the same recovery, a shorter roll), over 70% heavy
  (recovery almost twice as long, worse i-frames, slower stamina recovery), over 100% overloaded (no
  roll or backstep). Shields weigh 1–3.5 (small), 3–7 (medium), 8–29 (great).
- **Decision:**
  - Items have a `weight` (worn ones count; bags and what's in them don't). The iron set weighs 24,
    the sword 3, the knife 1.5, the charm 0.5; the buckler 2, the heater 4.5, the tower 13.
  - The maximum is `loadBase` 30 + 2.5 per point of vitality: 55 at the start. So a sword alone is
    light, the iron set with a sword medium (49%), with the tower shield heavy (73%).
  - **Light:** rolls at 14 blocks/s (medium 12): about 4.7 blocks. **Heavy:** 7 blocks/s for 0.6 s
    (3.2 blocks), i-frames to 0.28 s (0.34), 0.3 s before the next roll (0.15), a slower backstep,
    stamina back at 70%. **Overloaded:** no roll, no backstep, no running (holding the buttons does nothing: no stamina
    drain, the guard stays up), walking at 60%, stamina back at 70% as when heavy.
  - The character window shows the load (weight, maximum and class), tooltips the items' weight. A
    heavy roll thuds down (the roll's sound with a landing).
- **Alternatives considered:** carrying capacity from strength (Skyrim's is from stamina, Elden Ring's
  from endurance): vitality is this game's closest; the base and per-point are knobs. Weight slowing
  the walk or stamina in every load class: Elden Ring keeps the light-to-medium step about the roll only.
- **Consequences:** Armor and big shields now cost mobility. New data: the items' `weight` (a test
  wants every wearable item to weigh something). `Body` and `Combatant` gained `rollLoad` and `load`
  (not saved).

## ADR-040: Damage types

- **Status:** accepted (2026-11-08)
- **Context:** The roadmap: damage types (slash, strike, pierce, later elemental) with per-shield
  negation. Elden Ring's physical damage is standard, slash, strike or pierce, by the attack: a straight
  sword's swings slash and its thrusts pierce; enemies resist or are weak to each (skeletons to strike).
- **Decision:**
  - `DamageType` Slash, Strike, Pierce. A weapon's `damage` (default slash) is what its swings do; a
    slashing weapon's thrusts (`MoveSpec::thrust`: the fourth light, the first heavy, the rolling
    attack, the riposte) pierce. Fists strike. A creature without a weapon has its own `damage`
    (default strike): wolves bite (pierce), ghouls claw (slash), boars gore (strike).
  - Creatures' `resist` per type takes off (or, negative, adds) a share of a hit: the boar's hide 20%
    of slashes, 10% of strikes, and it's weak to pierce (20%); the wolf weak to slash (15%); the ghoul
    weak to strike (30%) and resisting pierce (30%). Armored men have none for now (the story fights
    are tuned without them).
  - Shields' `negation` is a number for all, or one per type: the oak buckler stops 85% of slashes,
    70% of strikes, 80% of pierces; the heater 90% of strikes. The sword's and the arms' guards stop the
    same of every type.
  - Hits of a striking weapon sound blunt.
- **Alternatives considered:** a fourth, standard type: it adds a column with nothing to tell it
  apart yet. Armor resistances per type for the player: they'd need per-type numbers on every piece;
  shields and creatures carry the texture for now. Strike adding stance damage: that's the weapon
  classes' (hammers) to decide.
- **Consequences:** Which attack to use depends on the foe (swings on ghouls, thrusts on boars, fists
  aren't bad on ghouls). New data: the items' `damage`, the shields' per-type `negation`, the
  creatures' `damage` and `resist`.
- **Follow-ups:** elemental types with magic; armor per type; damage numbers by type (roadmap, feel).

## ADR-041: The perfect guard and parrying without a shield

- **Status:** accepted (2026-11-08)
- **Context:** Two roadmap items. A perfect guard ("decide"): the user picked a small bonus. Lies of
  P's perfect guard is the reference: a guard raised about 0.15 s (9 frames) before the hit negates all
  of its damage (a plain guard lets some through) and still costs some stamina. And parrying with light
  weapons: in Elden Ring daggers (3/5/13 frames at 30 fps) and fists (4/6/13) parry, as small shields do.
- **Decision:**
  - **Perfect guard:** a hit in the first 0.15 s after the guard goes up takes no damage and half the
    stamina a block would, and opens the guard counter as a block does. It needs the guard to have been
    down for 0.3 s before (`perfectRearm`), so tapping the guard doesn't keep the window open. A guard
    that breaks isn't perfect. It sounds as the block plus a bright metal clink (`block_perfect`, CC0).
  - **Parrying without a shield:** with the off hand empty, Q / L2 parries with the main hand: a knife
    (its item's `parry`, 0.9: as quick as the buckler) or bare fists (`fistParry` 1). A sword can't. A
    shield's parry comes first, and a greatshield's none means none. The knife sweeps across the body
    in the right hand; fists parry with the left. The rest is the shield's parry (window, deflect,
    riposte, the weak guard around it).
- **Alternatives considered:** a strong perfect guard that also staggers the attacker or refunds
  stamina: it would overlap the parry. No rearm time: the window could be kept open by tapping.
- **Consequences:** Blocking rewards timing a little, without competing with the parry's riposte. New
  data: the weapons' `parry`. `Combatant` gained `guardTime` and `perfectReady` (not saved).
