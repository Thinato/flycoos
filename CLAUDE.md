# flycoos: working notes for Claude

flycoos is a C++23 voxel action RPG (SDL3 GPU on Metal, a deterministic 60 Hz sim, soulslike melee). This file is
how we work on it: the rules, the gates every commit passes, and how changes are shown to the user (screenshots,
films and a review page with the patch notes) before they're committed.

**Where things are:**

| Doc | What's in it |
|---|---|
| `README.md` | Building, running, controls |
| `docs/ARCHITECTURE.md` | One row per source file: what lives where |
| `docs/DECISIONS.md` | The ADRs, numbered, append-only (the newest is the highest number) |
| `docs/COMBAT_ROADMAP.md`, `docs/WORLD_ROADMAP.md`, `docs/ROADMAP.md` | Done and to do, with the open questions |
| `docs/ART.md` | Art rules: one colour per voxel, gradients never noise, flat models. Two written exceptions: the white blade smear and the hit flashes |
| `docs/TALENTS.md` | The talent plan. Its foundations rules apply to all combat work now, though the talents themselves come last |
| `docs/VISION.md` | What the game is, and the IP guardrail |

## Ground rules

- **Commit and push only when the user asks.** Commit messages end with
  `Co-Authored-By: Claude <noreply@anthropic.com>`. No CI.
- **Never touch the user's running game.** It serves MCP on port 47800: every run you start uses `--no-serve`.
- **Never write to `worlds/`**, `worlds/story.flyw` above all. Copy a world into `build/review/` or the scratchpad and
  run the copy (a run with `--world X` writes a save next to X).
- **MCP world builders never undo or redo.** Leave the untracked `.karazhan/` alone.
- **Ids are append-only:** item, creature and save ids are never renumbered or reused.
- **IP guardrail:** borrow mechanics only. No other game's names, characters, art or assets, in code or in docs.
- **Sounds:** free licences only (CC0 and similar), credited in `CREDITS.md`.
- **Ask only genuine taste questions** (how it should look or feel). Decide everything else and say what you decided.
- **Deferred on purpose:** the controls and accessibility refactor, and the talents (last, after all combat features).
- The story world is deprecated: test combat in the arena (`worlds/arena.flyw`); play is the procedural world.

## Build and gates

```sh
cmake --preset release && cmake --build --preset release   # build/release
cmake --preset dev && cmake --build --preset dev           # build/dev: Debug + ASan/UBSan (fatal)
```

Every commit passes all of these. `tools/review/gates.sh` runs them all; it takes minutes, so run it in the
background (the `run_in_background` option, never a trailing `&`):

1. 0 warnings in build/release and build/dev.
2. `core_test` passes in both (`all checks passed`). The dev run is the ASan/UBSan run.
3. `python3 tools/test_mcp.py` passes (`all e2e checks passed`).

Pass looks like:

```
REL-BUILD-DONE exit 0 warnings 0
all checks passed
DEV-BUILD-DONE exit 0 warnings 0
all checks passed
all e2e checks passed
GATES-EXIT
```

The editor's LSP errors are false (there's no compile_commands.json at the root). Trust the CMake build.

## How a change goes in

1. **Design first** for anything bigger than a fix: recon, a short design, and the taste questions. Wait for the answers.
2. **Build it with an ADR** in `docs/DECISIONS.md` (Status, Context, Decision, Alternatives considered). Also update
   `docs/ARCHITECTURE.md`'s rows, the roadmap ticks and `README.md` when the controls or the debug window change.
3. **A debug UI for every new system:** a tab or node in the debug window (`.` key; `src/debug_ui.cpp`) to see its
   state live, tune its numbers and trigger it.
4. **Tests in `tests/core_test.cpp`** for every rule. Bound every wait loop (`for (int i = 0; i < 600 && ...; ++i)`):
   an unbounded `while` hangs the mutation check.
5. **A mutation check** (below) proves the new tests catch broken rules.
6. **A perf A/B** (below) for any rendering change.
7. **A review page** (below) for any change you can see: look, motion, AI behaviour, the HUD. The user approves
   the page BEFORE the commit.
8. **An independent code review** of the uncommitted diff, by a separate reviewer agent (oh-my-claudecode:code-reviewer)
   that sees the code, not your conclusions. It has found real bugs more than once. Fix what it finds and rerun the gates.
9. **Commit** when the user approves and asks.

## Screenshots

A benchmark run (`--frames N`) renders N frames and exits. Its last frame can be saved:

```sh
./build/release/flycoos --world <copy>.flyw --no-serve --frames 300 --look 0 -20 --time 0.42 --screenshot shot.bmp
sips -s format png -Z 1200 shot.bmp --out shot.png     # then Read shot.png to look at it
```

- **`--look YAW PITCH`** is in degrees: yaw 0 looks at -Z and -90 at +X; a negative pitch looks up. Top-down shots
  for positional behaviour use `--look 0 75`.
