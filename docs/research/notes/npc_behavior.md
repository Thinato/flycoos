# NPC Behavior Architectures for an Action RPG (FSM, HFSM, BT, Utility, GOAP) and Their Data Files

Context for the report writer: flycoos NPCs today are defined in `src/entity.h` / `src/entity.cpp`. An `EntityType` table row holds `temper` (Harmless/Passive/Aggressive), `aggroRange`, `wanderRadius`, `chaseSpeed`, weapon/loot/respawn. Each `Entity` carries `bool angry`, `wanderDir`/`wanderTimer`, speech/shout timers, and `home`. `kLeashRadius = 20` is a global constant. `updateEntities()` is one loop with an implicit 4-state machine: dead / angry (chase + punch, no pathfinding) / talking (face player) / wander. `src/debug_ui.cpp` already has an ImGui entity panel that shows "ANGRY/calm", aggro, leash and chase values, plus an Anger/Calm toggle button. `nlohmann/json` is already included in the project, so JSON data files cost no new dependency. (Verified by reading the repo on 2026-11-03.)

Source access caveats: the Bethesda Creation Kit wikis (ck.uesp.net, skyrimck.uesp.net, creationkit.com) and Nexus articles returned HTTP 403 or a maintenance redirect. Game AI Pro PDFs downloaded but could not be text-extracted (no PDF tools on the machine). Skyrim detail therefore comes from UESP's PACK file-format page and search-result snippets of the CK wiki. Unverified recall is marked as such and kept in Gaps.

## Q1. For a small game, when is a data-driven FSM enough, and when do behavior trees or utility AI pay off?

### Takeaway
For flycoos's creature list (boar, wolf, bandit, ghoul, guard), a small data-driven FSM is enough. Each creature has a handful of distinct modes that respond to events over time, which is exactly where FSMs fit. Behavior trees pay off once behaviors share reusable sub-sequences, such as villager routines with multi-step activities. Utility AI pays off when many options compete on graded scores, such as which target, flee or fight, or which need to satisfy. GOAP is overkill for one developer and this roster.

