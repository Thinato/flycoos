# Voxel RPG Code Architecture: Architecture Choices, Data-Driven Design, Incremental Refactoring

Context checked in the repo (2026-11-03) so the inferences below are grounded:
- `src/` holds about 6.5k lines across 30 files, and `tests/core_test.cpp` holds about 2.2k lines (`git ls-files | xargs wc -l`).
- `src/entity.h` already has a hard-coded Type Object: `struct EntityType { name, level, Stats, attackable, wanders, talks, Model, Temper, aggroRange, wanderRadius, chaseSpeed, ItemId weapon, ItemId loot, lootChance, respawnSeconds }` with `typeOf(EntityKind)`. `src/item.h` has `struct ItemType` in the same style.
- `src/main.cpp` runs a **variable-dt** loop. It calls `updatePlayer(..., dt)`, `updateEntities(..., dt)` and `updateCombat(..., dt, rng, ...)` with raw frame dt. There is no accumulator.
- Prefabs are already data: `prefabs/*.txt` (18 custom-text files). `nlohmann/json` is already a dependency.

---

## Q1. When is a full ECS worth it compared with arrays of structs plus free functions, at ~5–20k lines, one developer plus AI agents, and a few hundred entities?

### Takeaway
At this scale, the sources agree that ECS pays off mainly for (a) large, uniform object counts iterated every frame, and (b) composition/reuse across many entity kinds. A few hundred entities need neither. The most useful advice, from Catherine West, Nystrom and the ECS FAQ itself, is to adopt the *data-first thinking* without the framework: plain structs in vectors, index or generational-handle references, and systems as free functions running in a fixed order. Keep EnTT as an option if composition pressure shows up later.

