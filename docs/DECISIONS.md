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

## ADR-042: Attack tokens, strings of hits and delayed hits

- **Status:** accepted (2026-11-08)
- **Context:** Three roadmap items for enemies (docs/COMBAT_ROADMAP.md). First, a cap on how many in a
  pack swing at once (ADR-034's follow-up): soulslike packs hand out a few attack tokens and the
  rest wait. Second, combos: an NPC swung one hit per windup. Third, delayed attacks: a hit held in
  its windup to catch a roll that came too soon.
- **Decision:**
  - **Tokens:** each tick, after the sides, `gAi.attackers` (2) of the fighters on the player hold a
    token (`Entity::token`); only they swing. One in its `Windup` or `Recover` keeps its own, and
    gives it up when its recovery ends. The rest
    go to the ones within 6 blocks that have waited longest since their last string (`sinceSwing`),
    so turns rotate. Without a token, a fighter within the ring plus 3 blocks circles on a ring 3.6
    blocks out at its side, facing the player and walking slowly one way round, then the other.
  - **Strings:** a swing starts a string of 1 to the kind's `combo` hits (uniform). Each next hit
    starts at the last one's chain point while the player is within reach + 1, as the next light in
    turn. Each hit adds its swing time to the swing timer, so a string bunches hits with a longer
    pause after it, without hitting more often.
  - **Delayed hits:** a hit after the first is delayed with the kind's `delay` chance. It holds its
    windup `gAi.delaySeconds` (0.5) × 0.8–1.2 longer and makes the windup sound again.
  - **Data:** bandit combo 2; guard combo 3, delay 25%; chief combo 3, delay 35%, recovery 0.25 → 0.4 s;
    ghoul combo 2, delay 40%, recovery 0.4 → 0.5 s. The longer recoveries keep the story fights in
    their bands: strings land more hits on a player who only guards.
- **Alternatives considered:**
  - Strings that cost nothing extra: two- and three-hit strings about doubled the damage taken.
  - Tokens by distance alone: the nearest would keep swinging and the rest would never get a turn.
- **Consequences:**
  - **Balance test:** the story-fight test now needs at least 10 of its 12 seeds in each band, not
    all 12. Enemies answer by chance (ADR-043), so one fight can go either way. The means are still
    held to their bands, and bare-handed the chief still always wins.
  - **Fight replay:** `fightReplay` was re-pinned.
  - **Tests and debug:** `npcAi` checks the state machine on a bandit without tactics. The Entities
    tab shows each fighter's token, string and wait, and edits `gAi`.
  - **Replays:** in a pack of three or more, a waiting fighter draws a random number to pick which
    way it circles. Replays of such fights change, even for kinds without tactics.

## ADR-043: Enemies that guard, parry, dodge and back off; NPC shields

- **Status:** accepted (2026-11-08)
- **Context:** The roadmap's "Enemies that guard, parry, dodge and back off" and "NPCs with shields".
  The player had all of these (ADR-035, ADR-036, ADR-038, ADR-041); NPCs only swung.
- **Decision:**
  - **Answers:** a fighter that the player's attack can reach (in front of the player, within reach
    + 1.5) picks one answer per attack, as soon as it is chasing. Its own swing is committed. The
    kind's chances pick a `guard`, a `parry` or a `dodge`. A heavy or a riposte can't be parried, so
    it guards instead.
  - **Timing:** the answer comes after a reaction time of 0.06–0.18 s. A guard goes up and stays
    0.4 s after the attack has passed. A parry starts so its window meets the hit. A dodge rolls
    aside 0.12 s before the hit, whichever side is walkable, else it backsteps; four-legged kinds
    hop back. Parries and dodges are off by up to ±0.07 s. It doesn't swing while it answers.
  - **The NPC's guard mirrors the player's.** The shield's (or weapon's) negation by damage type
    takes part of the damage. Each block costs stamina: `blockStamina` plus an amount per point of
    damage, less the guard boost. NPCs now have stamina, refilling as the player's does. A blocked
    hit does no stance damage. Out of stamina the guard breaks: it reels 1.1 s, not open to a riposte.
  - **Parries, rolls and counters:** its parry deflects a light (the player reels 1 s). Its roll's
    i-frames let the hit through. After it blocks or parries, its next swing within 1 s winds up in
    0.3 s (a counter).
  - **Shields:** a kind may hold a `shield` (an off-hand item; only kinds with hands). It is drawn,
    guards and parries as the player's, and stays up whenever it chases within 6 blocks. While its
    guard is up an NPC walks, as the player does.
  - **Backing off:** after its string, with the kind's `back_off` chance, it walks away facing the
    player for 0.9 s.
  - **Data:**
    - bandit: parry 15% (knife), dodge 20%, backs off 30%
    - wolf: dodge 20%, backs off 50%
    - guard: Heater Shield, guard 60%, parry 10%
    - chief: guard 10% (sword), backs off 30%
    - Validation: guard + parry + dodge ≤ 1. A guard needs a weapon or a shield. A parry needs
      something that parries.
- **Alternatives considered:**
  - Answers picked only on the tick the attack starts: an NPC that was recovering never answered.
    In play the player's attacks mostly start then.
  - A riposte by the NPC after its parry: it would need a knocked-down pose for the player. The fast
    counter is the punishment for now.
  - Guard breaks that open a riposte: too cheap against shield users. They only reel.
- **Consequences:**
  - **Playing against them:** heavies beat parriers. Strings and guard pressure beat a shield.
    Attacking in a fighter's recovery is still the safest.
  - **Data and state:** creatures gained `combo`, `delay`, `shield`, `guard`, `parry`, `dodge` and
    `back_off`. `Entity` gained tactic state and NPC bodies now use `stamina`; none of it is saved.
  - **Tests and review:**
    - The `enemies` test covers tokens, strings and every answer, using test-only kinds that always
      answer one way.
    - The review page shows each in motion.

## ADR-044: Weapon classes: per-class movesets, poses and models in the data

- **Status:** accepted (2026-11-08)
- **Context:**
  - The roadmap's "Per-weapon timings and stance values in the data", and its first melee weapon
    classes: daggers, axes, maces.
  - Every weapon swung one shared move table (`kMoves`), only faster or slower.
  - Which model a weapon showed was a knife-or-sword guess on the item id.
  - The talent plan (docs/TALENTS.md) asks that code asks a weapon's class, never its item id.
