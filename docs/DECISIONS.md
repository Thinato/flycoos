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