### Cited Findings
- The ECS FAQ (Sander Mertens, author of flecs) defines ECS as "a design approach which promotes code reusability by separating data from behavior." Its stated reasons to use it are that "ECS can typically support larger numbers of game objects", plus reusability, easier extension and dynamic composition. It also says separating data from behavior makes multithreading easier. — [ECS FAQ](https://github.com/SanderMertens/ecs-faq)
- The same FAQ lists costs. There is a learning curve ("Some aspects of ECS design go against intuition"). Integrating with non-ECS engine types needs "additional glue-code to bridge between the native engine types and the ECS". Archetype storage can "create a lot of small tables, which can degrade performance". For a hand-rolled ECS, "you should fully expect that it will not outperform established implementations". The FAQ has **no** explicit "when not to use" guidance and nothing specific to small games. — [ECS FAQ](https://github.com/SanderMertens/ecs-faq)
- Storage designs, per the FAQ: "Archetype implementations are fast to query and iterate" (flecs), and "Sparse set implementations allow for fast add/remove operations" (EnTT). C++ options listed: Flecs (C/C++17, MIT), EnTT (C++20, MIT), Gaia ECS, Unreal Mass, and EntityX (inactive). — [ECS FAQ](https://github.com/SanderMertens/ecs-faq)
- EnTT is header-only with no dependencies and needs at least C++20. Its ECS has a "pay for what you use" policy. The API is `registry.create()`, `registry.emplace<T>()` and `registry.view<...>().each(...)`. It also ships signals/delegates, a dispatcher, a resource cache and loaders, and reflection. It integrates via CMake `FetchContent` (`EnTT::EnTT`). Users listed include Mojang's Minecraft (edition not stated). — [EnTT README](https://github.com/skypjack/entt)
- Catherine West's RustConf 2018 talk ("Using Rust for game development") describes a progression. First, a mostly global game state with plain public structs and procedural "systems" (ordinary functions) run in sequence, with pointers replaced by indices into arrays. Next, entities as a collection of optional components. Then structs-of-arrays. Finally, generational indices to stop reused indices from pointing at the wrong entity. She says of the plain-struct stage: "if I were doing a game jam game, this might be how I would write my game!". She also holds that an honest big procedural function beats a tangled web of objects. — [kyren.github.io RustConf talk](https://kyren.github.io/2018/09/14/rustconf-talk.html)
- West on OOP game objects: "The problem with OO is that everything happens somewhere else". She also says: "Stop conflating the data representation of your game with the systems that operate on it!" and "You can do a LOT with indexes into Vecs." — [kyren.github.io](https://kyren.github.io/2018/09/14/rustconf-talk.html)
- Overwatch (Tim Ford, GDC 2017) is the canonical large-scale ECS case. One write-up reports about 46 client-side systems and 103 component types, with only three systems handling gameplay netcode. Ford's aim was a "pit of success" architecture. Sean Middleditch's retrospective says the ECS portion was a small part of the talk, and that most of it covered state-based gameplay code plus prediction and server correction. — [GDC Vault listing](https://gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and); [Edgegap deep dive](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback); [Sean Middleditch retrospective](https://seanmiddleditch.github.io/my-gdc-17-talk-retrospective)
- A secondary guide (browser games, lower authority) argues that a small fixed cast of entities is a poor fit for ECS. In its view the indirection adds boilerplate without a performance payoff, and the win comes from iterating large uniform data sets every frame. — [simplified.media ECS guide](https://simplified.media/guides/ecs-browser-games)
- Wikipedia notes that ECS and data-oriented design are distinct concepts. Plain arrays of structs can be data-oriented without an ECS. — [Wikipedia: Entity component system](https://en.wikipedia.org/wiki/Entity_component_system)
- Bob Nystrom gave a talk titled "Is There More to Game Architecture than ECS?" (Roguelike Celebration 2018). A conference recap says a Desktop Dungeons developer, speaking after it, reported better modularization from his own approach than from pure ECS. **I could not reach a primary transcript or video page.** — [Steemit recap](https://steemit.com/roguelikecel/@markgritter/roguelike-celebration-bob-nystrom-on-architecture)

### Inferences
- flycoos already has an **entity + type object** design: `Entity` holds per-instance state and `EntityType` holds per-kind constants. That is the "plain structs + free functions" stage West describes. With a few hundred NPCs, cache layout is irrelevant. The pressure that justifies ECS is *composition*, meaning many kinds that each need a different subset of behaviors. The current `bool attackable/wanders/talks` flags are an early sign of it. Until that list grows to roughly 10+ optional behaviors with per-instance data, flags plus optional sub-structs (`std::optional<Combat>`, `std::optional<Dialogue>`) inside one `Entity` struct do the job with zero dependencies.
- Signals that it is time to add EnTT (header-only, C++20, so it fits the C++23 + FetchContent stack): heterogeneous things (projectiles, dropped items, doors, campfires) would each need their own vector and update loop; queries like "everything with Health and a Position near X" get rewritten in several places; or save/load needs generic component serialization. Migrating then is mechanical if systems are already free functions over plain data.
- Flecs (C99 core with C++ API, archetype storage, built-in query language, REST explorer) adds more machinery than this scale needs. EnTT is the lighter fit if an ECS is ever adopted. This inference comes from the FAQ's description of the two storage models and from EnTT's header-only packaging.
- The cheapest important borrow from ECS thinking is **stable handles**. Use generational indices `{uint32 index, uint32 gen}` for any cross-entity reference (aggro target, quest giver, owner of a dropped item) instead of raw pointers or vector positions. West presents this as the key fix, and it costs about 30 lines.

### Gaps
- I found no quantitative study (lines of code, bug rate, iteration speed) comparing ECS with plain structs for small single-player RPGs. Evidence is opinion, talks and anecdotes.
- No primary source for Nystrom's 2018 talk content was retrieved.
- AI-agent-specific evidence (whether ECS indirection helps or hurts LLM code edits) was not found. Plausibly, explicit structs and free functions are easier for agents to grep and reason about than registry-driven dispatch, but that is unsourced.

---

## Q2. Which module and layer boundaries keep simulation testable and independent from rendering? Game loop and fixed timestep.

### Takeaway
Separate a pure **simulation core** (world state, entities, combat, inventory, AI; no SDL, GPU or ImGui includes; deterministic given seed and inputs) from **presentation** (renderer, mesher, HUD/ImGui) and **tools** (editor, MCP bridge). Advance the simulation on a fixed dt with an accumulator, and render by interpolating between the previous and current sim state. The test executable then links only the core.

### Cited Findings
- Fix Your Timestep (Glenn Fiedler): "the renderer **produces time** and the simulation **consumes it** in discrete dt sized steps." Frame time accumulates, and physics always advances by the same dt, so results don't depend on framerate. — [Gaffer On Games: Fix Your Timestep!](https://gafferongames.com/post/fix_your_timestep/)
- Interpolation: "We can use this remainder value to get a blending factor between the previous and current physics state". Render `lerp(previous, current, alpha)` with `alpha = accumulator / dt`, and use slerp for rotations. — [Gaffer On Games](https://gafferongames.com/post/fix_your_timestep/)
- Spiral of death: when stepping costs more real time than it simulates, "being behind causes your update to simulate more steps". The fix is to clamp the frame time fed into the accumulator. Code shape:
  ```
  accumulator += min(frameTime, maxFrameTime)
  while accumulator >= dt: previous = current; integrate(current, dt); accumulator -= dt
  alpha = accumulator / dt; render(lerp(previous, current, alpha))
  ```
  — [Gaffer On Games](https://gafferongames.com/post/fix_your_timestep/)
- Veloren (open-source Rust voxel RPG) splits into crates: `common` (behaviour and definitions shared by client and server), `client` ("independent of any frontend", supporting chat-only, spectator and character modes), `voxygen` (the 3D graphical client built on `client`), `server`, `world` (worldgen and simulation "usable without a client or server") and `network`. Stated reasons are faster parallel compilation and letting consumers depend only on what they need. — [Veloren Book: Codebase structure](https://book.veloren.net/contributors/developers/codebase-structure.html)
- Overwatch's architecture kept gameplay netcode in three of about 46 systems, which shows that narrowly scoped systems with explicit boundaries keep cross-cutting concerns contained. — [Edgegap](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback)
- West's "UR-architecture" is a single game-state struct plus systems as ordinary functions called in a fixed order. That makes the state easy to construct in isolation, for example in a test. — [kyren.github.io](https://kyren.github.io/2018/09/14/rustconf-talk.html)

### Inferences
- A suggested layering for flycoos, mapped to the existing files. Enforce it with CMake targets, not conventions:
  - `flycoos_core` (static lib, no SDL, GPU or ImGui): `world`, `entity`, `item`, `stats`, `player` (movement and physics part), `prefab` (data part), `edit` (world mutations, undo), and a new `defs` (data loading, Q3). `core_test` links only this.
  - `flycoos_render`: `renderer`, `mesher`, shaders. It reads core state and never mutates it.
  - `flycoos_ui`: `hud`, `inventory_ui`, `debug_ui`, `editor_ui`. These read state and emit **commands** (for example "equip slot 3" or "place prefab X at P"). They don't mutate state directly.
  - `flycoos_tools`: `server` (MCP bridge) and `commands`, which already exist and go through the same command path as the UI.
  - `main.cpp`: SDL event pump, an input-to-intent mapping, the fixed-step loop and the render call.
  Having the CMake target graph forbid `core -> render/ui` includes gives a compile-time check, which is the C++ analogue of Veloren's crate split.
- Fixed timestep for flycoos: `main.cpp` passes frame `dt` straight into `updatePlayer`, `updateEntities` and `updateCombat`. A 60 Hz (or 30 Hz) fixed tick with an accumulator and frame-time clamp would make combat, AI timers and respawns frame-rate independent. It would also make `core_test` able to replay exact tick sequences. Interpolate only what is drawn: entity and player positions, yaw and camera target. Keep `prevPos` alongside `pos` in the body struct. Leave camera smoothing, `timeOfDay` visuals and the editor brush on frame dt.
- Determinism inside the core: one seeded RNG owned by the sim (already passed as `rng` to `updateCombat`), no wall-clock reads, and input sampled once per tick into an `Input` struct. That makes "N ticks with input script X gives state Y" tests trivial.

### Gaps
- No source was retrieved for Nystrom's Game Loop and Update Method chapters (gameprogrammingpatterns.com/game-loop.html, /update-method.html). They are believed to cover the same fixed-update and variable-render ground as Fiedler but were not fetched in this session.
- No fetched source specifically covers C++ CMake target layering for game engines. The layering above is inferred.

---

## Q3. How are entity, item and ability definitions laid out in data files, and how are cross-references and validation done?

### Takeaway
Every system surveyed uses the same core: **a definition record per "kind" (Type Object), keyed by a stable human-readable string ID, loaded into a registry at startup, with instances referencing definitions by ID or handle.** They differ in inheritance (CDDA `copy-from`/`abstract`; Nystrom's parent breeds), in override and layering (Bethesda plugins override records by FormID; Luanti `modname:item` namespaces), and in where validation happens (a load-time finalize pass is the norm, and JSON Schema is optional). Stable string IDs beat numeric IDs for anything a human or another file references. Bethesda's own history shows numeric FormIDs shift with load order, while EditorIDs stay stable.

### Cited Findings
**Type Object (Nystrom)**
- Intent: "Allow the flexible creation of new 'classes' by creating a single class". Each `Monster` points to a `Breed` holding shared data such as starting health and attack string. Breeds live in a JSON file so designers can tune without recompiling. — [Game Programming Patterns: Type Object](https://gameprogrammingpatterns.com/type-object.html)
- Inheritance via a `parent` breed ("Troll Archer" and "Troll Wizard" inherit from "Troll"). **Copy-down** resolves inherited values once at load, which gives fast lookups but no propagation of later parent changes. **Dynamic delegation** walks the chain on every lookup. Copy-down is recommended when attributes don't change at runtime. — [GPP: Type Object](https://gameprogrammingpatterns.com/type-object.html)
- Caveats: you must make sure every object has a valid type, because the compiler no longer checks. Type objects make "it very easy to use type objects to define type-specific _data_, but hard to define type-specific _behavior_". Workarounds are picking from predefined behaviors or using an interpreter. Single inheritance is the recommended balance. — [GPP: Type Object](https://gameprogrammingpatterns.com/type-object.html)

**Cataclysm: DDA (JSON)**
- `copy-from` names a parent ID, and omitted fields take the parent's values. `abstract` defines an inheritable ID that is "discarded after JSON loading completes", so it may omit mandatory fields. `relative` adds offsets to numeric fields, `proportional` multiplies (for example `1.5`), and `extend` / `delete` add or remove container entries such as `flags`. `copy-from` must reference an entry of the same `type`. There is **no universal default** for list fields: `monstergroup` extends by default and needs `"override": true` to replace. Inheritance support depends on whether the type's C++ loader uses `generic_factory`. — [CDDA JSON_INHERITANCE.md](https://github.com/CleverRaven/Cataclysm-DDA/blob/master/doc/JSON/JSON_INHERITANCE.md)
- CDDA advises using `copy-from` sparingly, since chained inheritance hurts readability. It fits near-identical variants or groups that always share properties, with one or two levels of abstracts. For minor variations it recommends JSON "variants". — [CDDA JSON_INHERITANCE.md](https://github.com/CleverRaven/Cataclysm-DDA/blob/master/doc/JSON/JSON_INHERITANCE.md)
- Example shape (from the doc's patterns):
  ```json
  { "abstract": "base_rifle_round", "type": "ITEM", "volume": "10 ml", "flags": ["IRREPLACEABLE_CONSUMABLE"] },
  { "id": "762_match", "copy-from": "base_rifle_round", "type": "ITEM",
    "relative": { "damage": { "damage_type": "bullet", "amount": 4 } },
    "proportional": { "price": 1.5 }, "extend": { "flags": ["INCENDIARY"] } }
  ```
  — [CDDA JSON_INHERITANCE.md](https://github.com/CleverRaven/Cataclysm-DDA/blob/master/doc/JSON/JSON_INHERITANCE.md)

**Bethesda Creation Engine (records/forms, plugins)**
- A plugin is "a collection of records", each split into fields (subrecords). "Records generally correspond to objects (e.g., a creature, a game setting, a dialog entry)". A record header has a 4-character type, size, flags, form identifier, version control info and internal version. GRUPs collect records of one type "to make it easier to skip over records that the reading program is not interested in". Top groups hold records of one type. — [UESP: Skyrim Mod File Format](https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format)
- ESMs are "pinned to the top of the load order, taking precedence over all non-flagged ESP and ESL plugin files". ESL plugins hold up to 4096 records (since v1.6.1130). — [UESP: Skyrim Mod File Format](https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format)
- FormIDs carry a ModIndex set by load order, so the master is always 00 and the 10th plugin gets 0x09. They are therefore unstable across load orders, and "In scripting, the best way to refer to an object is to use the EditorID". A hard-coded FormID breaks when the plugin moves in load order. These are Oblivion-era pages. — [CS UESP: FormID](https://cs.uesp.net/wiki/FormID); [UESP Oblivion Mod Merging](https://app.uesp.net/wiki/Oblivion_Mod:Mod_Merging)

**Luanti (formerly Minetest)**
- Content is registered from Lua with definition tables: `core.register_node`, `core.register_craftitem`, `core.register_tool` and `core.register_entity`. Item fields include `description`, `groups` (name to integer rating), `inventory_image`, `stack_max`, `tool_capabilities` (`full_punch_interval`, `groupcaps`, `damage_groups`) and callbacks (`on_use`, `on_place`, `on_drop`). Custom fields should use a `_` prefix "to avoid collisions with future engine fields". `drop` defaults to the node itself. `mod_origin` records which mod registered a node. Names follow a `modname:<name>` convention. — [Luanti API: Definition tables](https://api.luanti.org/definition-tables/)
- Luanti's approach is data *plus* behavior in one definition: callbacks live next to stats. That is Nystrom's "interpreter" route to per-type behavior, using an embedded scripting language. — [Luanti API](https://api.luanti.org/definition-tables/); [GPP: Type Object](https://gameprogrammingpatterns.com/type-object.html)

**Godot Resources / Unity ScriptableObjects**
- Godot: nodes provide behavior and Resources are "data containers". A resource loaded from disk is loaded once, and repeated loads "return the same copy every time". Custom Resource classes (the doc's `BotStats` example with exported `health`) are saved as version-control-friendly `.tres` text, can nest sub-resources recursively, and export as binary `.res`. — [Godot docs: Resources](https://docs.godotengine.org/en/stable/tutorials/scripting/resources.html)
- Unity: ScriptableObjects "exist in the project as assets, independent of GameObjects". Prefabs reference one shared asset instead of duplicating data, so "there is one copy of the data in memory". In a standalone player "you can only read saved data from the ScriptableObject assets", meaning definitions are read-only at runtime. — [Unity Manual: ScriptableObject](https://docs.unity.com/en-us/engine/current/manual/scripting/get-started/fundamental-unity-types/class-scriptable-object)

**Validation tooling for the flycoos stack**
- `pboettch/json-schema-validator` validates JSON Schema draft-7 on top of nlohmann::json (>= 3.8.0), gives human-readable errors, and can fill defaults as a JSON patch. It is **not header-only** (it builds a static library) and can be pulled via FetchContent. Limits: defaults are filled one level per pass, and validator construction is not thread-safe. The README calls it "work in progress". — [json-schema-validator](https://github.com/pboettch/json-schema-validator)

### Inferences
- Recommended flycoos layout. JSON fits because nlohmann is already a dependency, and TOML or YAML would add a library for no gain:
  ```
  data/items.json      [{ "id": "iron_sword", "name": "Iron Sword", "slot": "main_hand", "damage": 6, ... }]
  data/creatures.json  [{ "id": "boar", "level": 2, "temper": "passive", "stats": {...}, "weapon": null,
                          "loot": "boar_hide", "loot_chance": 0.5, "respawn_seconds": 120 },
                        { "id": "bandit_archer", "copy_from": "bandit", "weapon": "short_bow" }]
  ```
  One file per definition type, a JSON array of objects, and a required `"id"` in `snake_case` that is stable forever (the EditorID lesson from Bethesda). Optional `copy_from` provides single-parent, copy-down inheritance as recommended by both Nystrom and CDDA. Resolve it once at load and skip `relative`/`proportional`/`extend` until a real need appears.
- Cross-references: in data, references are string IDs (`"weapon": "iron_sword"`). At load, a **two-pass loader** first parses every file into `id -> json`, then resolves `copy_from` and converts each string reference into a typed handle (`ItemId`, a dense `uint16`). Any unknown ID is a hard error naming file, record and field. Runtime code never sees strings. Save files store string IDs, not dense indices, so adding a new item doesn't corrupt saves. This is the FormID-vs-EditorID lesson.
- Validation without a new dependency: the two-pass loader *is* the validator for references. For field types and ranges, nlohmann's `j.at("field").get<T>()` already throws on missing or mistyped fields. A `from_json` per struct plus a few range checks (for example `0 <= loot_chance <= 1`) covers what matters. The schema-validator library adds a non-header-only dependency for what a few checks already cover. Skip it unless the data grows large or non-programmers start authoring it.
- `core_test` gets one test that loads every shipped data file and asserts zero errors. That is the CI-less equivalent of schema validation and catches broken references before the game runs.
- Behavior stays in C++. Data selects among predefined behaviors via enums or strings (`"temper": "aggressive"`, `"ai": "wander"`), which is the first workaround Nystrom recommends. Embedding Lua the way Luanti does is a large step to defer until quests or abilities need real scripting.
- Hot reload (low cost given the stack): an ImGui debug-panel button (or an `mtime` poll in debug builds) re-runs the loader into a fresh registry, swaps it in only if loading succeeded, and keeps the old one on error. Live entities hold `EntityKind`/`ItemId` handles keyed by ID, so they pick up new stats immediately. Only re-run the loader between ticks.

### Gaps
- Veloren's asset format (RON files under `assets/`, referenced by dotted asset paths such as `common.items...`, with hot reloading through its `assets_manager` crate) is believed true, but the relevant Veloren Book page returned 404 and **was not verified** in this session.
- OpenMW's handling of ESM records (its ESM loader, and its newer Lua/YAML data approach) was not researched due to the tool-call budget.
- The current Skyrim-era FormID page (en.uesp.net Skyrim_Mod:Form_ID) returned 404. Override semantics ("the last loaded plugin's version of a record wins") come from general knowledge and the Oblivion merging article, not a Skyrim-specific primary page.
- The CDDA doc fetched does not describe how dangling references are reported at load time.

---

## Q4. What incremental refactor sequence fits a prototype with hard-coded C++ tables?

### Takeaway
Use the strangler fig pattern at the system level. Find a seam (the `typeOf(kind)` / item-type lookup functions are natural ones), make the new data-backed path produce **identical** values to the old table, prove it with a test, switch callers over, then delete the old table. Do one definition type per commit and start with the lowest-risk one. Avoid any big-bang "rewrite into ECS" step.

### Cited Findings
- Strangler fig (Fowler, citing Cartwright, Horn and Lewis): build new functionality alongside the legacy system and move behavior over piece by piece. The activities are defining outcomes, **finding seams** where components can be separated and replaced independently, delivering in parts starting with small low-risk components, and sustaining incremental delivery. — [Martin Fowler: Strangler Fig Application](https://martinfowler.com/bliki/StranglerFigApplication.html)
- On rewrites, Fowler says the simple-sounding plan has "go[ne] down in flames most of the time", partly because existing behavior is poorly understood. Transitional architecture is code that will "go away once the modernization is complete". The pattern's reduced risk and earlier value "outweigh its costs". — [Martin Fowler](https://martinfowler.com/bliki/StranglerFigApplication.html)
- CDDA notes that JSON inheritance support depends on whether a type's C++ loader uses `generic_factory`. In other words, even a mature data-driven game migrated type by type, and some types still differ. — [CDDA JSON_INHERITANCE.md](https://github.com/CleverRaven/Cataclysm-DDA/blob/master/doc/JSON/JSON_INHERITANCE.md)

### Inferences
Recommended sequence for flycoos. Each step is one commit with `core_test` green.
1. **Characterization test first.** Add a test that iterates every `EntityKind` and `ItemId`, then asserts each field of `typeOf(k)` and the item type against snapshot values copied from today's tables. This pins current behavior and is the strangler fig's safety net.
2. **Split the core library target** (Q2), without behavior change. Move the sim files into `flycoos_core` and make `core_test` link only that. Fix any `#include` that drags in SDL, GPU or ImGui. This is the seam that makes everything after it testable.
3. **Fixed timestep.** Add the accumulator loop and `prevPos` interpolation in `main.cpp`. Then add a test that N ticks of a scripted input give a deterministic position, HP and RNG state.
4. **Items to data first.** Items are leaves: they reference nothing, and creatures reference them. Write `data/items.json`, a loader into `std::vector<ItemType>` plus an `unordered_map<std::string, ItemId>`, and make the characterization test compare loader output against the old constexpr table. Once green, delete the table. Keep `ItemId` as a dense handle so no call sites change.
5. **Creatures to data**, the same way. String references to item IDs are resolved in the second pass, and `copy_from` is added only if two creature kinds actually share most fields (bandit and bandit_archer, say).
6. **Load-all test, plus a debug-panel "reload defs" button** (hot reload).
7. **Next candidates by rising risk:** loot tables, NPC dialogue phrases, and abilities/attacks. Abilities are where behavior pressure appears. Keep them as enum-selected C++ behaviors with data parameters, as Nystrom advises.
8. **Re-check the ECS question only when** the `Entity` struct accumulates many optional behaviors, or new non-NPC entity kinds (projectiles, doors, dropped items) appear. If so, EnTT via FetchContent replaces the entity vectors system by system. That is a strangler step again, because free-function systems port one at a time.
- An AI-agent-specific inference: each step above fits in one agent task with a clear pass/fail test, the characterization test or load-all test. That suits a one-developer-plus-agents workflow better than broad architectural rewrites.
- Keep EntityKind/ItemId as enums or dense ints in C++ during the transition, with the data file as the source of their values. Once every caller uses handles from the registry, the enum can go. That removal is the last strangler step.

### Gaps
- I found no published postmortem from an indie voxel RPG describing its specific table-to-data migration order. The sequence above is synthesized from Fowler's general pattern, Nystrom and CDDA practice, and the flycoos code as it stands.
- Cube World's internal architecture has no public primary source (closed source, no technical postmortem found), so nothing about it is cited here.