### Cited Findings
- FSMs replace piles of boolean flags (`isJumping_`, `isDucking_`) that allow invalid combinations. A fixed set of states with one current state guarantees only valid states, and an enum plus switch groups each state's code. — [Game Programming Patterns: State](https://gameprogrammingpatterns.com/state.html)
- FSMs fit when behavior depends on internal state, the state falls into a small set of distinct options, and the entity responds to a sequence of inputs over time. They are "not Turing complete" and hit limits quickly in complex AI; the author points to behavior trees and planning systems for more. — [Game Programming Patterns: State](https://gameprogrammingpatterns.com/state.html)
- Entry and exit actions put per-state setup in one place. Concurrent machines (for example movement and equipment) need n + m states instead of n × m. Hierarchical FSMs let substates inherit handling from a superstate, with unhandled events passing up the chain. Pushdown automata (a state stack) handle "do X temporarily, then resume". — [Game Programming Patterns: State](https://gameprogrammingpatterns.com/state.html)
- A behavior tree is a "tree of hierarchical nodes" that controls the flow of execution, unlike an FSM. Each tick goes from the root to a leaf, and each node returns SUCCESS, FAILURE or RUNNING. Node kinds are Control (Sequence, Fallback), Decorator (Inverter, Retry), Condition (never RUNNING) and Action. — [BehaviorTree.CPP: Introduction to BTs](https://www.behaviortree.dev/docs/learn-the-basics/BT_basics)
- A BT pitfall even in the intro docs: in "Fetch me a beer", if GrabBeer fails, the Sequence skips CloseFridge and the door stays open. Cleanup or exit logic has to be designed in explicitly (there it is a Fallback). — [BehaviorTree.CPP: Introduction to BTs](https://www.behaviortree.dev/docs/learn-the-basics/BT_basics)
- Unreal's BTs are "event-driven to avoid doing unnecessary work every frame". They use Decorators (not leaf tasks) as conditionals, Observer Aborts to interrupt a running branch when a watched value changes, and Services that run on a timer while a subtree is active (for example to pick the best enemy target). — [Unreal Engine BT overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview)
- Utility AI: Dave Mark's Infinite Axis Utility System (IAUS) is a "stand-alone architecture based entirely on utility mechanisms", "entirely data-driven through a database oriented design tool". Behaviors are scored together so they "play together", which the page says makes the AI less rigid and less likely to get stuck. It was discussed at GDC 2015 in relation to *Guild Wars 2: Heart of Thorns*, and designers reportedly built unique NPC AI in "as little as 7 minutes". — [gameai.com IAUS](https://gameai.com/iaus.php) (vendor page, promotional claims)
- A GDC 2024 AI Summit session on practical utility AI covers considerations, utility curves and decision hierarchies, noting that designers tune behavior by manipulating data and curves. — [GDC 2024 schedule: Practical Utility AI](https://schedule.gdconf.com/session/ai-summit-practical-utility-ai/902394) (search snippet only)
- GOAP: F.E.A.R.'s AI (Jeff Orkin, GDC 2006, "Three States and a Plan") used an FSM with only three states (Goto, Animate, UseSmartObject). An A* planner, closest to academic STRIPS, sequenced actions toward goals. — [Orkin, GDC 2006 paper (UW-Madison mirror)](https://pages.cs.wisc.edu/~dyer/cs540/handouts/gdc2006_orkin_jeff_fear.pdf); [GDC Vault](https://gdcvault.com/play/1013282/Three-States-and-a-Plan) (details via search summaries; the PDF was not read directly)
- Cataclysm DDA expresses creature temperament as data numbers, not code: `aggression` (hostile at 10+) and `morale` (flees when negative, unless anger + morale > 0 and HP > 1/3), each "tending towards this [baseline] value", with trigger lists that move them. That is a utility-flavoured, data-driven layer on top of fixed monster behavior. — [CDDA MONSTERS.md](https://docs.cataclysmdda.org/JSON/MONSTERS.html)

### Inferences
- flycoos's current `updateEntities` already is an FSM with implicit states (dead / angry / talking / wander) encoded in a bool and two timers. The lowest-cost step is to make it explicit (`enum class AiState { Idle, Wander, Alert, Chase, Attack, Return, Flee, Dead }`) with enter/exit actions and a `stateTime`, still in C++. Data-drive the parameters (ranges, timings, flee threshold) per kind from JSON, not the graph itself. Five creature kinds sharing one combat state graph do not need per-kind graphs.
- Data-drive the graph (a states + transitions JSON) only when two or more creature kinds truly need different topologies, for example a ghoul that never flees and a wolf that circles. Even then, a few boolean "traits" in the parameter file (`flees`, `circles`, `calls_pack`) usually cover it at far less cost.
- Behavior trees pay off for villager daily routines (go to the bench, sit, eat, go home, sleep: sequences with fallbacks) more than for combat. Skyrim itself puts the schedule layer in a tree-like "procedure tree" (see Q2).
- Utility scoring pays off locally, without a full utility architecture: choosing flee vs fight (HP fraction, allies near, level difference), choosing a target among several, and choosing which routine activity to do. One scoring function per decision point, inside the FSM, gives most of the benefit.
- GOAP: skip it. Its strength is emergent multi-step plans for smart human enemies. For boars and bandits it adds a planner, world-state symbols and debugging difficulty a solo developer does not need.

### Gaps
- Could not extract the Game AI Pro chapters (BT Starter Kit, Rez Graham's Introduction to Utility Theory, Anthony Francis's BT pitfalls, Rez Graham's Light-Weight FSM) for direct quotes. URLs: [BT Starter Kit](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter06_The_Behavior_Tree_Starter_Kit.pdf), [Intro to Utility Theory](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter09_An_Introduction_to_Utility_Theory.pdf), [Overcoming Pitfalls in BT Design](http://www.gameaipro.com/GameAIPro3/GameAIPro3_Chapter09_Overcoming_Pitfalls_in_Behavior_Tree_Design.pdf), [Reusable Light-Weight FSM](http://www.gameaipro.com/GameAIPro3/GameAIPro3_Chapter12_A_Reusable_Light-Weight_Finite-State_Machine.pdf).
- Found no quantitative comparison (benchmarks or authoring time) between FSM, BT and utility for small games. The guidance above is synthesis, not measured.
- Veloren's agent code and OpenMW's AI package implementation were not reviewed (time budget).

## Q2. How do Skyrim's AI packages and combat styles work at a high level, and what is worth borrowing?

### Takeaway
Skyrim splits non-combat routine (an ordered stack of "packages", each gated by a schedule and conditions and built from a template's procedure tree) from combat (a separate, largely hard-coded combat AI tuned per actor by numeric Combat Style and AI Data values like Aggression and Confidence). Worth borrowing for flycoos: the split itself, the "first valid package in an ordered list wins" rule, schedule windows in data, and personality as a few enum or number knobs. Not worth borrowing: the full procedure-tree editor.

### Cited Findings
- Oblivion's Radiant AI gave NPCs "full 24/7 schedules", with goals like "Eat Breakfast," "Go to Work," or "Socialize" paired with available locations and objects. — [Giant Bomb: Radiant AI](https://giantbomb.com/wiki/Concepts/Radiant_A_I) (search snippet); UESP describes it as "NPCs interacting with their environment and possessing daily schedules" — [UESP Oblivion:Radiant](https://en.uesp.net/wiki/Oblivion:Radiant)
- Skyrim lead designer Bruce Nesmith said the system was more complex and deeper in Skyrim than in Oblivion, but players are more likely to notice it in the older game. — [FRVR summary of Nesmith interview](https://frvr.com/blog/news/elder-scrolls-designer-explains-that-radiant-ai-is-an-enormous-investment-for-bethesda-but-every-improvement-actually-makes-it-less-noticeable-to-players/) (secondary)
- Skyrim PACK record structure:
  - Flags include "Must Complete" and "Once Per Day".
  - Schedule (PSDT) has month, day of week (-1 any; groups like "7=Weekdays"), date, hour (0-23, -1 any), minute, and duration in minutes.
  - Package-level conditions (CTDA) control when the package can be active.
  - A package points to a Package Template and fills its typed "data inputs" (Location such as "Near reference"/"In cell"/"Near Self", Target such as "Specific reference"/"Object Type"/"Self", Bool, Float, Int, Topic).
  - Templates hold a procedure tree whose branch types are Sequence, Random, Simultaneous and Stacked, with leaf Procedures. Each branch can carry its own conditions and a "Success Completes Package" flag.
  - Packages have On Begin / On End / On Change hooks (idle animation + dialogue topic).
  — [UESP Tes5Mod: PACK format](https://en.uesp.net/wiki/Tes5Mod:Mod_File_Format/PACK)
- Package-level conditions sit in the header, and branch-level conditions in the procedure tree. — [UESP PACK](https://en.uesp.net/wiki/Tes5Mod:Mod_File_Format/PACK)
- Oblivion's "wander" maps to Skyrim's "Sandbox" package, and NPCs use placed idle markers while sandboxing. — [Nexus: How AI Overhaul works](https://www.nexusmods.com/skyrimspecialedition/articles/2467) (search snippet; the page itself was 403)
- In a Fallout 4 (same engine family) guide, a stacked main procedure tree makes "the following sequences ... act like package stacks with priority from top to bottom". A higher-priority package takes over once its conditions are met. — [Nexus FO4 article 3549](https://nexusmods.com/fallout4/articles/3549) (search snippet)
- Combat personality (AI Data): Confidence governs when an actor avoids or flees threats. "Cowardly" always avoids/flees; "Cautious" avoids unless stronger than the threat. Aggression "Average" flees if outmatched; "Foolhardy" never flees. — [UESP CK: AI Data / Confidence](https://skyrimck.uesp.net/wiki/Confidence); [UESP CK: Aggression](https://ck.uesp.net/wiki/Aggression) (search snippets)
- The specific decisions of Skyrim's combat AI "cannot be controlled through the Creation Kit"; the limited lever is Combat Override Packages on the actor's AI Packages tab. The Combat Style field "Avoid Threat Chance" is listed as not used or unknown. — [UESP CK: Combat](https://ck.uesp.net/wiki/Combat); [UESP CK: Combat Style](https://ck.uesp.net/wiki/Combat_Style) (search snippets)
- A modder reports making generic sleep/eat packages that use generic variables and a staggered schedule. — [Nexus forum, user pnubs](https://forums.nexusmods.com/profile/5485031-pnubs/content/) (search snippet, anecdotal)

### Inferences
Borrow for flycoos:
1. **Two layers.** A per-kind combat FSM (data-tuned), plus later a schedule layer for villagers and guards that only runs while not in combat. Combat preempts the schedule; when combat ends, the NPC re-evaluates the schedule.
2. **Ordered package list per NPC, first valid wins.** Each package = `{ activity, hours window, conditions, location (marker id) }`. This is far simpler than a BT and maps well onto flycoos's existing story markers (points of interest, regions) as package targets.
3. **Personality as two or three knobs.** Replace or extend `Temper` with Skyrim-like `aggression` (Unaggressive / Aggressive / Frenzied, approximately the current Harmless / Passive / Aggressive) and `confidence` (Cowardly … Foolhardy), which decides fleeing. Combat style becomes a few multipliers (attack cooldown, circle vs press-in, fallback chance).
4. **Begin/End hooks** on activities (play an idle, say a line) are cheap and add a lot of life.

Skip: procedure-tree editors, Random/Simultaneous branches, the full condition-function system. A handful of named condition predicates is enough.

### Gaps
- The CK wiki pages for Package, Combat Style and package-stack evaluation timing returned 403. Unverified recall, not confirmed this session:
  - The stack is evaluated top to bottom, with the first package whose schedule and conditions pass running.
  - Re-evaluation happens periodically, on cell load, and via the Papyrus `EvaluatePackage()`.
  - Combat Style fields include offensive/defensive multipliers, melee "circle" and "fallback" multipliers, "flank distance" and "stalk time".
  These should be confirmed before the report states them as fact.
- AI and Games (Tommy Thompson) articles on Radiant AI were not found in search results.

## Q3. How are behaviors represented in data, and how do you keep them debuggable?

### Takeaway
The common representations are (a) per-creature numeric parameter blocks plus trigger lists (CDDA monster JSON), (b) states with transitions guarded by named conditions (FSM JSON), and (c) node trees with blackboard-keyed ports (BehaviorTree.CPP XML; Unreal BT + Blackboard assets). All of them stay debuggable only if the runtime exposes the current state or active node, the reason for the last transition, and the blackboard values. For flycoos that means an ImGui panel extending the existing entity debug view.

### Cited Findings
- BehaviorTree.CPP XML: nodes are elements, and attributes are ports. A port takes a literal (`message="hello world"`) or a blackboard key in braces (`message="{greetings}"`). Output ports write to the blackboard (`text="{the_answer}"`), and two nodes connect by sharing a key. Port types must match, or `createTreeFromFile` throws. Verbatim example:
  ```xml
  <root BTCPP_format="4" >
      <BehaviorTree ID="MainTree">
         <Sequence name="root_sequence">
             <SaySomething     message="hello" />
             <ThinkWhatToSay   text="{the_answer}"/>
             <SaySomething     message="{the_answer}" />
         </Sequence>
      </BehaviorTree>
  </root>
  ```
  — [BehaviorTree.CPP tutorial 02: ports](https://www.behaviortree.dev/docs/tutorial-basics/tutorial_02_basic_ports)
- The blackboard is "a key/value store shared by all nodes in the tree". — [BehaviorTree.CPP tutorial 02](https://www.behaviortree.dev/docs/tutorial-basics/tutorial_02_basic_ports). In Unreal it is "a separate asset called a Blackboard", read through Blackboard Keys. — [UE BT overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview)
- Unreal places conditional decorators at the root of the branch they guard "so the affected part of the tree is easy to see", which is a readability and debuggability argument. — [UE BT overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview)
- CDDA monster data: `aggression`, `morale`, `aggro_character`, `anger_triggers` / `fear_triggers` / `placate_triggers` (shared flag vocabulary, for example HURT, PLAYER_CLOSE, FRIEND_ATTACKED, PLAYER_WEAK, STALK, SOUND, MEAT), `vision_day` / `vision_night` (defaults 40 and 1 tiles), and `death_function`. — [CDDA MONSTERS.md](https://docs.cataclysmdda.org/JSON/MONSTERS.html); trigger names from [mutant_mammal.json (0.F)](https://grok.dragonflybsd.org/xref/dports/games/cataclysm-dda-tiles/Cataclysm-DDA-0.F/data/json/monsters/mutant_mammal.json) (search summary)
- On CDDA trigger magnitudes, a forum answer says HURT scales with damage and PLAYER_CLOSE adds a fixed amount (reportedly 5). — [CDDA forum: How does aggression work?](https://discourse.cataclysmdda.org/t/how-does-aggression-work/19608) (community, unverified)
- Skyrim packages are data records: a template (procedure tree) plus per-instance typed inputs, schedule and conditions. — [UESP PACK](https://en.uesp.net/wiki/Tes5Mod:Mod_File_Format/PACK)

### Concrete data-file examples (proposed for flycoos; designed here, not copied from a source)

**A. Parameter-driven FSM (recommended first step).** One shared combat graph in C++ and per-kind numbers in JSON. This replaces the `EntityType` columns and `kLeashRadius`:
```json
{
  "boar": {
    "temper": "passive",
    "perception": { "aggro_range": 8, "fov_deg": 360, "needs_los": false, "lose_sight_after": 4.0 },
    "leash": { "radius": 18, "return_heal_per_s": 5 },
    "move": { "wander_radius": 10, "chase_speed": 5.5 },
    "attack": { "reach": 1.6, "windup": 0.45, "recover": 0.6, "cooldown": 1.2 },
    "flee": { "hp_below": 0.2, "chance": 0.5, "duration": 4.0 }
  },
  "barrow_ghoul": {
    "temper": "aggressive",
    "perception": { "aggro_range": 10, "fov_deg": 140, "needs_los": true, "lose_sight_after": 6.0 },
    "leash": { "radius": 25, "return_heal_per_s": 0 },
    "move": { "wander_radius": 4, "chase_speed": 4.0 },
    "attack": { "reach": 1.8, "windup": 0.7, "recover": 0.8, "cooldown": 1.6 },
    "flee": null
  }
}
```

**B. Data-driven FSM graph (only if topologies diverge).** States with enter actions and ordered, guarded transitions. The first passing transition wins, and `min_time` gives hysteresis:
```json
{
  "id": "melee_creature",
  "initial": "wander",
  "states": {
    "wander": { "on_enter": ["pick_wander_point"],
                "transitions": [ { "to": "chase",  "if": "sees_target" },
                                 { "to": "chase",  "on": "damaged" } ] },
    "chase":  { "on_enter": ["shout"],
                "transitions": [ { "to": "return", "if": "target_beyond_leash" },
                                 { "to": "return", "if": "lost_sight_for", "arg": "lose_sight_after" },
                                 { "to": "flee",   "if": "hp_below", "arg": "flee.hp_below" },
                                 { "to": "windup", "if": "target_in_reach", "min_time": 0.2 } ] },
    "windup": { "on_enter": ["start_windup_anim"], "duration": "attack.windup",
                "transitions": [ { "to": "recover", "on": "timeout", "do": ["deal_hit"] } ] },
    "recover":{ "duration": "attack.recover",
                "transitions": [ { "to": "chase", "on": "timeout" } ] },
    "flee":   { "duration": "flee.duration",
                "transitions": [ { "to": "return", "on": "timeout" } ] },
    "return": { "on_enter": ["clear_target"],
                "transitions": [ { "to": "wander", "if": "at_home", "do": ["heal_full"] } ] }
  }
}
```
Condition and action names map to a C++ `std::unordered_map<std::string, fn>` registry. An unknown name fails loudly at load, the same discipline as BehaviorTree.CPP's port-type check.

**C. Behavior tree for a villager routine (later), in JSON mirroring BT.CPP semantics:**
```json
{ "selector": [
  { "sequence": [ { "cond": "in_combat" }, { "action": "flee_to", "target": "{home}" } ] },
  { "sequence": [ { "cond": "hour_between", "from": 22, "to": 6 },
                  { "action": "goto", "target": "{bed}" }, { "action": "sleep" } ] },
  { "sequence": [ { "cond": "hour_between", "from": 12, "to": 13 },
                  { "action": "goto", "target": "{tavern}" }, { "action": "idle", "anim": "eat", "secs": 60 } ] },
  { "action": "sandbox", "around": "{workplace}", "radius": 6 }
]}
```
`{bed}`, `{tavern}` and the rest are blackboard keys filled from flycoos story markers. Note that this "selector of guarded sequences" is the same thing as a Skyrim-style ordered package list. A plain list may suffice:

**D. Skyrim-style package list (simplest schedule format):**
```json
"schedule": [
  { "hours": [22, 6],  "do": "sleep",   "at": "marker:ravensford_inn_bed_2" },
  { "hours": [12, 13], "do": "eat",     "at": "marker:ravensford_tavern" },
  { "hours": [6, 22],  "do": "sandbox", "at": "marker:ravensford_smithy", "radius": 6 }
]
```

### Inferences
- Debug UI: extend `debug_ui.cpp`'s entity panel (it already shows ANGRY/calm and has a toggle) to show:
  - current state name and time in state;
  - the last transition with its reason (for example `chase -> return: target_beyond_leash`) and a ring buffer of the last 8 transitions;
  - the target, distance, LOS result and perception timers;
  - for packages, the active package and why higher-priority ones failed.
  Add world-space gizmos (aggro circle, leash circle around `home`, FOV cone, line to target) and a "force state" combo box instead of the single Anger/Calm toggle.
- Store a `const char* reason` with each transition at runtime, essentially free. It is the single most useful debug datum, because the most common question is "why did it stop chasing?".
- Hot-reload the JSON (watch mtime or a debug-UI button) so tuning numbers doesn't require a rebuild. The project already reloads world data via the editor and MCP bridge, so the pattern fits.
- Perception pattern: compute perception at a lower rate than movement (for example 5 Hz), write the results to a small per-entity blackboard struct (`target`, `canSee`, `lastSeenPos`, `lastSeenTime`, `distToTarget`, `distTargetToHome`), and let the FSM read only the blackboard. This separates the expensive part (LOS) from decisions and makes everything displayable. LOS in a voxel world is a 3D DDA ray march, cheap for ranges of 10 to 20 blocks.

### Gaps
- Did not verify BehaviorTree.CPP's Groot2 live-debug features or its logger APIs (the fetched page did not cover them).
- No primary source fetched on visual BT debuggers in shipped games.

## Q4. Common pitfalls (state explosion, oscillation, tick cost) and fixes (hysteresis, timers, LOD ticking)

### Takeaway
State explosion has well-known fixes: concurrent machines (n + m instead of n × m), hierarchy, and a state stack. Oscillation is fixed by separate enter and exit thresholds, minimum dwell times, and values that decay toward a baseline (CDDA's anger and morale). Tick cost is fixed by event-driven or periodic evaluation (UE services, timer-based perception) and level-of-detail ticking by distance. At a few hundred simple NPCs, the cost is in pathing, LOS and physics, not in the decision structure.

### Cited Findings
- State explosion: concurrent state machines give n + m states instead of n × m. Hierarchical FSMs share handling via superstates. Pushdown automata give "resume what I was doing". — [Game Programming Patterns: State](https://gameprogrammingpatterns.com/state.html)
- Tick cost: Unreal BTs are event-driven and do "not have to iterate through the entire tree every tick". Services run "every specified amount of seconds" while their subtree is active. Observer-aborting decorators interrupt only when a watched value changes. — [UE BT overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview)
- Oscillation and damping via baselines: in CDDA, aggression and morale are modified by triggers but "tend towards" their baseline values, and fleeing requires negative morale and fails if anger + morale > 0 with HP above 1/3. That combined condition stops flip-flopping between fight and flee. — [CDDA MONSTERS.md](https://docs.cataclysmdda.org/JSON/MONSTERS.html). A forum post reports anger decays slowly (about 10% chance per turn) — [CDDA forum](https://discourse.cataclysmdda.org/t/how-does-aggression-work/19608) (community, unverified)
- Cleanup and exit pitfall in BTs: a failing step in a Sequence skips later steps (the fridge door left open). — [BehaviorTree.CPP intro](https://www.behaviortree.dev/docs/learn-the-basics/BT_basics)
- Skyrim combat re-evaluation pitfall: a modder reports that a combat override package fails to hold because the NPC re-evaluates after casting a spell. Re-evaluation timing creates surprising overrides. — [Nexus forum: combat override not working](https://forums.nexusmods.com/topic/11945868-ai-packages-combat-override-not-working) (anecdotal)
- AI level of detail is a recognised topic: Ben Sunshine-Hill, "Phenomenal AI Level-of-Detail Control with the LOD Trader", Game AI Pro ch. 14. — [gameaipro.com PDF](http://www.gameaipro.com/GameAIPro/GameAIPro_Chapter14_Phenomenal_AI_Level-of-Detail_Control_with_the_LOD_Trader.pdf) (title only; content not extracted)

### Inferences
- **Leash oscillation (relevant now).** flycoos turns `angry` off when the player leaves `kLeashRadius` and back on when they re-enter within `aggroRange`. A player standing at the leash edge can toggle it every frame. Fixes, all cheap:
  1. Add a `Return` state that ignores the player until the NPC is back near home (optionally healing). This is the classic MMO "evade" pattern.
  2. Use hysteresis: re-aggro distance smaller than leash distance, plus a re-aggro cooldown of 2 to 3 seconds after returning.
  3. Leash on NPC distance from home, not only the player's distance, so a chase that drags the NPC far still ends.
- **Chase-without-path stall.** The current code makes the NPC wait if it isn't `walkable` (a `ponytail:` comment acknowledges this). Pair it with a "lost target for N seconds, then Return" transition, so stuck NPCs don't stand there forever.
- **Attack windups.** Model windup and recover as timed states (or substates of Attack). The attacker commits during windup (no turning, or reduced turning), which gives the player a dodge window and avoids per-frame re-decision jitter. Damage is applied once, on the windup's timeout transition.
- **Flee.** Gate it with a threshold plus a cooldown or chance (CDDA-like), and give it a fixed duration followed by Return or Chase, so it doesn't flicker at the HP threshold.
- **Tick cost at a few hundred NPCs:**
  - Decisions in an enum FSM cost almost nothing.
  - Run perception (LOS ray marches) at 2 to 5 Hz, staggered by `entityIndex % N`.
  - Beyond about 64 blocks from the player, skip AI and run only cheap wander or nothing.
  - Beyond chunk-load distance, freeze the NPC, or for scheduled villagers snap to the scheduled location when the player approaches (simulate the schedule by clock, not by walking). This is the main reason a package-list schedule (format D) beats a BT for off-screen villagers: "where should I be at hour H" is a lookup.
  - flycoos's single `std::vector<Entity>` loop is fine at this scale. Physics (`updatePlayer` per entity) is likely the larger cost. Measure with the existing benchmark flags before optimising.
- **Event inputs:** route `damaged`, `ally_attacked` (pack wolves, bandit camps) and `heard_noise` as events consumed by the FSM on the next tick (a small per-entity event bitmask). That avoids polling and makes "passive until hit" a one-line transition (`on: damaged`).

### Gaps
- Found no published per-NPC tick-cost numbers for FSM vs BT vs utility at the few-hundred scale. The tick-cost guidance above is reasoning, not measurement.
- Could not read the LOD Trader chapter or the Game AI Pro BT-pitfalls chapter for specific recommendations.
- No sourced material was found on the "evade/reset" leash pattern from shipped MMOs and ARPGs. It is presented as common practice, not cited.