- **Other flags:** `--time 0..1` (time of day), `--seed N`, `--debug` (the debug window is open and in the shot),
  `--view shaded|quads|chunks|ao|normals|wire`.
- Without `--look`, the benchmark player walks a circle and the camera orbits. For a scripted scene (a fight, a
  pose, an open window), use the film tools below; `still.sh` gives one frame.
- **For look changes** (colours, blocks, lighting), shoot the same fixed views with the old and the new build and put
  them side by side on a review page.

## Review films: `tools/review/`

The game has no recorder built in. The tools build a **copy** of the source with a scene hook patched in
(`scene_hook.py`), film scripted scenes in a copy of the arena, and turn the frames into GIFs and strips. Everything
goes into `build/review/RIG/` (gitignored, survives restarts). RIG is a short name for the work, like `bosses`.

| Tool | What it does |
|---|---|
| `tree.sh RIG WHO SOURCE` | A hooked release build in `build/review/RIG/WHO`. SOURCE is a git revision for the "before" (usually `HEAD`), or a directory for the "after" (`.` or an agent's worktree). Rerun it after edits |
| `shoot.sh RIG WHO NAME FRAMES [FLY_...]` | Films one scene into `shots/WHO+NAME/fNNNN.bmp` |
| `clip.sh RIG NAME FROM TO CROP SFROM STO [STEP]` | `page/NAME.gif` (real speed), `NAME-slow.gif` (4x slow) and `NAME-strip.jpg` |
| `still.sh RIG SHOT FRAME CROP NAME` | One frame as `page/NAME.jpg`, for close-ups |
| `perf.sh RIG RUNS FRAMES [FLY_...]` | The perf A/B: before and after, alternating |
| `mutate.py RIG MUTANTS.py` | The mutation check |
| `gates.sh` | Every commit gate |
| `page.html` | The review page template |

**How the hook works:**
- It only acts in a benchmark run with `FLY_SCENE=1`. At frame 30 it sets the scene up from `FLY_*` variables.
- Then it saves every second frame, numbered from the scene's start. 60 frames is 1 second, and the numbers are even.
- A box at the top left shows the scene clock, each NPC's AI state, its reason and health, and the player's state.
- **All the variables are listed at the top of `scene_hook.py`:** where you stand and what you hold, who spawns where,
  scripted attacks, guards, heavies and parries, god mode, calm NPCs, a fixed camera (`FLY_EYE`, `FLY_AT`).
  Positions are blocks from the arena's middle.
- **A new scene needs a knob the hook lacks?** Add it to `scene_hook.py` (in the repo, so the next film has it).
- **The hook stops with "anchor not found"?** `main.cpp` changed under it: fix the anchor text in `scene_hook.py`.
- Blocks that need a newer feature (effects, bosses, grabs) are left out on older trees, so "before" trees build too.

**A typical session:**

```sh
tools/review/tree.sh bosses before HEAD        # the last commit
tools/review/tree.sh bosses after .            # the working tree (or an agent's worktree path)
S=(FLY_PLAYER="0 0 -32" FLY_NODUMMIES=1 FLY_GOD=1 FLY_ANGRY=1 FLY_EYE="11 44.5 -30" FLY_AT="0 41.5 -30.5")
for who in before after; do
  tools/review/shoot.sh bosses $who A 600 $S FLY_SPAWN="barrow_lord 1 0 -29"
done
# Look at a few frames (sips to png, Read) to pick the crop and the moment worth slowing down, then:
for who in before after; do tools/review/clip.sh bosses ${who}A 0 598 1000x563+300+180 0 150 14; done
tools/review/still.sh bosses afterA 120 1000x563+300+180 still-roar
cp tools/review/page.html build/review/bosses/page/index.html   # fill it in
open build/review/bosses/page/index.html
```

**Tips:**
- **Same scene, same moves, both builds.** The sim is deterministic, so a before/after pair differs only by the change.
- Crop both sides of a pair the same. Shots are 1920x1080.
- **Pick the slow window** around the moment that matters, about 1 to 3 seconds of the scene.
- **Strips:** 8 to 14 frames with a step that spans the slow window.
- **Check the AI box in the frames.** A scene that didn't go as scripted (the enemy wandered off, you died) films
  nothing useful. Fix the script and reshoot.
- **Scenes take time:** a 600-frame scene takes about 30 s to shoot. Several scenes for both builds are worth a
  background run.
- **The game reads `data/` from its working folder** (else from the source tree it was built from). `shoot.sh` and
  `perf.sh` run inside the tree's folder, so a "before" build gets its own data.
- **BMPs are 6 MB each:** one scene is several GB. Delete `shots/` once the clips are made. When the work is
  committed, move `page/` to `build/review/archive/adrNNN-name/`, then delete `build/review/RIG`.
- **Past review pages** (ADR-042 onward) are in `build/review/archive/`. They're a good reference for scenes,
  crops and wording.

## Review pages

The user judges visible changes on a local HTML page opened with `open`, and approves it before the commit.
Start from `tools/review/page.html`:

- **Intro:** what was filmed, where, which commit "before" is, and anything scripted (you can't die, the enemy
  can't swing back).
- **What changes: the patch notes.** One bullet per thing the player will notice, in plain words, with the numbers
  that matter (seconds, blocks, damage). Then what the debug window gained.
- **One section per scene:** a short paragraph, then before/after side by side: the real-speed GIFs, then the 4x
  slow GIFs, then the strips.
- **Close-ups:** stills of details a GIF goes past too fast.
- **Performance:** for rendering changes, the table of every run.
- **Questions:** only taste questions, each with the current choice and the alternatives.

Write it for the user, not for a reviewer of code: what they will see and feel. They read GIFs at real speed and
4x slow, and strips; keep making all three.

## Perf A/B

Any rendering change gets one.

- **Scene:** the heaviest scene the change touches (a crowd fight, the cap full), filmed with `perf.sh` (no frames
  written). Use at least 5 runs each, alternating before and after.
- **The machine is a fanless MacBook Air:** frame times drift with heat, and about one run in six hitches or pins to
  120 Hz. Drop runs with a hitch or a mean over 6 ms from both sides.
- **Report every run**, the averages and p99. Make sure the "before" binary really is the old commit.
- **Apple GPU lesson:** extra vertex-to-fragment varyings cost about 15%. Pack them into the existing ones.

## Mutation check

It proves the new tests catch broken rules. Write `build/review/RIG/mutants.py` with
`M = [(path, old_text, new_text, name), ...]`: each mutant flips one comparison, drops one clause or line, or
changes one data value that the change added.

```sh
python3 tools/review/mutate.py RIG build/review/RIG/mutants.py --dry   # every old_text found once?
python3 tools/review/mutate.py RIG build/review/RIG/mutants.py --workers 3 --worker 0   # also 1 and 2, in the background
```

- **Each mutant gets a verdict:**
  - `caught` (core_test failed, good)
  - `SURVIVED` (add a test, or explain in the report why the mutant can't change behaviour)
  - `BUILD FAIL` (proves nothing: rewrite it)
- **Aim for about 50 to 70 mutants** for a feature. Report the score, for example "71 of 72 caught; the survivor is
  equivalent because...".
- Kill only processes you started, by PID. `pkill -f mutate.py` once killed another agent's run.

## Supervisor mode: features done by worktree agents

For big features, the main session supervises background agents, so its own context lasts longer. Run several
agents in parallel when the work splits cleanly.

1. **Spawn** a general-purpose agent with `isolation: "worktree"`, in the background. Its brief holds the goal,
   what's already decided, the files to read, and a pointer to this file.
   - **Worktree builds reuse the main checkout's downloads:**
     `cmake --preset release -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_BASE_DIR=<main>/build/release/_deps`
     (same for dev).
   - Its review rig goes in the main checkout's `build/review/RIG/` (absolute path), so it survives the worktree.
2. **Phase 1:** the agent does recon and writes a design (under ~700 words) plus taste questions. It writes no code.
   The main session asks the user (AskUserQuestion) and sends the answers back (SendMessage).
3. **Phase 2:** the agent builds the change and runs every step of "How a change goes in", including its own
   independent code review. It makes the review page, then stops and reports.
   - **Agents never commit, push or ask the user**, and never touch the main checkout's source.
   - **An agent's worktree with no changes yet can be auto-removed** while it waits on phase-1 answers. The agent
     recreates it from the same commit.
4. **Integration:** the main session shows the user the page. Once approved:
   - In the worktree: `git add -A && git diff --cached <base> > patch`.
   - In main: `git apply --index patch` (use `--3way` for a second patch on top of the first).
   - Merge the usual conflicts: ADR order in DECISIONS.md, the field lists in defs.cpp, ARCHITECTURE rows.
   - Run `gates.sh`, then commit and push when asked.
   - Remove the worktree, its `worktree-agent-*` branch and the rig.

Each agent uses about 450k to 650k tokens and takes about 1.5 to 2 hours.

## Gotchas

- **The shell is zsh:** `$var` doesn't word-split. Use arrays (`S=(A=1 B=2); cmd $S`) or `${=var}`.
- **AppleClang 17:** no `std::jthread`; use `std::thread` and `join`.
- **Shaders load at run time** from `build/<preset>/shaders/`.
- **The arena fight tests in core_test are chaotic** (enemies answer by chance). Any combat-number change can move
  them: check the bands, and tune with a small grid search over the data, not by hand.
- **Tests that block need the guard up more than 0.15 s first**, or the hit counts as a perfect guard (ADR-041).
- **Noises** (`Combatant::noises`) build up in tests; check `made()` with `|=`.