- **Decision:**
  - **Classes:** a closed set in code, `WeaponClass` {fists, dagger, sword, axe, mace}. Every main-hand
    item names its `class` in items.json. Bare hands are fists, which no item may claim. Code asks
    `classOf(weapon)` for the moveset, the poses, the held model, the swing sound and the icon.
  - **Movesets are data:** `data/weapon_classes.json` holds one record per class. Each gives every
    attack move's timing (seconds; hold, hit, chain and cancel points) and what it does (damage
    multiplier, stance damage, stamina, lunge, `heavy`, `thrust`).
    - The loader checks every move is there and its points come in order.
    - `none` and `parry` stay code's: the parry is the shield's (or a knife's or the fists') and
      times the same for every class.
    - A class may also give `speed` (x every move's length; fists 0.75, which replaces the old
      `fistScale` knob) and `hit_stop` (x a landed hit's hit-stop).
    - The weapon's own `attack.speed` still scales its class's moves, so a knife (0.75) and a mace
      (1.15) differ by item as well as by class.
  - **The sword's moveset is the old table, number for number.** Fists copy it (`copy_from`) at
    0.75 speed. Sword and fist fights are unchanged.
  - **Daggers:** tight slashes and stabs (light 3, light 4 and most special moves pierce). Low stance
    damage (6 a light), cheap stamina (9), the strongest riposte (x3.5), hit-stop x0.7.
  - **Axes:** chops with the head turned to lead the swing (a new pose field, `roll`), a rising
    chop to end the chain, an overhead cleave and a sweeping cleave. No thrusts. 13–18 stance a light,
    stamina 14, hit-stop x1.15. The Iron Hand Axe: 7–11, speed 1.1, weight 3.5.
  - **Maces:** the axe's chops as smashes, ending in a big overhead smash. Strike damage, the most
    stance damage (16–22 a light, 34–36 the heavies), hit-stop x1.3. The Iron Mace: 6–10, speed
    1.15, weight 4.
  - **Poses and models:** each class has its own swing poses (model.cpp); fists use the sword's.
    The axe and mace get voxel models and icons.
- **Alternatives considered:**
  - Per-item move tables: a record per weapon would repeat a whole moveset. Classes hold the
    moveset, items keep their damage, speed and weight.
  - Poses in the data too: they are tuned by eye against the models, which are code. They stay
    code until there is an animation tool.
  - Turning the axe's edge with fixed poses only: horizontal sweeps then led with the poll. A roll
    per pose fixes it at the cost of one float.
- **Consequences:**
  - **Tuning:** movesets tune without a rebuild: edit the file, then Data > Reload defs. The debug
    Player tab shows the held weapon's moveset.
  - **Data files:** `weapon_classes.json` is required. Test data folders copy it.
  - **Replays and balance:**
    - The bandit's knife swings the dagger's moveset, whose hits land a frame earlier; `fightReplay`
      was re-pinned.
    - The bandit's strings (light 1, then light 3) now end in the dagger's stab, which pierces: a
      buckler stops 80% of it, not 85%.
    - The story fights (sword, fists) didn't move.
  - **Getting the new weapons:** the axe and mace have no drop or merchant yet; the debug Items
    tab gives them.
  - **Later:** greatswords, two-handing and dual wielding add classes and grips; spears will want
    a per-class reach.
  - **Tests and review:**
    - The `weaponClasses` test pins the sword's numbers and each class's character, damage types and
      hit-stop, and checks every class's chain lands.
    - The review page shows each class's chain and heavies.

## ADR-045: Criticals: backstabs, random critical hits, camera shake

- **Status:** accepted (2026-11-09)
- **Context:**
  - The roadmap's "Backstabs and criticals" and, from Feel, "Camera shake on heavy hits" and "a crit marker".
  - The talent plan (docs/TALENTS.md) wants crit chance and crit damage as stats that items, buffs
    and later talents raise.
  - Ripostes were the only critical: an attack at a stance-broken NPC.
- **Decision:**
  - **Backstabs:** an attack from behind an NPC (within 60 degrees of its back, in reach and facing
    it) is a critical. The NPC must not be attacking, rolling or reeling, and must not have been hurt
    in the last 2 seconds, since it turns to face its attacker.
    - It is the riposte move, with the class's riposte numbers (sword x3, dagger x3.5). Elden Ring
      also gives both the same multiplier.
    - The move holds its NPC until the hit lands: it can't turn or act. The hit knocks it down
      (1.2 s), like a riposte.
    - Each tick the hold and the hit find the target again: an NPC still behind the player, held or
      broken. A downed, just-hurt or rolling NPC is never caught, so a second NPC in reach can't take
      the backstab meant for the first.
    - A blocked crit is shown and heard as a block. The camera doesn't shake while the sim is paused
      (the editor, the F2 note).
    - NPCs have no awareness yet, so an aggressive one that has seen the player turns and swings first.
      Backstabs land on passive or unaware NPCs, and on ones turned away (recovering from a swing,
      circling in a pack). Stealth comes later.
  - **Random crits:** two new stats, `crit_chance` (base 5%) and `crit_damage` (base +50%), in the
    stat pipeline (ADR-022) like any other.
    - Every hit but the riposte move's rolls once, the player's and the NPCs' alike, and does
      `1 + crit_damage` more.
    - Items raise them with `mods`: the Rusty Knife has +5% crit chance (10% in all), so daggers and
      bandits crit more often.
  - **The marker:** a critical hit's damage number is 1.5x bigger, orange and ends in "!". It makes
    the crit sound (`hit_crit`), which ripostes already used.
  - **Camera shake:** a trauma value on the player, 0..1, that wears off in a second. The camera
    turns by trauma squared, up to 3 degrees, along smooth noise.
    - Adds to it: a riposte or backstab (`hitShake`, 0.5), a random crit (0.3), the player's heavy
      hits by stance damage over 20, a hit taken (1.5x the share of max health it took, at most 0.6)
      and a guard break (0.3).
    - The Camera tab scales it (`cameraShake`, 0 turns it off).
- **Alternatives considered:**
  - A separate backstab move with its own numbers: Elden Ring uses the same multiplier for both. One
    move keeps one set of numbers per class.
  - No random crits, as in Elden Ring: the talent trees (and Cube World and WoW, whose stats this
    game follows) want crit chance and damage to raise.
  - Player-only crits: NPCs use the same stat sheet and formula. A 5% enemy crit keeps fights
    symmetric, and the damage number marks it.
  - Shaking the camera's position: rotating the view about the eye can't push it into terrain.
- **Consequences:**
  - **Replays and balance:** every hit draws one more random number, so `fightReplay` and
    `boarReplay` were re-pinned (same hits on the same frames, other rolls).
    - The story fights' means move with the draws by about 4 over 12 seeds. Over 48 seeds the
      chief's is 31 (30 without crits), so its band is now 20 to 35.
    - Backstabs don't change the scripted fights, which face their foe.
  - **Talents:** crit chance and crit damage are stats, ready for the Ranger's tree.
  - **Tests and review:** the `criticals` test checks the stats, the backstab's geometry and its
    conditions, and a backstab that holds and knocks down a boar and a recovering bandit. It also
    measures random crit rates (the knife's 10%, an NPC's on the player) and their x1.5, and the
    camera trauma. The debug Player tab's Criticals node tunes the angle, the wait and the shake.

## ADR-046: Status effects: bleed, poison, frost, and one list of timed effects

- **Status:** accepted (2026-11-09)
- **Context:**
  - The roadmap's "Status effects: bleed, poison, frost, stun build-up bars", half of Feel's
    "Damage numbers by type", and the talent plan's foundation 4: timed effects (poison, buffs,
    enchants) share one list on the combatant. Nothing like that list existed.
  - Foundation 1: what a talent could raise is a stat, so build-up and resistance are stats.
- **Decision:**
  - **Build-up, the Elden Ring way:** six new stats, `bleed`, `poison`, `frost` (what each hit that
    lands adds to the target's bar, base 0) and `bleed_resist`, `poison_resist`, `frost_resist`
    (what fills it, base 100).
    - A hit adds the attacker's build-up, through its weapon skill's sheet like crits. A guard lets
      through what its negation lets through, even from a hit too light to hurt: a buckler 15% of a
      slash, a perfect guard or a parry nothing.
    - The bars drain at 8 a second (`gStatus.decay`). Full, the status takes effect and its bar
      starts over. Poison and frost don't build while their effect is on.
    - Status damage skips armor and damage taken, and is a share of the target's max health. So it
      scales with the target, not the attacker's level or gear.
  - **The three statuses** (all in `StatusTuning gStatus`, tuned live in the debug window):
    - **Bleed:** a burst of 10% of max health + 5, at once.
    - **Poison:** a timed effect: 0.5% of max health + 0.5 (rounded) every second for 10 s. Its
      ticks aren't hits: a poisoned back can still be stabbed, and an NPC that gave up the chase
      isn't called back by them.
    - **Frost(bite):** a burst of 10% + 3, then a timed effect for 15 s with +20% damage taken (an
      `increased` modifier on DamageTaken).
  - **Timed effects:** `TimedEffect {kind, source, seconds, length, stacks, modifiers}` in
    `Combatant::effects`. `collect()` adds their modifiers, so they flow through the stat pipeline;
    updateCombat runs their clocks and poison's ticks. A fresh one of a kind replaces the one on.
    A death ends them all and empties the bars. Saves don't keep them.
  - **Creature mods:** creature records take `mods` like items (`Combatant::innate`). A wolf's bite
    builds bleed (22), a ghoul's claws poison (25), and ghouls resist poison (+400).
  - **Sources:** the Rusty Knife builds bleed (30), so the player's knife bleeds a foe in about 4
    hits, and bandits bleed the player. Frost has no source yet (magic later); the debug window
    applies it.
  - **Armor resists statuses:** each iron piece adds bleed and frost resist (40, 85, 25, 75, 25: 250
    for the set) and a little less poison resist (200 for the set), as Elden Ring's metal armor is
    robust. Status damage ignores armor, so without this a geared player in the story fights lost
    most of their health to it.
  - **What shows:**
    - Damage numbers come in a list of four per body, so a bleed's burst shows beside the hit that
      caused it: bleed red and "!", frost pale blue, poison's ticks small and green.
    - Build-up bars sit under each nameplate's health bar and over the player's, with names. An
      active poison or frostbite shows as a brighter bar draining with its time; health turns green
      while poisoned.
    - The character sheet lists the weapon's build-up and the three resists.
  - **Sounds:** a wet splat for a bleed (over a heavy hit), bubbles for poison, a glassy clink for
    frost: CC0 packs already in CREDITS.md, trimmed and levelled.
  - **Kills:** poison kills, and the kill is the player's (character XP and the Killed event that
    quests count): only the player afflicts NPCs.
