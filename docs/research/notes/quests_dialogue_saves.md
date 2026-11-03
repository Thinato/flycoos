# Quests, quest journals, dialogue conditions, world-state flags, inter-system events and save games for flycoos

Scope: how RPGs implement data-driven quests and journals, condition-gated dialogue, world-state flags, decoupled event flow between systems, and single-player save games. Recommendations target flycoos (C++23 voxel RPG). It has an authored, editable world file `.flyw` (RLE voxel chunks plus tagged sections: INST prefab instances, MARK markers with regions/POIs/spawns, LINE dialogue lines per spawn marker). NPCs cycle fixed lines, and there are no quests or player saves yet. The roadmap item is "Quests and a quest journal: data-driven objectives and flags (e.g. 'bring Hakon's helm to Torvald'), dialogue that reacts to quest state" (docs/ROADMAP.md).

Source-access note: the Creation Kit wikis (ck.uesp.net, skyrimck.uesp.net) and geckwiki.com returned HTTP 403 to the fetch tool. So did the UESP Morrowind "Creating dialogue" page, and witcher.fandom.com returned 402. For those pages, the CK facts below come from search-result summaries of those pages, not from full-page reads. They are flagged where this matters. UESP's main wiki (en.uesp.net) was readable: it gave the save format, change flags and console pages.

---

## 1. What is a minimal but extensible quest data model, and how are objectives tracked from gameplay events?

### Takeaway
Bethesda's model is the proven minimal core. A quest is a numbered list of stages; each stage may write a journal entry and turn objectives on or off. Objectives are a separate list with display text and an optional map target. "Aliases" bind quest roles to concrete world objects. Progress comes from gameplay events (pickups, deaths, dialogue choices, entering triggers) setting stages. For flycoos: quests as data (stages + objectives + triggers keyed to existing IDs: marker names, entity kinds, item IDs) driven by a small set of gameplay events. Persist only "current stage + stages done + objective states + counters" per quest in the player save.