- **Alternatives considered:**
  - Status numbers on the creature record as their own fields: creature `mods` reuse the stat
    pipeline and make every stat a creature trait, so the player's and NPCs' numbers work the same.
  - Statuses through armor: Elden Ring's status damage ignores defense, and resists are the counter.
    That's what makes armor's resist matter.
  - Stacking poison: one at a time with a lockout, as Elden Ring, keeps it readable. `stacks` is in
    the struct for later effects that stack.
  - Stun build-up (the roadmap's fourth): the stance system (ADR-035) already is one.
- **Consequences:**
  - **Balance:** the story fights hold their bands. Over 48 seeds the wolves leave 27 (28 before),
    the ghouls 26 (26), the chief 31 (31). An unarmored player is bled and poisoned often: armor and
    shields are the answer.
  - **Tests:** the crit tests and two bandit-hit tests give their bodies a huge bleed resist, so they
    measure what they're about. The `statusEffects` test checks the stats and sources, build-up, decay
    and resists, each status, the lockout, the guard's share both ways, the effects' modifiers through
    `sheet()`, a death clearing them, a poison kill's XP and event, and bleeds in fights both ways.
    Mutation-checked: each of 18 rules broken alone fails it.
  - **Talents:** build-up and resists are stats; the effects list is ready for buffs, enchants and
    spells. The proc numbers are global knobs (`gStatus`) for now, as the foundations allow.

## ADR-047: Healing flasks and rest points

- **Status:** accepted (2026-11-09)
- **Context:**
  - The roadmap's "Healing flasks (limited charges, refilled at rest points)". Since ADR-046 bleed
    and poison wear the player down mid-fight, and health only came back out of combat.
  - Foundation 1: what a talent could raise is a stat.
- **Decision:**
  - **The drink:** R (D-pad down on a pad) drinks from the flask. It's `Move::Drink`, a code move like
    the parry (`kDrinkSpec`: 1.3 s, the drink frame at 0.65 s), so it commits, buffers and is
    cancelled the way attacks are. Its last 0.3 s (from 78%) can be cut by a roll, the guard, another
    press or walking on at full speed.
    - It walks at 1.5 blocks/s (`gMelee.drinkSpeed`) and may turn; until that last part it can't
      attack, guard or roll, and stamina doesn't refill. It doesn't start in the air or in water.
    - At the drink frame a charge heals 40% of max health. A hit that makes the player flinch before
      then cancels it, the charge kept; a hit under the player's poise doesn't. No hyperarmor.
    - With no charges the gesture still plays (the bottle shaken, nothing heals): pressing it in
      a panic costs the time, as in Elden Ring.
  - **Two stats:** `flasks` (charges a rest fills, base 3) and `flask_healing` (each drink's share of
    max health, base 40%). `Combatant::flasks` counts what's left.
  - **Rest points are fires:** E within 3 blocks of an Ember block, not hit for 5 s and nothing angry
    within 24 blocks. Loot and talk come first. Being hit lately is the test, not the combat timer:
    poison's ticks keep that running for its whole 10 s, and a rest is the cure.
    - Every fire counts: campfires, cottage and longhouse hearths, the forge, a shrine's brazier. The
      story world has them already and the editor places them, with no new marker kind.
    - The endless world gets a campfire per 96x96 cell where the ground is dry, nearly level and clear
      of trees and rocks: about one per 115 blocks.
  - **A rest** refills the flasks, heals fully, refills stamina, clears status build-up and effects,
    and sets `Session::restPoint`. Knocked out, the player gets back up there with full flasks
    (before any rest, at the player spawn). Enemies keep their own respawn timers: resting doesn't
    bring them back.
  - **Saves:** a `REST` section (charges, capped by the Flasks stat on load; the rest point, bounded).
    Older saves load with full flasks, never rested. The debug window's "Regenerate world" forgets
    the rest point.
  - **What shows:** a red flask with the charge count left of the health bar (grey glass when
    empty); "[E] Rest by the fire" by a fire; a notice on resting. The left hand lifts a small voxel
    flask to the lips and the head tips back; it's red with charges left, glass when empty. The
    character sheet lists both stats; the debug window's Player tab has a "Flasks and rest" node
    (charges, the drink's progress, walk speed, whether it can rest here, the rest point, rest now).
  - **Sounds:** the cork, two gulps at the drink frame, an empty bottle shaken, and a fire flaring up
    on resting. qubodup's CC0 bottle and drink set and rubberduck's CC0 fire spells, trimmed and levelled.
- **Alternatives considered:**
  - A "rest" marker kind (a site of grace): any fire is a rule the player reads from the world, works
    in both worlds and needs no editor or file-format change.
  - Respawning enemies on rest (Elden Ring's sites of grace): the enemies here respawn on their own
    timers (ADR-023), and a reset would undo the story world's quest state. Left out.
  - Spending the charge when the drink starts: Elden Ring spends it at the heal, so a cancelled
    drink costs only the time.
  - A heal over time: the burst at a frame is the soulslike read, and the timed-effects list can
    carry a regen flask later.
- **Consequences:**
  - **Balance:** the story fights' bands (ADR-046) are measured without drinking, as before; the flask
    is the player's margin on top.
  - **Generated terrain:** the campfires change every pinned terrain hash and mesh count (re-pinned:
    `generation`, `chunkMeshes`, `lighting`, `regions`, which also checks there are 4–40 fires in 512x512).
  - **Tests:** the `flasks` test checks the stats, the drink frame's timing and heal (capped at max
    health), walking and turning while drinking, no attack, guard or early roll, a flinch cancelling
    it, an NPC not reading it as an attack, the empty gesture, no drink in the air or water, a drink
    press waiting out an attack, the rest gate (fire, reach, combat, an angry wolf), what a rest
    restores (a poison that's really ticking included), and getting back up at the rest point. The
    save test round-trips `REST`, loads an older save with full flasks (by the gear's Flasks stat),
    caps saved charges by it, and rejects a cut section, a bad "rested" byte and a far-off rest point.
    Mutation-checked: each of 30 rules broken alone fails a test.
  - **Talents:** charges and healing are stats; a talent like Elden Ring's "more flasks" is a flat
    modifier.

## ADR-048: Weapon scaling with attributes, and two-handing

- **Status:** accepted (2026-11-09)
- **Context:**
  - Two roadmap Systems items. Every weapon got the same `strength x speed` bonus (WoW-style), agility
    only added haste, and intellect and spirit did nothing for weapons, so the build and the weapon
    choice didn't interact.
  - Two-handing is mostly "strength x1.5" in Elden Ring, so it builds on scaling. Greatswords and
    colossal weapons (later) need two hands.
  - Foundation 1: what a talent could raise is a stat.
- **Decision:**
  - **Scaling:** items.json weapons take `"scaling": {strength, agility, intellect, spirit}`, 0..5
    each (missing: strength 1, the old rule, which fists and creatures keep). A hit adds, per attribute,
    the attribute x its scaling x the new `scaling` stat (base 1) x the attack's base speed, summed
    and rounded once (`attributeBonus`, replacing `strengthBonus`). The UIs grade a scaling E (under
    0.25), D, C, B, A, S (1.4 and over).
    - Shipped: the sword strength 0.7 / agility 0.5 (C/C), the knife 0.3 / 1.2 (D/A), the axe 1.2 /
      0.2 (A/E), the mace strength 1.5 (S). The sword's and knife's sums keep their damage at the
      starting attributes, so NPCs with them hit as before.
  - **Two-handing:** F (Triangle + R1 on a pad; a Triangle tap opens the bags on release) toggles
    `Combatant::twoHanded`. It's a buffered want that waits for a free body (not mid-move or mid-roll;
    it cuts a move's recovery at its cancel point, like a drink press). Bare hands can't, and putting
    the weapon away lets go (a swing under way keeps its grip to its end); swapping straight to
    another weapon keeps the grip.
    - Strength x1.5 (`gMelee.twoHandStrength`) as a "Two-handed" more modifier in `collect()`, so the
      character sheet, tooltips and hits all see it.
    - The weapon guards better: 85% of the damage, boost 0.45 (`twoHandNegation`, `twoHandBoost`;
      one-handed 75% / 0.35, a heater shield 100% / 0.56).
    - The off hand does nothing: `shieldOf` is null (no shield guard, no bounce) and nothing parries,
      not even a knife. The shield still weighs on the equip load.
  - **What shows:** at rest the weapon is held in front, the tip up and forward; in every pose (the
    guard, every swing of every class; not in a roll's tuck or swimming) the left hand sits on the hilt below the right, `kPommel`
    voxels down the weapon's length, easing there over about 0.2 s (`gripBlend`). Drinking, it lets go
    for the flask. A shield hangs on the back, face out. The HUD shows a "2H" tag right of the bars;
    tooltips show a weapon's grades; the character sheet shows what each attribute adds now and the
    grip. The equip clink plays on toggling.
  - **Saves:** a `GRIP` section, a byte (anything but 0 or 1 is rejected). Older saves: one-handed.
  - **Debug window:** a "Scaling and two-handing" node in the Player tab: the toggle, the hand's
    blend, the strength and guard knobs, and every weapon's scaling and what it adds now.
- **Alternatives considered:**
  - Elden Ring's curve per attribute level (soft caps): with attributes in the single digits so far it
    reads the same as a straight line. A curve can come when attributes grow past ~20.
  - Strength x1.5 only inside the damage formula: the modifier is simpler, shows on the sheet, and
    strength has no other use yet.
  - Its own two-handed moveset (the roadmap's wording): the one-handed timings with both hands read
    well, and real two-handed movesets come with greatswords, which are two-handed weapons.
  - Hiding the shield: on the back says where it went, at the cost of one pose.
  - One stat per attribute's scaling: one `scaling` stat covers "+10% scaling" talents; a talent bent
    to one attribute can be a condition later.
- **Consequences:**
  - **Balance:** the story fights' bands and the level bands measure the same as before (the sword's
    damage didn't move at the starting attributes). The axe and mace hit about 1 harder; two-handed,
    the mace about 2 more.
  - **Tests:** the `scaling` test checks the grades and their edges, each attribute's share, rounding
    once, the Scaling stat, the shipped weapons' leanings (agility favours the knife, strength the
    mace), the toggle (its sound, strength x1.5 in the sheet, more damage, no weapon no grip, letting
    go on unequip but not on a swap, a swing keeping its grip, waiting out a swing and a roll), no parry
    two-handed, and the pose (the left hand on the hilt toward the pommel at rest, guarding and
    mid-swing, letting go to drink, the weapon tucked in a roll as one-handed, the shield's board
    outside the torso's back). The melee test blocks the same hit one- and two-handed. The data test rejects scaling
    on non-weapons, unknown attributes and out-of-range values; the save test round-trips `GRIP` and
    rejects bad ones. Mutation-checked: each of 34 rules broken alone fails a test.
  - **Talents:** "+10% scaling" is an increased modifier on `scaling`; a two-handed condition can key
    on `twoHanding()`.

## ADR-049: Great weapons: greatswords and greataxes

- **Status:** accepted (2026-11-09)
- **Context:**
  - Two roadmap weapon classes: "Greatswords / two-handed swords: slow, high stance, wide sweeps" and
    "Greataxes". ADR-048 built two-handing and left two-handed movesets to them.
  - Every swing reached the same 2 blocks and 60 degrees either side (`kPunchReach`, `inReach`),
    whatever the weapon.
  - Foundation 5 (TALENTS.md): no new use-based skills.
- **Decision:**
  - **Two classes**, appended: `greatsword` and `greataxe`, each with a moveset in
    data/weapon_classes.json, swing poses (`kGreatswordSwings`: wide sweeps both ways, a diagonal,
    overhead smashes, thrusts for the rolling attack and riposte; `kGreataxeSwings`: the axe's chops,
    bigger, the head leading, a wide horizontal cleave for light 4 and heavy 2) and held models with
    long grips (`ModelGreatsword`, `ModelGreataxe`; the left hand `pommel()` voxels below the right).
  - **Always in both hands:** a class's `two_handed` makes `twoHanding()` true while its weapon is in
    hand: ADR-048's strength x1.5, the better guard, the shield on the back, no parry. The grip press
    does nothing then; the player's own `twoHanded` flag is kept, so a two-handed sword stays so after
    swapping through a great weapon.
  - **Slow and heavy:** lights about 1.05 s (greatsword, speed 1.45) and 1.12 s (greataxe, 1.55)
    against the sword's 0.69; 18–30 stamina a swing; the most stance damage (lights 17–22 and 19–24,
    so every light makes a poise-12 ghoul flinch, as the mace's do; only the last lights reach the
    chief's 20);
    hyperarmor on the heavies, the jumping heavy and the guard counter; hit-stop x1.35 and x1.5; every
    swing plays the heavy whoosh, deeper.
  - **Reach and arc per class:** `reach` (blocks, default 2) and `arc` (degrees either side, default
    60) in weapon_classes.json, used by `inReach` with the held weapon's class, so `punchTarget`,
    `criticalTarget` and NPCs' swings reach by weapon. Greatsword 2.6 / 75, greataxe 2.5 / 70.
  - **Items:** Iron Greatsword (9–13, strength A 1.1, agility D 0.4, weight 9) and Barrow Greataxe
    (10–14, strength S 1.45, weight 10), save ids 17 and 18.
  - **Found as rare drops:** creatures take an optional `rare_loot` / `rare_loot_chance`, rolled only
    when the usual drop's roll fails (a corpse holds one item, so saves don't change; kinds without one
    draw exactly as before). Bandits: the greatsword at 10% (6% of kills); barrow ghouls: the
    greataxe at 12% (8%).
  - **Skill:** they earn One-Handed XP as swords do (foundation 5).
  - **UI:** class icons, a tooltip line (reach, two-handed), the debug moveset node shows reach, arc
    and two-handedness, and the two-handing node says a great weapon is always two-handed; the
    creature tables and the MCP catalog show rare drops.
- **Alternatives considered:**
  - One-handing a great weapon with enough strength (Elden Ring's requirement): attributes are in the
    single digits, so it would be a wall, not a choice. Always two-handed is simpler and reads clearly.
  - Hitting every foe in the arc (cleaving a pack): a real difference, but it reshapes the hit
    resolution (one target per swing today). A wider arc and longer reach give the sweep its feel;
    multi-target swings can come with colossal weapons.
  - A list of drops per creature: one rare drop covers the need with a smaller change and no save
    change.
  - A new heavy enemy wielding them: the story world's spawns are authored in the editor with the
    user; the bandit and ghoul drops work in both worlds now.
  - A Two-Handed skill: foundation 5 forbids new use-based skills.
- **Consequences:**
  - **Balance:** with the sword nothing changes (the story fights and level bands measure the same).
    The story fights now also run with the axe, the mace and both great weapons (health left, 12
    seeds): chief 38 / 27 / 40 / 52 (sword 32), wolves 41 / 5 / 15 / 6 (31), ghouls 53 / 65 / 72 / 72
    (21). Heavy weapons win where openings are few; quick wolves punish slow swings. Per second of
    lights at level 1: sword 14.6, greatsword 16.2, greataxe 17.7.
  - **Tests:** `greatWeapons` checks the classes' flags and reach, the items' leanings and weights,
    stance and hit-stop above the mace's, hyperarmor on heavies only, swing lengths, always
    two-handed (strength, no shield or parry, the grip press ignored, the sword's grip kept across a
    swap), lights landing at 2.3 blocks and 70 degrees for the greatsword and not the sword, NPC and
    backstab reach, the light chains with their stance and hit-stop, the rare drops' rates and the
    first roll's draw unchanged, both hands on the long grip in every pose, and the blade, pommel and
    beard staying out of the ground through every move. The data test rejects bad `two_handed`,
    `reach`, `arc` and rare drops (and one behind a certain drop, never rolled); the sound test checks
    the deeper whoosh. The draw order of a bandit's two rolls is pinned, and a sword's grip survives
    a swap to a great weapon mid-swing. Mutation-checked: each of
    29 rules broken alone fails a test.
  - **Rng:** a bandit or ghoul whose usual drop fails takes one more draw from its rng, so what it
    does after a respawn shifts; a pinned run through a respawn would need re-pinning (none does).
  - **NPCs:** an NPC sees a swing coming by the attacker's weapon too (a greatsword's arc and reach).
    No creature wields a great weapon; one would two-hand it (no shield), but its AI still spaces
    itself at a sword's length.
  - **Later:** colossal weapons and great hammers can be more two-handed classes with data alone
    (plus poses and a model).

## ADR-050: Bows, arrows and projectiles

- **Status:** accepted (2026-11-09)
- **Context:**
  - The roadmap's ranged section: "Bows: draw and hold, aim mode (over the shoulder), arrows as items,
    arc and drop", "Projectiles in the sim: travel, hit detection, sticking into terrain, blocked by
    guards" and "Lock-on aiming for ranged weapons". Crossbows, throwables, ranged enemies and staff
    spells all need the same projectiles.
  - Every hit was a melee swing landing at its impact frame on whatever `inReach` found, resolved in
    one block of `updateCombat`.
  - The inventory has no stacks: containers hold one item per slot.
  - Foundations (TALENTS.md): no new use-based skills; pressed actions share one path.
- **Decision:**
  - **Projectiles in the sim** (`projectile.h/.cpp`, `Session::projectiles`): an arrow is a tip
    position, a velocity, the bow that loosed it, its power and stance damage. Each tick it falls
    under gravity (10 blocks/s², flatter than bodies' 28) and is swept from where it was to where it
    goes: a voxel ray (`raycast`, Amanatides-Woo) against the terrain and a segment-box test against
    every living, attackable body (`hitBox`: 0.4 blocks either side, as tall as its model), the
    nearest first. Nothing tunnels: a test fires 600 blocks/s at a one-block wall. No randomness in
    flight; at most 64 (the oldest stuck one goes first, else the oldest); gone after 5 s in flight
    or on leaving the world that's in.
  - **One hit path:** the player's hit on an NPC moved out of `updateCombat` into `strike()`; swings
    and arrows both call it (crit roll, resistance, `resolveHit`, the NPC's guard from the direction
    it came from, statuses, noises, XP, Killed, stance). A swing (`melee`) also freezes both bodies,
    bounces off a greatshield and shakes the camera; an arrow does none of those. The arrow's roll
    and skill are its bow's (the player's swing weapon, set for the moment of the hit). A dodge's
    i-frames let an arrow through, and it flies on. The rng draws and story bands are unchanged.
  - **Terrain:** an arrow stops where it went in and stays 30 s; walking within 1.3 blocks of it
    picks it back up (a quiver not yet full). It makes a thunk by the block it hit.
  - **The bow** (an appended `bow` class, `ranged` and `two_handed`): its one move is the shot
    (`Move::Shot`, appended; a ranged class has only it, melee classes don't have it, and the data
    check says so). The attack button draws at once (no tap-or-hold wait): the windup to the hold
    point is most of the draw, then it holds there while the button is down, the draw building over
    `drawSeconds` (0.6 s) to full (a creak; it holds on at full draw, no auto-release). Letting go
    looses at the impact frame: speed 20 to 55 blocks/s and x0.4 to x1 the damage and stance by the
    draw. A shot costs 8 stamina and an arrow; with the quiver empty the press does nothing. A roll
    or the flask lets a draw down (the arrow kept). No guard and no parry with a bow; no riposte or
    backstab either (an arrow can't be one). Short Bow: 5–8 pierce, agility B 1.1, strength E 0.3,
    weight 2, save id 19.
  - **Aim mode:** the guard button with a bow aims instead: the camera comes in over the right
    shoulder (3 blocks back, 1.1 to the right; a finer mouse), a crosshair shows (closing in with the draw), the body walks
    at 2.5 blocks/s facing the camera, and the shot goes straight at the first terrain or body under
    the crosshair (the arrow still drops: far shots need holding over). Outside aim mode a shot goes
    the way the body faces (3 degrees up), or locked on, the low arc that comes down on the target
    (45 degrees when it's out of reach). NPCs see a drawn bow as a threat as a swing: one that would
    parry guards instead.
  - **Arrows as a count** (`Combatant::arrows`, at most `kQuiver` 30): resting by a fire or getting
    back up fills it, picking arrows up tops it up. Saved in a new `ARRW` section (missing: full).
  - **Pose and models:** `ModelBow` (a bowed stave and a straight string), `ModelBowDrawn` (the string
    pulled back to a point) and `ModelArrow`, drawn per projectile like a held item. The bow hangs from
    the left hand at the side; raised (aiming, or through a shot) the body turns side on, the bow out in
    front of the face and the right hand drawing the string to the cheek, a nocked arrow between
    the hands until the release.
  - **Found:** guards drop the bow half the time (they're passive: you have to start it).
  - **Sounds:** a draw creak (and a last one at full draw), the loose's twang, a thunk into terrain
    (pitched by stone, wood or earth), an arrow into flesh; CC0 (CREDITS.md).
  - **HUD and UI:** an arrow and the quiver's count right of the health bar with a bow in hand (in
    place of the 2H tag), the crosshair, a bow icon, a tooltip line. Debug window, Player tab: a
    "Bows and projectiles" node (arrows, the draw, every knob, the live projectiles, clear, reset).
  - **Skill:** a bow earns One-Handed XP, as every weapon does until talents (foundation 5).
- **Alternatives considered:**
  - Item stacks for arrows: real items you loot and carry, but a save format change, stack UI in the
    bags and splitting rules, for one item kind. A count like the flasks is a few lines; arrow items
    can come with crafting.
  - Hitscan shots (a ray at release): no flight, no drop, nothing to dodge, and the same code would
    not serve spells or thrown pots.
  - Spawning arrows from `updateCombat`: it has no session (no projectile list, no aim point); a
    `loosed` flag hands the release to `step()`.
  - Auto-releasing at full draw (as a charged heavy does): a bow is held drawn to aim.
  - A charge on the heavy button only, light shots on a tap: Elden Ring's bows have no draw; Skyrim's
    draw by hold time reads better with one button, and the aim mode needs the guard button free.
  - Bows for NPCs now: their AI only swings and spaces itself at a sword's length; archers that keep
    their distance are the next ranged step, and the data check refuses a ranged NPC weapon until then.
- **Consequences:**
  - **Balance:** melee is unchanged (`strike()` draws the rng as the old block did; the story
    fights and level bands measure the same). A new level band for the bow: the starting player kills
    a level 1 boar in 9 full draws (about 7 s) and a level 2 bandit in 13 (about 10 s), against the
    sword's 11 swings (10.8 s) and 12 (11.8 s): a little quicker, from range, for an arrow a shot.
  - **Tests:** `bows` checks the class (only the shot) and its data checks, the draw (at once, held at
    full draw, loosed on letting go, the noises), a tap's slow weak shot, range by draw against the
    arc's formula, an empty quiver, letting a draw down with a roll, aim mode (no guard or parry, the
    slow walk, straight at the crosshair's point), gravity on the step, no tunnelling up to 600 blocks/s,
    pickup (not into a full quiver), expiry stuck and in flight, the cap, hits through `strike()` with
    the bow's roll and XP, the same shots giving the same result, a kill's Killed, a guard blocking from
    the front (no hit-stop, no recoil) and not from behind, a dodge letting it through, lobbed shots
    hitting a locked-on dummy 30 blocks off from a tap and a full draw, the aim maths, the pose (hanging
    out of the ground, raised, drawn, an arrow until the release), rest filling the quiver, and the
    sounds. The save test round-trips the count and rejects bad ones; an NPC that always parries guards
    a drawn bow. Mutation-checked: each of 58 rules broken alone fails a test (a 56th, aiming not cutting
    a shot's recovery, changed nothing and was dropped).
  - **Performance:** arrows are model instances like held items. A/B on the story world, 4 alternating
    runs each against e6e6a90: the display holds every run at 120 fps (8.33 ms, p99 9.4–9.6 ms) for the
    old build, this one and this one with 64 arrows on screen; even 16,000 arrows (250 times the cap)
    hold 120 fps with an unchanged p99, so their cost is below what the frame cap lets us measure.
  - **Limits:** an arrow's hit reads the player as it is when it lands (gear swapped mid-flight
    counts); an arrow that sticks within reach (shot into a wall at point-blank) is picked straight back up.
  - **Later:** crossbows (load and fire), throwables and archer enemies on the same projectiles;
    staff spells as projectiles with their own model; arrows as items with crafting; arrows stuck in
    bodies.

## ADR-051: Archers that keep their distance

- **Status:** accepted (2026-11-09)
- **Context:**
  - The roadmap's enemies section: "Ranged enemies keeping their distance (needs projectiles)".
    ADR-050's arrows flew one way, player to NPCs, and its data check refused a ranged NPC weapon.
  - The NPC→player hit was one block of `updateCombat`, used only by swings landing in reach.
  - NPCs fight at a sword's length: Chase closes in, attack tokens decide who swings, the rest circle.
  - Foundations (TALENTS.md): pressed actions share one path, so an NPC's shot goes the way the
    player's does.
- **Decision:**
  - **One hit path the other way:** the NPC→player block moved into `strikePlayer()` (entity.h), the
    mirror of `strike()`: a random critical, `resolveHit` with the NPC's swing weapon times
    `Strike::power`, then the player's parry (swings only), the guard from `Strike::from` (a fresh one
    is perfect), else a flinch from `Strike::stance` unless hyperarmor; statuses, armor XP, noises,
    camera shake. A swing (`melee`) also freezes its attacker on the guard and bounces off a
    greatshield; an arrow does neither. The rng draws are as before, so the story fights, level bands
    and replays are unchanged.
  - **Arrows have an owner** (`Projectile::owner`: 0 the player, else an `Entity::id`). An archer's
    arrow tests only the player's box (no friendly fire), skips a rolling player (i-frames), hits
    through `strikePlayer` with the archer's stats and the bow that loosed it (its swing weapon, set
    for the moment of the hit), and a wall in the way stops it. With its archer gone (despawned) an
    arrow is no one's and flies on. Missed arrows stick and can be picked up, as the player's.
  - **The archer's shot** is the player's: `Move::Shot` with its own hold. It raises the bow, holds
    the draw `gAi.drawHold` (0.8 s, x 0.85–1.15; the draw builds in Windup as in updateMelee, the
    creak at full draw) while turning after the player, stops turning 0.12 s before the release, and
    looses at the impact frame (`loosed`, then `step()` calls `loose(s, e)`): lobbed at the player's
    chest as far away as the player is but along its own facing, off by up to its kind's `aim_error`
    either way, up or down. No lead: a sidestep late in the draw, a roll or a shield beats it. Its
    quiver never runs out. A hit that makes it flinch mid-draw spoils the shot.
  - **Keeping its distance** (`keepDistance`, Chase with the bow out): too far, or no line from its
    bow to the player's chest (`clearLine`, the voxel `raycast`), it steps in; too close (inside
    `keep_range`'s min) it backs off facing the player while there's room; in its band, or cornered,
    it shoots once its `attack_cooldown` has passed since its last shot started (the first comes at
    once), and strafes round the player between shots. It gives up stuck (as a chase does).
  - **Up close** (`sidearm`): once the player is within `gAi.closeIn` (3 blocks) in a fight it takes
    out its knife and fights with it in the pack with attack tokens, slowly (its cooldown spaces its
    swings too: a poor fighter up close, a fallback, not a second bandit); the bow comes back past its
    band's min or `closeIn`, whichever is farther (no flip-flopping), never mid-move, and when the
    fight ends. With the bow out an
    archer takes no side and no token (it isn't on the ring) and doesn't guard.
  - **Data:** a `weapon` that shoots needs `keep_range` (`[min, max]`, 2 <= min < max <= 40) and
    allows `aim_error` (0..0.5 rad) and `sidearm` (a main-hand weapon that swings); all three need
    one, and it can't `guard`, `parry` or hold a `shield`. `bandit_archer` (save id 8, `EntityKind::BanditArcher`):
    level 2, 9/2/4/2/2 (90 health), aggressive at 14 blocks, the Short Bow, keeps 7–16 blocks,
    ±0.07 rad, a shot every 3.2 s, the Rusty Knife up close, dodges 25%, drops the bow 25%.
  - **Where:** at the bandit's camp in the default spawns (the endless world's start, and worlds
    without markers), a few blocks behind the bandit; F6 and the Entities tab spawn it anywhere.
    The story world is untouched (its spawns are authored in the editor).
  - **Debug window:** the Entities tab shows an archer's band, aim, hands, line of sight and draw;
    the AI node has the draw hold and the sidearm distance.
- **Alternatives considered:**
  - Arrows that can be parried: a parry's window is timed against a swing's arc, and Elden Ring's
    parries don't take arrows either; a parry meeting one is a weak guard.
  - Aiming at the player's position at the release (or leading it): nothing would beat it but a roll
    or a shield. Along its facing, which stops tracking just before, a late sidestep works too.
  - A kick or a bow bash up close instead of a sidearm: a new move and pose for one kind; the knife
    reuses the bandit's moveset, and data can give any archer any sidearm (or none: it shoots
    point-blank).
  - Attack tokens for shots too: archers stand out of the ring, and a token held at 14 blocks would
    starve the melee fighters; one archer per camp needs no limit yet.
  - A data-only kind (no `EntityKind`): the default spawns name it in code, as they name the bandit.
- **Consequences:**
  - **Balance:** melee is unchanged (the story fights and level bands measure the same). A new band:
    the starting player (shortsword, no armor) rushing an archer from 14 blocks wins 12 of 12 with
    87 health left on average (its first arrow lands as the player closes in, then the slow knife);
    rushing a bandit with an archer behind it wins 9 of 12 with 21 left, against 28 for two bandits
    and 56 for one, played the same way: an archer covering a fighter is about as much trouble as
    a second fighter.
  - **Tests:** `archers` checks the data (and its checks), the shot (at once, the creaking tell held at
    full draw, the arrow at the player, lobbed, its hit and sounds, back to Chase after "shot"), a
    shield blocking from the front (not from behind, no hit-stop or recoil), the sword's guard letting
    some through and a perfect guard none, a flinch by the arrow's stance against the poise, a roll
    through it, no friendly fire, the player's arrows still hitting NPCs, a wall stopping it, an
    ownerless arrow, a mid-draw flinch, the band (backing off facing the player, closing in, strafing
    between shots), no shot without a line of sight, a cornered shot, the sidearm's hysteresis (and
    none mid-swing, the bow back on death, no flip-flop with a far `closeIn`, the knife's slow swings), no ring, side or guard with the bow, `strikePlayer`'s
    power and parry rule, the aim's spread both ways and along the facing, determinism and the band
    above. Mutation-checked: each of 51 rules broken alone fails a test.
  - **Limits:** an archer reads the player's position, not the world: no pathfinding round a wall it
    can't see past (it steps in, then gives up); several archers can shoot at once. An arrow's hit
    reads its archer's gear as it is when it lands (the knife out by then: its modifiers), as the
    player's arrows do. The tick it takes out its knife it still holds its token from the tick before.
    Blocking an arrow opens a guard counter, as blocking a swing does.
  - **Later:** crossbows and throwables (likely ADR-052) on the same path; an archer limit if camps
    grow; arrows stuck in bodies.

## ADR-052: Crossbows: load and fire

- **Status:** accepted (2026-11-09)
- **Context:**
  - The roadmap's ranged section: "Crossbows: load and fire, slower and stronger, no draw time".
  - ADR-050 built the bow on one move (`Move::Shot`: draw, hold, loose at the impact frame) and the
    projectile path; ADR-051 made it shoot both ways. A crossbow differs in when its work happens:
    the effort (spanning the string) comes after a shot, not before.
  - Foundations (TALENTS.md): pressed actions share one path. The reload must not be a separate
    button or system.
- **Decision:**
  - **A ranged class that `loads`** (WeaponClass::Crossbow, data `"loads": true`, needs `ranged`) has
    two moves in data: the shot and a new `Move::Reload` (only classes that load have it). The bolt in
    it is `Player::loaded`, the crossbow's ItemId: put away it stays in, and a bow in hand shoots its
    own arrows. It isn't saved: a crossbow comes back from a load empty.
  - **Loaded, a press fires at once:** `pick` returns the shot; it doesn't hold (no draw: always full
    power), raises to the shoulder by its hold point and fires at its impact frame (0.24 s), a bolt at
    `gRanged.boltSpeed` (75 b/s, against an arrow's 55 at full draw: a flatter flight). `loose` spends
    the bolt in it, not the quiver.
  - **Then it reloads, as a press would:** at the release updateCombat queues `Want::Light`, so the
    reload starts at the shot's chain point through the input buffer, and a roll (a newer want)
    replaces it. Empty, `pick` returns the reload (1.6 s, walking slowly): tipped nose down, the right
    hand drawing the string back to the latch, and at its impact frame (1.2 s) a bolt from the quiver
    is seated (a click). A roll or the flask before then lets it down, still empty, and the next press
    reloads first; a press late in the reload (within the input buffer, 0.5 s, of its end) fires as
    it ends. The queued reload is marked (`reloadWant`): a weapon swapped in before it starts drops
    it, it isn't a press. Decided from what's in hand now (not the swing's weapon), and a reload goes
    on at 0 stamina (it costs none). An empty quiver drops the press (a loaded crossbow still fires
    its bolt). NPCs don't answer a reload as an attack (it hits nothing).
  - **The Light Crossbow** (save id 20): 12–16 pierce with no attribute scaling (its power is in the
    mechanism: as strong in weak hands), shot stance 20 (a bow's 9), weight 3.5. **Guards drop it**
    (50%) instead of the bow; archers drop the bow (ADR-051).
  - **One quiver:** bolts and arrows are the same count (ADR-050's quiver), picked up the same way.
  - **Look and sound:** `ModelCrossbow` (slack), `ModelCrossbowSpanned` (a bolt in it, or the reload
    past its seat) and `ModelBolt` (an arrow's grid, shorter, tipped the same so `arrowPose` serves
    both); poseModel puts both hands on it (the trigger hand on the grip, the other under the
    fore-stock): low at the right hip at rest, the butt at the right shoulder to aim or fire, tipped
    nose down to reload. Sounds from the bow's CC0 samples: the fire a deeper snap with a knock, the
    reload a lower creak, the seat the equip click.
  - **HUD:** under the quiver's arrow a bar lit while loaded; the crosshair is tight loaded, open empty.
    The inventory names it ("press to fire, it reloads after"). **Debug window:** the Bows node shows
    loaded or empty, the reload's progress and a Load button, and the bolt speed slider.
  - **Creatures can't carry one yet** (a data error): an NPC would need the reload in its AI.
- **Alternatives considered:**
  - The reload as the shot's recovery (one move): a roll out of it would load for free, or need the
    loaded flag anyway; two moves keep each one plain.
  - Reloading on a press only (no automatic reload): two presses a shot. Queued like a press it's
    automatic and still cancellable, and the buffer lets a press during it fire at its end.
  - One-handed with a shield in the off hand: the guard button aims, so a shield would need the
    controls refactor (deferred); two-handed for now.
  - Bolts as their own count: a new save field and HUD count for no play difference yet.
  - Attribute scaling: the bow already rewards agility; the crossbow's niche is power without stats.
- **Consequences:**
  - **Balance:** with the starting stats a bolt averages 1.4x a full-draw arrow on a bandit (14 vs
    10; tested over 12 seeds), and breaks stances twice as fast, but a shot every ~2 s (0.6 + 1.6)
    against the bow's ~1.6 s: about even early, the bow ahead as agility grows.
  - **Tests:** `crossbows` checks the data and its checks (`loads` needs `ranged`, the reload only and
    always with it, no creature with one), reloading on a press (the crank, the seat's timing, the
    bolt from the quiver, the spanned model, no shot while held), firing at once (no hold, full power,
    the bolt's speed, along the facing, the quiver untouched), the queued reload, a press buffered
    through it, a roll in the reload (empty, the arrow kept), a roll after the shot (no reload), a
    roll in the raise (still loaded), an empty quiver, the bolt kept through a swap to the bow (and
    another weapon's bolt not counting), the automatic reload dropped by a swap to a sword, a reload
    at 0 stamina, what's in hand deciding mid-swing, NPCs not answering a reload, aim mode (straight
    at the point; walking slowly in the reload), the damage against the bow, the models and the pose
    (hands on it, level at the right shoulder, as at rest a bow's hand, the bolt in the groove, tipped
    to reload with the hand on the string's way). Mutation-checked: each of 32 rules broken alone
    fails a test.
  - **Limits:** no NPC crossbows; the loaded bolt is lost on a save and load; bolts and arrows share
    one count; only haste (as with every move) makes the reload faster. A press early in the reload
    expires before it ends.
  - **Later:** an NPC crossbowman (keepDistance with a reload between shots); heavier crossbows;
    throwables on the same path.

## ADR-053: Throwables: knives, bombs and poison pots

- **Status:** accepted (2026-11-10)
- **Context:**
  - The roadmap's ranged section: "Throwables (knives, pots, bombs)". ADR-050 built projectiles and
    the one hit path (`strike()`); ADR-052 the second ranged class. Throwables are the last ranged
    item, and the first that isn't a weapon in hand: they're used beside whatever is held.
  - Foundations (TALENTS.md): pressed actions share one path, no use-based skills. A throw must be
    a move like the others, its hits the same strikes.
- **Decision:**
  - **Items with a `throw` record** (data, no slot, an `attack` required): `look` (knife, bomb or pot:
    its model), `speed`, `lob`, `radius` (a knife none: it sticks; a bomb or pot some: it bursts),
    `pouch` (how many it holds), `stance` and its own status build-up (`bleed`, `poison`, `frost`).
    Three, appended: the **Throwing Knife** (21: 4–6 pierce, agility 0.6, 40 bleed, stance 6, flat
    at 32 b/s, a pouch of 10), the **Clay Bomb** (22: 16–22 strike, no scaling, stance 40, lobbed at
    14 b/s, 2.5 blocks, 3) and the **Poison Pot** (23: 1–2 strike, 110 poison, lobbed, 2.5 blocks, 3).
  - **A pouch, not bag items** (as ADR-050's quiver): `Player::pouch` counts each by ItemId and
    `readied` is the one the throw button throws; saved in a THRW section. A new game, a rest by a
    fire and getting back up fill each to its pouch (never lowering one).
  - **The throw is a move** (`Move::Throw`, appended; code's, `kThrowSpec`: 0.7 s, drawn back by 0.25
    s, let go at 0.35 s, 8 stamina, chain at 75%, cut at 70%). G / the touchpad press (`Want::Throw`)
    starts it once the body is free, on the ground, with stamina (it waits in the buffer for some)
    and one of the readied kind left, not with a bow or crossbow in hand (both hands are full); else
    the press does nothing at all (no draw let down, no reload or recovery cut). A press can cut an
    attack's recovery like the flask's. NPCs answer a close throw's windup like an attack (guarding
    instead of parrying: a throwable can't be parried). It walks
    slowly and turns toward the aim until it lets go; a flinch before then keeps it in the pouch. X
    readies the next kind that has any (or click one in the Bags window's pouch row).
  - **Thrown:** at its impact frame updateCombat sets `hurled` and `step()` calls `hurl`: one out of
    the pouch, a projectile whose weapon is the throwable, from the chest, at its speed: along the
    facing (`hipPitch` up, or `gRanged.lobPitch` 0.4 rad if it lobs), or locked on in the low arc
    onto the target (aim mode is a bow's: there's no throwing with one in hand).
  - **A knife** hits like an arrow, through `strike()` with the knife's stats (the swing weapon swap)
    and `Strike::buildup`: its own build-up, instead of the thrower's (buildUp's `own`: a Rusty
    Knife in hand doesn't make a pot bleed). It sticks in
    terrain and walking over it puts it back in the pouch (an arrow still goes to the quiver).
  - **A bomb or pot bursts** on the first body or voxel in its way (a little short of it): `burst`
    strikes every living, attackable NPC within the radius of the burst point and in sight of it
    (a voxel ray to its middle; a dodge's i-frames let it through), its damage, stance and build-up
    from 1 at the middle to `burstEdge` (0.5) at the radius, a guard facing the burst blocking it as
    any hit (one right over it, from where it flew in). The dead are left alone; a kill is a kill. It never hurts the player (its own throw). A bomb shakes the camera by how near it was.
  - **Look and sound:** a left-hand overhand throw (the arm up behind the shoulder, the body turned,
    then whipped through; a shield or the two-handed grip let go meanwhile), the throwable in the
    hand until it lets go (poseModel's `flask` output); in flight a knife points along its way, a
    bomb or pot tumbles. A burst is a puff that swells and goes in 0.35 s, fire or a pale green
    cloud, drawn **glowing** (unshaded: `ModelInstance::glow`, a spare bit of the shader's packed
    `blockAndQuad`, the quad index now 15 bits for the debug view; no new vertex output). Sounds: the
    throw a quick whoosh, a knife the arrow's, a bomb TinyWorlds' CC0 explosion over a heavy hit, a
    pot rubberduck's CC0 breaking clay with the poison hiss.
  - **UI:** the readied one's icon and count left of the flask; the Bags window's pouch row (click to
    ready; tooltips); the debug Player tab's Throwables node (counts, readied, a fill, the knobs).
- **Alternatives considered:**
  - Throwables as bag items (stacks): the inventory has no stacking yet; a pouch count is ADR-050's
    quiver again and can become items when stacks exist.
  - A quick-item slot cycled with the flask's button: it would change the flask; the controls
    refactor (deferred) can merge them. G / touchpad and X are free.
  - The right hand throwing (the weapon put away): a stow and draw for every throw; the left hand is
    the flask's already. With a bow or crossbow, both hands are full: no throws.
  - Bursts hurting the player: a thrown bomb at your feet would be a trap with no upside yet; NPC
    throwers, when they come, will hurt the player through `strikePlayer`.
  - A lingering poison cloud: a burst is one strike; a cloud needs an area effect over time, later.
- **Consequences:**
  - **Balance:** with the starting stats a bomb averages 20 on a bandit at its middle and 13 near its
    edge (12 seeds), about two sword swings each, to everything in reach, three a rest. A knife does
    4–6 and three bleed a bandit (40 a hit, 100 to fill). A pot poisons anything without poison
    resist at its middle (a ghoul shrugs it off). Melee, bows and the story fights are unchanged.
  - **Tests:** `throwables` checks the data and its checks (no slot, a look, a speed and a pouch, a
    knife without a radius, a bomb or pot with one, an attack), a new game's full pouch, a throw
    (the move, its stamina, the timing of the release, one out of the pouch, the speed and way, the
    whoosh), a knife sticking and picked back up (not past a full pouch; an arrow to the quiver),
    nothing to throw or a bow in hand doing nothing (no draw let down, no recovery cut; also when it
    changes while the press waits), waiting for stamina, a flinch keeping it, walking slowly and
    turning to the aim, cutting a swing's recovery, a great weapon not slowing it, NPCs answering it
    with a guard (never a parry), readying the next kind, a rest and a respawn filling it, a knife's
    hit and its own bleed (not the weapon in hand's; a pot no bleed), the dead left alone and a kill
    counted, a bomb's shake, a bomb lobbed with nothing to aim at bursting where it lands (the puff
    and its sound, gone after), a pot landing on a lock-on target, a burst striking near and edge
    bandits (less at the edge), none past it or behind a wall, never the player, a pot poisoning a
    bandit and not a ghoul, a dodging bandit untouched, the models, the puff's size over time, and the
    pose (drawn back up behind the shoulder with it in the hand, out in front and gone at the
    release, back at rest after). Saves round-trip the pouch; an older save gets a full one; unknown
    ids are dropped with a note and counts clamped. Mutation-checked: each of 44 rules broken alone
    fails a test.
  - **Limits:** throwables aren't bag items, can't be looted or bought yet (a rest refills them);
    no NPC throws; bursts are instant (no lingering cloud) and don't hurt the player; no throws with
    a bow or crossbow in hand; on a pad, readying another needs the Bags window, and the throw is the
    touchpad press, which Xbox and Switch pads lack (the deferred controls refactor will rebind it). Line of sight is one
    ray to a body's middle. A bomb that flies out of the loaded world or for 5 s is gone unburst. A
    save made before a kind was added has none of it until the next rest.
  - **Later:** NPC throwers (a bandit's fire pot, through `strikePlayer`); frost pots and other kinds
    are data; elemental damage with magic; a lingering cloud; throwables as stackable items.

## ADR-054: The arena: a combat test world, and spawning groups

- **Status:** accepted (2026-11-11)
- **Context:**
  - Trying out a fight meant finding a fitting place in the endless or story world and pressing F6
    once per NPC, one kind at a time, always 5 blocks ahead. Mechanics that depend on terrain (high
    ground, cover from arrows and bursts, water, footing, corridors) had no fixed place to test them,
    and the review pages had no repeatable stage.
  - The user asked for a combat debug world, "arena", with different terrain, and spawning by
    count at points measured from its middle ("spawn 3 of Wolf at 0, 0").
- **Decision:**
  - **`worlds/arena.flyw`, built by code** (`src/arena.cpp`, `buildArena`), so it can be rebuilt
    and is tested: `flycoos --build-arena PATH` builds it without a window and saves it. A 1024²
    world file like the others (the editor edits it; rebuilding overwrites edits). Centred on
    `worldMiddle` (a world file's centre corner (512, 512), the endless world's origin).
  - **The ring:** a sand floor 112 across with a clay duel ring and (0, 0) marked, a wall 7 high
    (a walkway on top, too high to climb out), nine tiers of stands with plank seats, an outer brick
    wall with merlons, two gates with iron bars; outside, a flat grass plain.
  - **Zones,** each with a named point (poi marker) a radius around it: the Hill (a plateau 5 up, a
    ramp a block every two, stairs a block every one, a cliff), the Pillars (five columns 7 high, a
    wall 3 high that hides you, a kerb 1 high, crates), the Pool (a shallow end a block deep, a deep
    end four, a plank bridge a block wide on posts), the Rough (bumps 1 and 2 high), the Narrows (a
    L-shaped corridor 2 wide between walls 4 high) and the Targets. The player spawn is at the south
    gate by a brazier with three training dummies (a spawn marker); a region names the arena.
  - **Its pieces are prefabs** (structure files, so the editor can place them anywhere, and they're
    instances in the arena it can move): `arena_pillar`, `brazier` (embers at waist height: a rest
    point, ADR-047; five in the arena), `banner` (eight on the walkway), `arena_gate`,
    `archery_target` (hay, arrows stick in it), `crate_stack`.
  - **Spawning groups** (`spawnGroup`, entity.h): `count` of a kind (at most 256) around a point,
    the first on it and the others on the spiral spawn markers use, each in its column's standing
    place nearest the point's height (within 4 blocks: under a roof or a crown, not on it; else the
    column's top), facing the player; columns without ground (the endless world's unloaded ones)
    and non-finite points spawn none. Not from a marker, like F6's: not saved, not respawned. The
    debug window's Entities tab sets the kind, a count (1 to 16) and where: ahead (5 blocks, a
    bigger group a little further, at the player's height), or at x, z from the world's middle
    (on the ground under any trees there), typed or set by a button per named point (the arena's
    zones) or "Here"; F6 spawns the same, and both say when fewer landed than asked.
- **Alternatives considered:**
  - Building the arena by hand with the editor or the MCP tools: not repeatable or testable, and
    every layout change would be manual. Code first; hand edits on top remain possible.
  - A small world file (e.g. 256²): world files are 1024² (the grid and the renderer assume it);
    the plain around compresses to little (the file is 120 KB).
  - Generated terrain around the arena: scenery nobody fights in, and a seed's hills would sit
    against the outer wall unevenly. A plain is enough for a test world.
  - A `spawn` command on the MCP server: the command server edits the world, not the sim; captures
    stage fights with the same `spawnGroup`. Later if needed.
- **Consequences:**
  - Combat reviews get a fixed, varied stage; the new prefabs give the story world arena-like
    pieces. Nothing in the endless or story worlds changes.
  - **Tested** (`arena()` in core_test): `worldMiddle` for fixed and streamed grids; the shipped
    prefabs load without errors, the arena's among them; the build fails cleanly without them; the instance and marker counts, every
    marker valid; the middle flat with its marks, the wall too high, the stands rising, the plain
    outside, nothing just inside the wall and every banner's pole and cloth on the walkway, the
    gates' bars (too close to walk through) and passages, each zone's heights and blocks (ramp, stairs, cliff,
    water depths, bridge, wall, kerb, columns, the corridor clear through, the bumps' heights); the
    player starting at the gate able to rest, the three dummies; the world file round trip; groups
    on the point and the spiral, apart, facing the player, each its own seed, on the hill's top,
    under a canopy (on it only when asked at its height), on the top when asked far above, none
    without ground or at a non-finite point, at most 256, the same for the same seed.
    Mutation-checked: each of 22 rules broken alone fails a test (a 23rd, a banner's centre a cell
    off its pole, still leaves them on the walkway, which is what the check asks).
  - **Limits:** rebuilding overwrites hand edits; the spawn panel's points are relative to the world's
    middle in every world (only the arena is laid out around it); a group's spiral can put one on
    a crate or behind a wall it reaches; no compass in game, so the zones are told apart by
    their names.
  - **Later:** a `spawn` command for the MCP bridge; saved spawn presets ("3 wolves at the centre");
    sides (NPCs against NPCs); more zones as mechanics need them (ice, a ledge over a drop, doors).

## ADR-055: Pursuit: seen from afar, chased while in sight, searched for when lost

- **Status:** accepted (2026-11-11); supersedes ADR-020's leash (`leash_radius`, `reaggro_radius`)
- **Context:**
  - An enemy noticed the player within 8 to 14 blocks, and only if the player was within 16 of its
    home; it gave up the moment the player was 20 from its home, wherever the fight was. Kiting one a
    little way meant it turned round mid-fight and walked home. The user: "It's frustrating kiting an
    enemy and taking so much effort avoiding his attacks only to them stop following you."
  - Aggro had no line of sight: through a wall counted the same as in the open.
- **Decision:**
  - **Sight** (`canSee`, entity.h): nothing solid (an opaque voxel) on the line from 1.5 above an
    NPC's feet to 1.1 above the player's (the chest). One ray, all round (no field of view yet).
  - **A much larger aggro range, in sight only:** bandit 24 (was 10), wolf 28 (12), chief 24 (10),
    ghoul 16 (8), archer 30 (14). Measured in 3D as before (a cliff, another floor). No gate on the
    player's distance from its home any more; the cooldown after getting home (`reaggro_cooldown`)
    stays.
  - **No leash:** a chase goes on while the NPC sees the player, wherever that takes it. It ends
    when the player is `chase_range` from the NPC (not from its home: by default the larger of 32
    and twice the aggro range, must be more than it; 48 to 60 for the shipped aggressive kinds but
    the ghoul's 32, 32 for the passive ones), when the player is down, or after `give_up_seconds` unable to make
    headway (a player up a pillar), as before.
  - **Losing sight:** fighting, an NPC notes where it last saw the player (`lastSeen`; a hit tells
    it too, even one it reels from). Out of sight for `gAi.sightGrace` (0.5 s, round a corner, or the every-other-tick
    peek at a wall's end) the chase becomes a new state, `Search` (reason `lost_sight`): it runs to
    where it last saw them, then turns one way and the other, looking. Seeing them again
    (`found_target`, no shout: it's the same fight) or a hit puts it back in `Chase`; after
    `gAi.searchSeconds` (8) it gives up (`gave_up_search`) and walks home (`Return`: ignoring the
    player, healing on arrival). A searching NPC counts as in a fight: no resting by a fire near it.
  - **Walking home, a hit brings it back** (it used to ignore hits all the way): with no leash the
    walk can be long, and free hits on it were an exploit. Not when it gave up for want of a way to
    the player (`lost_target`, a player up a pillar) and the player is still out of reach: then it
    keeps walking, so arrows from up there don't keep it pinned below, but a player come down to stab
    its back gets a fight. A searching NPC of a timed marker stays past its window till it's done.
  - **Going round things:** chasing, searching or going home, a blocked step tries 45 degrees to
    one side, the other, then 90 (`detour`), checking the body's width (`roomy`: the ground ahead of
    both its edges, so it doesn't catch a corner; a slanted doorway a body just fits still works). A
    detour counts toward giving up and a free step only winds that clock back, so it can't circle a
    pillar with the player on top forever (an archer keeping its distance still resets it, as before).
  - The debug window: two knobs in Fighting AI (chases out of sight, searches for); per NPC its
    aggro and chase ranges, whether it sees the player, how long it hasn't, and how far off it last
    did. `leash_radius` and `reaggro_radius` are gone from `creatures.json` (no shipped kind set
    them); `chase_range` is new.
- **Alternatives considered:**
  - Keeping a leash, just longer: the user asked for the opposite, and any leash turns back a fight
    the player is still in.
  - Searching by wandering about the last place seen, or tracking the player's trail: more to
    tune, and a search that finds a hidden player through walls feels unfair. One spot and a look
    round is readable; a trail can come with stealth.
  - Pathfinding (A* on the voxel grid): the right fix for walls and ramps, and a bigger change
    (cost per NPC, a navigation grid that follows edits). Detours handle crates, pillars, a pool's
    edge and corridor corners; an enemy still can't find the ramp up a hill.
  - A field of view (no sight behind it): goes with stealth (crouching, noise), later.
- **Consequences:**
  - Enemies notice from across a clearing, follow a kited player across the map while they see
    them, lose them behind cover and come looking. More fights find the player; the default camp
    spawns (`spawnEntities`) moved out to 34 and 40 blocks so the starting area stays quiet.
  - The story fights' balance numbers are unchanged (the fights start in reach). In the story world
    every aggressive spawn marker is 94 blocks or more from the player spawn, so the start stays quiet.
  - One ray per fighting or searching NPC per tick within its chase range (and per calm aggressive
    one within its aggro range): a few dozen voxel steps each.
  - A save made mid-chase far from home (beyond the chase range) loads the NPC at home.
  - **Tested:** `pursuit()` in core_test, on a flat field 160 blocks across: seen from 20 blocks in
    the open (twice the old range), not through a wall nor past its aggro range in a minute;
    kited 60 blocks past its home at 6 blocks a second it chases all the way; outrun past its chase
    range it gives up, walks home and heals; behind a wall it chases on for the grace, then searches,
    reaches where it last saw the player, looks round, gives up after the search time and goes home;
    searching, it finds the player stepping back into view (without a shout), not one past its chase
    range, and a hit sends it back; a hit from out of sight, even one it reels from, tells it where
    the player is; seen every other tick it never turns to searching; it goes round a crate stack it
    can't see over, and through a slanted doorway a body just fits, to reach the player; a passive
    boar hit once chases 40 blocks from home. No rest near a searching NPC (`flasks()`); a hit on
    the way home brings it back, after `lost_target` only from in reach; a grace of 0 doesn't flap; a
    forced search starts where the NPC stands; a timed marker's searcher stays past its window
    (`pursuit()`). Mutation-checked: each of 28 rules
    broken alone fails a test. `npcAi()` and `fighters()` re-pinned
    for the chase range (giving up past it, height, Return, the pillar: a player up a pillar is still given up on
    though it walks round), the data table and validation (`chase_range` above `aggro_range`,
    fighters only; `leash_radius` unknown).

## ADR-056: Lasting deaths: nothing respawns, and quests need their people

- **Status:** accepted (2026-11-11); supersedes ADR-018's and ADR-023's respawning (`respawn_seconds`).
  The roadmap item's other half, a spawner for random populations out of view, is ADR-057's (next).
- **Context:**
  - Every NPC came back at its home, healed, 5 to 120 seconds after it died. Hakon the Red, the
    story's one named enemy, rose again after two minutes; the quest text said so ("from Hakon
    himself if he rises again").
  - Villagers couldn't be hit at all, so a quest giver could never die.
  - The roadmap: "The dead near the player stay dead (corpses and all, until they despawn far away
    and unseen). Friendly NPCs die for good too: a quest giver who dies takes their quest with them."
- **Decision** (the user's calls: villagers attackable and flee; nothing respawns, the dummy is
  immortal instead; corpses go far and unseen, keeping their loot until taken; Hakon dead for good):
  - **Nothing respawns.** `respawn_seconds` is gone from `creatures.json`; updateCombat no longer brings
    the dead back. The **training dummy** is `immortal` (a new creature flag, attackable kinds without
    loot only): hits, statuses and poison show their numbers, count as hits (XP, sounds, the stagger)
    and take no health, so it never topples.
  - **Villagers can be hit** (`attackable`), and **run from a hit**: a harmless kind that wanders goes
    to `Flee` (reason `damaged`, or `steadied` after reeling) for `kFleeSeconds`, away from the player
    and round what's in the way (`detour`, as a chase's), a new hit starting it over (even on its way
    home), then walks home and heals. It runs at its `chase_speed` (6 for the villager; the player
    runs 9). Like the dummy, it teaches at the training rate and gives no kill XP. Guards don't come to
    its aid yet. A villager is a **bystander** (harmless and walking about): it can't be locked onto,
    and a backstab or riposte never picks it, so a fight in the village doesn't pull the player's
    attack (or the lock after a kill) onto Torvald; hitting one takes aiming at it.
  - **Who died is recorded** (`Session::dead`, `DeadSlots`: spawn marker id to a bit per slot). Once a
    tick `buryDead` records every dead marker NPC, and `syncSpawns` never fills a recorded slot again,
    whether its marker's window opens, the marker is edited or the game loads; an edited marker keeps
    a corpse that still holds loot (under the marker as it is now) instead of clearing it. The player
    save keeps the record in a new section, `DEAD` (u32 count, per marker still in the world u32 id and
    u8 slot bits); `NPCS` still keeps a corpse while it lies. An older save's corpses say who died (they load dead and are recorded on the
    first tick), so old saves need nothing. The Entities tab's "Respawn NPCs at their markers" clears
    the record: a fresh start for every marker.
  - **Corpses go far and unseen:** `buryDead` removes a corpse with nothing on it once the player is
    `gAi.corpseGone` (64 blocks) from it and can't see it (`canSee` from the player's eyes, or its part
    of the world isn't loaded). One still holding loot stays until it's taken, so a quest item (Hakon's
    helm) can't vanish with it. NPCs not from a marker (the spawn panel's, F6) go the same way, unrecorded.
    Timed markers still clear an empty corpse out of their window after `kCorpseLinger`.
  - **Quests need their people:** a quest lists spawn marker names it can't do without (`needs`).
    When every NPC of one of them is dead (`npcDead`, from the record), the quest fails, started or not:
    a started one says so in a toast ("Quest failed: Torvald's Helm (Torvald is dead)"), one not yet
    started never starts. The journal lists both under Failed with the reason ("Torvald is dead. This
    can never be finished now." or "... started now"). `npc_dead` conditions read the record too, so
    they hold for good. `checkQuestMarkers` warns about a need that names no spawn marker. Torvald's
    Helm needs Torvald (Maren Holt only points the way: with her dead it starts from Torvald); its
    lines no longer say Hakon gets up again, and once it has failed Maren and Captain Brenna say so
    instead of pointing the way to Hakon.
  - The debug window: Entities has a "Lasting deaths" node (the corpse distance, how many corpses lie,
    every marker's dead slots) and, per corpse, whether it stays and why; Quests shows each quest's needs
    and who of them is dead.
- **Alternatives considered:**
  - Respawning only common enemies on a long timer: the user chose nothing, with ADR-057's spawner
    to fill the world back up (the procedural world makes up for what's lost).
  - A dummy that respawns: the user wants it never to fall.
  - Deriving a quest's needs from its `talked_to` triggers: Maren Holt is one, and the quest doesn't
    need her. An explicit list says what the author means.
  - Dropping a corpse's loot on the ground when it goes: there are no dropped items in the world yet;
    keeping the corpse is the same promise with nothing new.
- **Consequences:**
  - The story world empties as the player fights through it: three raiders, three wolves, two ghouls
    and a boar, once each. ADR-057's spawner (mainly for the endless world) is what brings life back.
  - The endless world's five starting NPCs die for good in that save too.
  - A villager can be killed by a stray swing or a bomb in the village, and its quest goes with it.
  - The helm itself can still be destroyed from the bags (Ctrl+right-click), which leaves the quest
    unfinishable without saying so; quest items that can't be destroyed would close that.
  - One canSee ray per empty corpse 64 or more blocks off per tick, until it goes.
  - A quest that failed through `fail_quest` and whose need dies later says the need's death in the
    journal (the reason isn't saved, it's read from the record).
  - **Tested:** `lastingDeaths()` in core_test: the dummy keeps its health through a blow, a bleed and
    poison while each shows its number, and is never killed; a villager runs from a hit (away from the
    player, round a wall), starts over on a new hit and on one while going home, runs once steady after
    reeling, then walks home and heals; buryDead keeps a corpse seen across open ground, a near one
    and one with loot, takes one far behind a wall or in an unloaded part, records marker NPCs and not
    stray ones; syncSpawns fills no dead slot, even for an edited marker, and an edited marker keeps a
    corpse holding loot; in play nothing comes back in 130 s, the save keeps the record (dropping removed
    markers), an older save's corpse is recorded again, and bad DEAD sections (marker 0, no slots, a
    marker twice, a size mismatch) reject the file; Torvald dying fails Torvald's Helm started (with the
    toast) or not (silently, and it never starts), Maren and Brenna then say so, a done quest stays done,
    `npc_dead` holds after the corpse goes, Maren's death doesn't fail it, Hakon's corpse keeps the helm;
    `needs` validation and the marker check; villagers can't be locked onto or backstabbed, the dummy
    can. Older tests re-pinned: the dummy as a damage meter reads the hit's number, the boar stays dead,
    the data table (`immortal`, the villager attackable at 6 b/s), validation (`respawn_seconds` unknown,
    `immortal`'s rules), the save round trip and timed corpses.
    Mutation-checked: 36 of 37 rules broken alone fail a test (the survivor, hitting a corpse, is
    equivalent: `hurt()` refuses a dead body, and an immortal one never dies).