### Cited Findings
- Creation Kit quest work is organized around "setting up stages, updating the quest log, putting relevant elements into aliases", and "displaying, completing, and failing objectives" is a separate concern from stages. — [Beyond Skyrim Arcane University: Quest implementation](https://wiki.beyondskyrim.org/wiki/Quest_Implementation)
- The typical progression triggers in Skyrim modding are picked-up quest items, target deaths, dialogue choices, book reads, and hand-placed triggers/activators. "Basic counters" react to N targets dying or N activators being activated. — [Beyond Skyrim Arcane University: Quest implementation](https://wiki.beyondskyrim.org/wiki/Quest_Implementation)
- In the CK, objectives are set up in their own tab, separate from stages. Stage code (fragments) updates and displays objectives. Aliases create the map markers that guide the player. Alias names can be slotted into objective display text. — search summary of [Bethesda Tutorial Quest Objectives](https://skyrimck.uesp.net/wiki/Bethesda_Tutorial_Quest_Objectives), [Bethesda Tutorial Radiant Quests](https://ck.uesp.net/wiki/Bethesda_Tutorial_Radiant_Quests), and [FO4 Simple Fetch Quest Tutorial ch. 9](https://ck.uesp.net/wiki/Fallout_4_Simple_Fetch_Quest_Tutorial_Chapter_9:_Quest_Startup) (full pages 403'd)
- If aliases fail to fill, "stages won't set, actors won't speak their dialogue, fragments don't run and objectives don't display". Aliases are a hard dependency of the whole quest. — search summary of [Fallout CK wiki: Quest](https://falloutck.uesp.net/wiki/Quest)
- Skyrim's console exposes this data model directly. `setstage <quest> <stage>` sets "the current stage of the given quest". `getstage` shows "the current active quest stage". `resetquest` "sets all of the quest's stages to 0". `GetObjectiveDisplayed` / `GetObjectiveCompleted` return 0/1 per objective. `sqo` "itemizes quest objectives and their states". `sqt` lists "all active quest IDs and their targets". `movetoqt` "move to quest target… does not advance the quest". `sqv <quest>` shows quest variables. `completequest` "may not be recognized by related NPCs", meaning skipping stages bypasses the stage-side effects other systems rely on. — [UESP Skyrim:Console](https://en.uesp.net/wiki/Skyrim:Console)
- Skyrim persists quest runtime state in the save as flag-tagged change records for each QUST form: `CHANGE_QUEST_FLAGS` (0x2), `CHANGE_QUEST_SCRIPT_DELAY` (0x4), `CHANGE_QUEST_ALREADY_RUN` (0x04000000), `CHANGE_QUEST_INSTANCES`, `CHANGE_QUEST_RUNDATA`, `CHANGE_QUEST_OBJECTIVES` (0x20000000), `CHANGE_QUEST_SCRIPT` (0x40000000), `CHANGE_QUEST_STAGES` (0x80000000). So the saved quest state is just stages, objectives, flags, "already run", and script variables. — [UESP Skyrim Mod:ChangeFlags](https://en.uesp.net/wiki/Skyrim_Mod:ChangeFlags)
- Morrowind (the older, simpler Bethesda model) uses a "journal" per quest that is just an ID plus an integer index. The Journal tab entries are what the player sees in the journal. Dialogue filters compare against the journal index (e.g. one line filtered on `Journal < 10`, a later one on `Journal = 10`), and a dialogue line's Results box advances the journal. — search summary of [UESP Morrowind Mod: Creating dialogue in Morrowind](https://content1.uesp.net/wiki/Morrowind_Mod:Creating_dialogue_in_Morrowind) (full page 403'd)
- Journal UX lesson from CD Projekt RED (Witcher 3 / Cyberpunk 2077), GDC 2023: "make consequences visible to the player", since investing in something not telegraphed well is mostly not worth it. — search summary of [GDC Vault: 10 Key Quest Design Lessons](https://gdcvault.com/play/1028897/10-Key-Quest-Design-Lessons) and [Game Developer write-up](https://www.gamedeveloper.com/marketing/10-key-takeaways-from-the-quest-design-of-cyberpunk-2077-and-the-witcher-3)

### Inferences
- **Minimal model for flycoos** (Bethesda-shaped, Morrowind-sized):
  - `Quest { id, title, stages[], objectives[] }`.
  - `Stage { index, journal_text?, on_enter: [actions], complete|fail flag }`.
  - `Objective { index, text, target: marker-name?, counter_goal? }`.
  - `Trigger { event kind + filter -> set stage N }`, where the filter is one of: kill entity-kind X (optionally from spawn marker M), talk to spawn marker M, have item Z ×n, enter region R. Stage indices with gaps (10, 20, 30) leave room for inserting steps later. This is common CK practice, not verified on a fetched page.
- **Use existing IDs as alias equivalents.** Flycoos markers already have stable names (spawn, POI and region markers), so a quest can reference "the NPCs of spawn marker `Torvald`" and "region `Barrow`" instead of a full alias system. That fits the Act 1 story data. Skip dynamic/radiant alias filling (YAGNI) until there are procedurally targeted quests.
- **Actions** a stage can run (small closed set): show/complete/fail objective, give/take item, set flag, start quest, and set the spawn marker's line set (see §2). This closed set replaces Papyrus fragments.
- **Tracking:** the quest system subscribes to five or six event types (EntityKilled{kind, spawnMarker}, ItemCountChanged{item, count}, TalkedTo{spawnMarker, topic}, EnteredRegion{region}, StageSet). On each event it checks only the triggers of active quests. With tens of quests, a linear scan is fine.
- **Journal UI:** each active quest shows its title, the journal entries of the stages reached (newest first), and the currently displayed objectives (completed ones struck through). Keep Active/Completed/Failed groups and one "tracked" quest whose objective target is drawn on the HUD/map. flycoos already has map drawing for markers.
- **"Have item Z" pitfall:** "have" is a state, not an event. Re-check it on ItemCountChanged and also when the stage is entered, since the player may already hold the item.

### Gaps
- Exact CK field semantics were not verifiable because the CK wiki pages 403'd. These include the stage index range, the "Start Game Enabled" and "Run Once" flags, objective target alias types, and the alias fill types (specific reference / unique actor / find matching / create reference). Treat any recollection of them as unverified.
- No primary source was readable for Witcher 3 journal UI mechanics: quest categories, suggested level, tracked quest, and descriptions that are rewritten as the quest progresses. witcher.fandom returned 402, and no REDkit quest-graph docs turned up.
- No postmortem specifically on implementing a quest system in C++ was found within the search budget.

---

## 2. How are dialogue lines gated by conditions and quest stages, and is Ink or Yarn worth integrating vs a simple custom format?

### Takeaway
Bethesda games gate each dialogue response with a flat list of conditions. Each condition is a function, a subject, a comparison and a value, joined by AND/OR, and OR binds tighter than AND. Responses are tried top-down and the first one whose conditions all pass is used. That is cheap to build and maps directly onto flycoos' per-spawn-marker LINE section. Ink (via the MIT-licensed C++ runtime inkcpp) and Yarn Spinner (official runtimes C#, plus Unreal-only C++) are powerful but bring their own variable store, compiler toolchain and save state. For short, quest-gated NPC lines, a custom "line + conditions + actions" format is the better fit. Reconsider Ink only if branching conversations with player choices become central.

### Cited Findings
- CK condition semantics: conditions join with AND by default. "Consecutive ORs act as one block during evaluation and take precedence over AND", so `A AND B OR C AND D` = `A AND (B OR C) AND D`. In dialogue, "Run On: Subject" is the speaker and "Target" is the actor being addressed. To check the player you use Target. — search summary of [CK wiki Category:Conditions](https://ck.uesp.net/wiki/Category:Conditions), [GECK Quest and Dialogue Tutorial](https://geckwiki.com/index.php/Quest_and_Dialogue_Tutorial), and an [NV dialogue-conditions audit PR](https://github.com/LootJunkieAzza/NVForever/pull/675) (CK/GECK pages 403'd)
- Gating a greeting on quest progress is done with a GetStage condition and a comparison operator (e.g. `GetStage MyQuest < 20`). — search summary of [Nexus forum: Hello topic](https://forums.nexusmods.com/topic/9095593-le-make-a-hello-topic-run-only-upon-dialog-start-with-npc/) and the GECK tutorial above
- Morrowind dialogue selection: dialogue "is read from the very top line to the bottom, line by line, until the very first line that meets all of the speaker condition filters". A common bug is an unconditioned line placed above a more specific one, which shadows it forever. Function/variable filters (e.g. Journal) support `>, <, =, !=, >=, <=`. — search summary of [UESP Morrowind Mod: Creating dialogue](https://content1.uesp.net/wiki/Morrowind_Mod:Creating_dialogue_in_Morrowind) and [UESP Dialogue Notes](https://en.m.uesp.net/wiki/Morrowind_Mod:Dialogue_Notes)
- Morrowind's editor does not evaluate some conditions (local variables, journal conditions), so the editor's filtered view lists more lines than the player will see in game. — same search summary
- Skyrim keeps global variables as a first-class saved table ("Global Variables" is global data table type 3 in the save) alongside quest change forms. — [UESP Skyrim Mod:Save File Format](https://en.uesp.net/wiki/Skyrim_Mod:Save_File_Format)
- **Ink:** `.ink` compiles to a single JSON file. The runtime saves and restores with `story.state.ToJson()` / `LoadJson()`. Game functions are bound with `EXTERNAL` + `BindExternalFunction` (int/float/bool/string). Game code reads and writes ink globals via `variablesState["name"]`, and `ObserveVariable` notifies on change. — [inkle: Running your ink](https://github.com/inkle/ink/blob/master/Documentation/RunningYourInk.md)
- **inkcpp** is a C++ Ink runtime plus a JSON-to-binary compiler under the MIT license. It claims "all features of ink 1.1 are supported, and checked with ink-proof", and its goals include "no heap allocations during execution" and "no external dependencies" (the compiler uses nlohmann JSON). It offers snapshots (globals plus runners), C++ function binding and observers. Compiling `.ink` sources needs Inkle's `inklecate` on PATH. The repo had about 121 stars and 7 open issues at the time of reading, with no explicit stability label. — [JBenda/inkcpp](https://github.com/JBenda/inkcpp)
- An inkcpp snapshot "contains the globals and all connected runners", holds only current state, and needs the original story file to load. — search summary of the [inkcpp wiki](https://ddprojects.chickenkiller.com/JBenda/inkcpp/wiki/Using-the-InkCPP-Runtime) and [JBenda/inkcpp](https://www.github.com/JBenda/inkcpp)
- **Yarn Spinner:** `<<if>>/<<elseif>>/<<else>>/<<endif>>` blocks; `$variables` set with `<<set $x to 5>>`; expressions must be boolean (`<<if 1>>` is invalid). Conditional options are written `-> Sure I am! <<if $reputation > 10>>`. Unavailable options are still sent to the game, which decides how to show them. — [Yarn Spinner docs: Flow Control](https://docs.yarnspinner.dev/write-yarn-scripts/scripting-fundamentals/flow-control)
- Yarn Spinner's official integrations are Unity, Godot (C#) and Unreal (a "pure-C++ implementation for Unreal Engine 5.4+"). No official standalone C++ runtime was found. — search summary of [Yarn Spinner for Unreal](https://www.yarnspinner.dev/docs/unreal/), [Godot overview](https://docs.yarnspinner.dev/yarn-spinner-for-godot/godot-csharp/overview), and [GitHub repositories](https://docs.yarnspinner.dev/about/github-repositories)

### Inferences
- **Recommended custom format.** Extend the existing per-spawn LINE data, or a sidecar dialogue/quest file (see §4). Each line becomes `{ text, conditions[], actions[] }`, evaluated top-down with the first passing line winning (Morrowind rule), plus a fallback group of unconditioned "idle" lines that keeps today's cycling behaviour.
- **Condition vocabulary:** `stage(quest) <op> n`, `stage_done(quest, n)`, `objective(quest, i) state`, `flag(name) <op> n`, `has_item(id) >= n`, `in_region(R)`, `npc_dead(spawn)`. Use one flat list with an AND default and an OR flag that binds tighter (the CK rule) to avoid writing an expression parser. If an expression parser is wanted later, Yarn's syntax is a good model.
- **Actions** reuse the quest actions: set stage, set flag, give/take item, show objective.
- **Why not Ink/Yarn now:** both own a separate variable store, which must be synced with the quest/flag state and saved separately. Both need an external compiler in the content pipeline (inklecate is .NET; Yarn's compiler is C#). Their strength, branching player-choice conversations, is not what flycoos has: E-to-talk NPCs saying one line. The world editor and MCP bridge also already edit LINE data, and a plain format keeps that editable in-game and by Claude.
- **When to reconsider:** if conversations gain player choices and multi-line branching, inkcpp is the only credible drop-in C++ runtime of the two (MIT, allocation-conscious). Yarn would mean porting a runtime.
- **World-state flags:** one `std::unordered_map<std::string, int32_t>` of named flags/globals, saved in the player save, is the CK "Global Variables" equivalent. Quest stages should stay queryable as their own conditions rather than mirrored into flags, so there is one source of truth.

### Gaps
- The full CK condition-function list (GetStageDone, GetIsAliasRef, GetItemCount, GetGlobalValue, GetIsID) and the "Use Global" / "Swap Subject and Target" options were not read from a primary page (403). Only the AND/OR precedence and Subject/Target semantics are confirmed via secondary summaries.
- The OpenMW dialogue/journal implementation was not examined (UESP filter page 404; OpenMW source not fetched).
- No sources were found on how the Witcher's dialogue gating differs from Bethesda's.

---

## 3. Event bus vs direct calls: what works for a small C++ game?

### Takeaway
Use the Observer pattern (a synchronous callback list) when one system announces something "interesting" to loosely related systems: combat to quests, inventory to quests, dialogue to quests. Use a queue only when you need decoupling in time, such as handling events at a defined point in the frame or avoiding re-entrancy. For flycoos, a single tiny event channel works: a `std::vector<Event>` that systems push to during the frame, which the quest system drains once per frame. It is simpler than a general pub/sub bus and avoids the re-entrancy and lifetime problems Nystrom warns about.

### Cited Findings
- Observer lets code "announce that something interesting happened" without caring who receives it. The example is physics notifying achievements without referencing them. — [Game Programming Patterns: Observer](https://gameprogrammingpatterns.com/observer.html)
- Observers are synchronous: "A slow observer can block a subject". The cost is "walking a list and calling virtual methods", so it is cheap outside hot paths. — [Game Programming Patterns: Observer](https://gameprogrammingpatterns.com/observer.html)
- The Observer pattern brings "dangling observers" and the "lapsed listener problem". Because the links are made at runtime, they are hard to trace statically. Guideline: if you often need to understand both sides of a communication, "don't use the Observer pattern to express that linkage". It works best between loosely related parts of a system, not within one feature. — [Game Programming Patterns: Observer](https://gameprogrammingpatterns.com/observer.html)
- Event Queue is "the asynchronous cousin" of Observer. Use it "when you want to decouple something in time". Its example is combat posting "enemy died" to a queue the tutorial system reads, "without the two being directly aware of each other". — [Game Programming Patterns: Event Queue](https://gameprogrammingpatterns.com/event-queue.html)
- Queue pitfalls:
  - A central queue "is still a global, with all of the danger that entails".
  - Events must carry their data, because the world may have changed (e.g. the entity is gone) by processing time.
  - A common rule is to avoid sending events while handling one, which prevents feedback loops.
  - Ownership passes to the queue.
  - Processing lag "can visibly affect behavior".
  - A queue is "a poor fit when the sender needs a response".

  — [Game Programming Patterns: Event Queue](https://gameprogrammingpatterns.com/event-queue.html)
- Beyond Skyrim lists "Story Manager events" as a distinct quest-start mechanism. Bethesda routes some world events (kills, location changes and similar) through a central manager that starts quests, rather than having systems call quests directly. — [Beyond Skyrim Arcane University: Quest implementation](https://wiki.beyondskyrim.org/wiki/Quest_Implementation) (topic list only; mechanism detail not read)

### Inferences
- **Recommended flycoos shape (lazy version):**
  - `struct GameEvent { Kind kind; std::string id; int a; glm::vec3 pos; }`, carrying data by value (per Nystrom's queue pitfall).
  - Combat, inventory, dialogue and region checks call `events.push_back(...)`.
  - Once per frame, `quests.update(events, state)` consumes them and clears the vector.
- **What this design gives:**
  - No subscriber registration, so no dangling listeners.
  - A deterministic processing point.
  - Quest actions that fire their own events (e.g. "give item" raises ItemCountChanged) land in the next frame's batch, not recursively. That prevents feedback loops.
- **Ceiling:** with two or more consumers (achievements, a tutorial, UI toasts), loop a fixed list of consumer functions over the same vector. A full pub/sub bus with type-erased handlers is only worth it at many consumers or across threads.
- **Direct calls stay right** for request/response (e.g. dialogue asking "does this condition pass?") because a queue cannot return a value. The condition evaluator should be a plain function reading quest/flag/inventory state.
- **Region entry is derived, not pushed.** flycoos already has `regionAt(markers, pos)`. Compare the player's region with last frame's and emit EnteredRegion on change.

### Gaps
- No postmortem or blog specifically comparing event buses with direct calls in a shipped small C++ RPG was found within the budget. The recommendation rests on Nystrom's pattern guidance.

---

## 4. What should a save file contain when the world is authored and editable, and how to version it?

### Takeaway
Bethesda's model is the reference: the authored data (master/plugin files) stays immutable at runtime. The save holds a header, the list of content files it depends on, global tables (globals, player location, etc.) and "change forms": per-object deltas tagged with change flags that say which aspects changed (moved, inventory, life state, quest stages, objectives…). For flycoos, keep `.flyw` as the authored base. Write a separate player save that references the world file and stores the player, quest/flag state, per-NPC/spawn deltas and voxel edits as chunk deltas. Version it with an integer plus chained one-step migrations, and write it atomically (temp file, fsync, rename, fsync directory) with a checksum and a `.bak`.

### Cited Findings
- Skyrim save layout:
  - Magic `TESV_SAVEGAME`, then a header: save number, player name, level, location, in-game date, race, sex, XP, FILETIME, screenshot dimensions, compression type.
  - A screenshot.
  - A `formVersion` (74 for LE 1.9 / SE 1.5.97).
  - The plugin list.
  - A file location table.
  - Three global data tables (types 0–8: Misc Stats, Player Location, TES, Global Variables, Created Objects, Effects, Weather, Audio, SkyCells; 100–114: process lists, combat, interface…; 1000–1005: Papyrus etc.).
  - Change forms, then a formID array.

  — [UESP Skyrim Mod:Save File Format](https://en.uesp.net/wiki/Skyrim_Mod:Save_File_Format)
- Each change form is `formID (3-byte RefID), changeFlags (uint32), type (uint8), version (uint8), length1, length2?, data`, and "the layout of the data section is not a Record as it is in a mod file". If length2 ≠ 0 the data is zlib-compressed. The header compression type can be none, zlib or LZ4. — [UESP Skyrim Mod:Save File Format](https://en.uesp.net/wiki/Skyrim_Mod:Save_File_Format)
- Each change form also carries its own version byte, so per-record versioning sits under the file's `formVersion`. — [UESP Skyrim Mod:Save File Format](https://en.uesp.net/wiki/Skyrim_Mod:Save_File_Format)
- Change flags say which aspects of an object differ from the base data:
  - REFR/ACHR: `CHANGE_REFR_MOVE` (0x2), `CHANGE_REFR_CELL_CHANGED` (0x8), `CHANGE_REFR_SCALE`, `CHANGE_REFR_INVENTORY` (0x20), `CHANGE_REFR_BASEOBJECT`.
  - ACHR: `CHANGE_ACTOR_LIFESTATE` (0x400), plus disposition and damage modifiers.
  - REFR objects: `CHANGE_OBJECT_EXTRA_LOCK`, `CHANGE_OBJECT_OPEN_STATE`, `CHANGE_OBJECT_EMPTY`.
  - QUST: `CHANGE_QUEST_STAGES`, `CHANGE_QUEST_OBJECTIVES`, `CHANGE_QUEST_FLAGS`, `CHANGE_QUEST_ALREADY_RUN`, `CHANGE_QUEST_SCRIPT`.
  - `CHANGE_FORM_FLAGS` (0x1) for all records.

  — [UESP Skyrim Mod:ChangeFlags](https://en.uesp.net/wiki/Skyrim_Mod:ChangeFlags)
- Bethesda's delta model fails in a known way, "save bloat": objects deleted by script are removed from play but stay in the save, and references that should be removed sometimes are not, so they persist indefinitely. One user reported a 60-hour save with more than 24,000 references; cleaning took it from 14 MB to 11 MB. These are community reports, not Bethesda statements. — [Nexus forums: save bloat](https://forums.nexusmods.com/topic/786371-save-bloat-should-i-worry), [Nexus: Save Game Cleaner](https://www.nexusmods.com/skyrim/mods/31724), [STEP forum: save bloat](https://stepmodifications.org/forum/topic/5853-save-bloat-what-is-it-why-does-it-cause-ctd-why-does-this-fix-it/)
- Ink keeps its own serializable state (`state.ToJson()`), and an inkcpp snapshot needs the original story file to load. Any embedded dialogue runtime adds a save blob tied to a specific content version. — [inkle: Running your ink](https://github.com/inkle/ink/blob/master/Documentation/RunningYourInk.md); search summary of the [inkcpp wiki](https://ddprojects.chickenkiller.com/JBenda/inkcpp/wiki/Using-the-InkCPP-Runtime)
- Atomic replace on POSIX:
  1. Write a temp file in the same directory.
  2. fsync it.
  3. Rename it over the original.
  4. fsync the parent directory.

  Dan Luu cautions:
  - "rename isn't atomic on crash" in POSIX's guarantees (only during normal operation).
  - A new file may be absent until the directory is fsynced.
  - macOS needs `fcntl(F_FULLFSYNC)` for a real flush.
  - "The most common class of error was incorrectly assuming ordering between syscalls."

  — [Dan Luu: Files are hard](https://danluu.com/file-consistency/)
- Game-oriented guidance (secondary, lower-quality sources):
  - Write temp then rename. Rename is atomic on NTFS/ext4/APFS with FAT32 caveats ([bugnet.io](https://bugnet.io/blog/how-to-debug-game-save-corruption-bugs)), but it is not guaranteed on Windows, so keep a `.bak` ([claudeskills.info save-systems](https://claudeskills.info/zh/skills/gamedev-skills/awesome-gamedev-agent-skills/save-systems/)). The two sources conflict on Windows atomicity.
  - CRC32 catches bit rot and external edits and is a reasonable speed/reliability trade-off ([bugnet.io](https://bugnet.io/blog/how-to-debug-game-save-corruption-bugs)). A commercial Unreal save plugin verifies SHA-256 before running migrations ([Unreal forums: NEXUS Persistence](https://forums.unrealengine.com/t/gameforgex-nexus-persistence-versioned-save-system-with-migrations/2776042)).
  - Embed a version integer, migrate with chained single-step functions (vN→vN+1), validate on load and fall back to the backup ([claudeskills.info save-systems (ja)](https://claudeskills.info/ja/skills/gamedev-skills/awesome-gamedev-agent-skills/save-systems/)). The Godot SaveState addon combines temp-file writes, validation, a `.bak`, and a schema number with merge-from-defaults ([awesome.ecosyste.ms: savestate](https://awesome.ecosyste.ms/projects/github.com%2Fyoussof20%2Fsavestate)).

### Inferences
- **Separate files (the Bethesda master/save split):**
  - `world.flyw` is authored and edited only in the editor.
  - `save_N.flys` is runtime, and its header stores the world file name plus a world content hash or version.
  - On load: read `.flyw`, then apply the save's deltas. If the world hash changed (the designer edited the world after the save), deltas keyed by stable IDs still apply. Deltas keyed by positions or indices may not.
- **Stable IDs are the prerequisite.** Bethesda keys every change form by formID. flycoos instances and markers are already "sorted by id" in `.flyw`, so key NPC deltas by spawn-marker id, and NPCs within a spawn by slot, never by vector index. Key quests and items by string ID.
- **What the player save contains** (each a tagged section, reusing `.flyw`'s tagged-section style):
  - HEAD: version, world file name + hash, play time, timestamp, player name/level.
  - PLYR: position, yaw, health/stats, level/XP.
  - INVT: inventory + equipped.
  - QUST: per quest {stage, stages_done bitset/list, objective states, counters, state active/done/failed}, plus the tracked quest.
  - FLAG: the global flags map.
  - NPCS: per spawn marker {dead/alive per slot, respawn timer, line cursor}. Persist an NPC position only if it matters.
  - VOXD: voxel deltas as chunk-level diffs: each chunk the player modified, RLE'd with the existing chunk codec.
  - Time of day / weather if they exist.
- **Voxel deltas vs full world:** store only player-modified chunks. A full 1024×192×1024 world is mostly unchanged authored data, and saving whole modified chunks (rather than per-voxel ops) is simplest and bounded. Compare Bethesda's CELL/REFR change forms, which store changed aspects rather than whole cells. Skip the bloat trap: when a delta chunk equals the base chunk again, drop it.
- **Versioning:** a `uint32 version` at the top plus tagged sections (unknown tags skipped, missing tags defaulted) covers most evolution with no migration code. Add explicit `migrate_vN_to_vN1` steps only for semantic changes. Append-only enums (flycoos already does this for entity kinds) keep old numeric IDs valid.
- **Robustness (minimum):**
  - Write to `save.tmp`, then `fsync` (`F_FULLFSYNC` on macOS, the dev platform).
  - Rename the previous save to `save.bak`, rename tmp to save, then fsync the directory.
  - Write a CRC32 of the payload in the header. On a bad CRC or a failed parse, offer `.bak`.
  - Bounds-check every count on load, as `.flyw` already does with `kMax*` caps, because the save is untrusted input.

### Gaps
- No primary Bethesda/engine-developer source explains the design rationale of change forms (e.g. why flags rather than full records). The structure above is reverse-engineered by UESP.
- No authoritative source covers Windows `ReplaceFile`/`MoveFileEx` atomicity, so the secondary sources conflict. This matters only if flycoos ships on Windows.
- No source was found on reconciling saves with a later-edited authored world (designer changes the world after the player saved). Bethesda's equivalent is mod load-order changes breaking saves, but no authoritative write-up was retrieved.
- Veloren and OpenMW save implementations were not examined within the tool-call budget.
