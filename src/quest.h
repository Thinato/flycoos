#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "item.h"
#include "story.h"

struct Session;
struct Entity;
struct Defs;

// Quests, world flags and conditional dialogue (ADR-026). Quests are data (data/quests/*.json,
// loaded by loadDefs); their runtime state lives in Session. Systems tell quests what happened
// through one vector of GameEvents per tick, which updateQuests drains once at the end of step().

// What happened this tick, carried by value. Combat pushes Killed (the player's hit), talking (E)
// TalkedTo, region entry RegionEntered, and step() raises ItemCountChanged for every item whose count
// in the player's inventory changed since the last tick. RegionEntered is for the innermost region
// only (regionAt), when it changes: walking from the Vale into Ravensford enters Ravensford, back out
// enters the Vale again; an `entered` trigger on an outer region doesn't fire from inside an inner one.
struct Killed {
    EntityKind kind;
    uint32_t spawnMarkerId;  // 0: not from a marker
};
struct ItemCountChanged {
    ItemId item;
    int count;  // the count now
};
struct TalkedTo {
    uint32_t spawnMarkerId;
};
struct RegionEntered {
    uint32_t markerId;
};
using GameEvent = std::variant<Killed, ItemCountChanged, TalkedTo, RegionEntered>;

enum class Cmp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
inline constexpr const char* kCmpNames[] = {"==", "!=", "<", "<=", ">", ">="};
// Saved as a byte (QUST): append only.
enum class ObjectiveState : uint8_t { Hidden, Shown, Completed, Failed, Count };
inline constexpr const char* kObjectiveStateNames[] = {"hidden", "shown", "completed", "failed"};

// One test of quest, flag, inventory or world state; names and ids are resolved at load (quest,
// objective and item handles), marker names stay strings (the world can change under them).
struct Condition {
    enum Kind : uint8_t { Stage, Done, Failed, Objective, Flag, HasItem, InRegion, NpcDead } kind = Stage;
    bool negate = false;  // "not": true
    Cmp op = Cmp::Eq;     // stage, flag, has_item
    int value = 0;        // stage, flag, has_item: compared with; objective: an ObjectiveState
    uint16_t quest = 0;   // stage, done, failed, objective
    uint16_t objective = 0;
    ItemId item = NoItem;
    std::string name;  // flag; in_region: a region marker's name; npc_dead: a spawn marker's name
};

// The closed set of things a stage or trigger does.
struct Action {
    enum Kind : uint8_t { ShowObjective, CompleteObjective, FailObjective, GiveItem, TakeItem, SetFlag, SetStage, StartQuest,
                          CompleteQuest, FailQuest } kind = SetStage;
    uint16_t quest = 0;  // the quest it acts on (objectives and set_stage: its own)
    int value = 0;       // objective index, stage id or flag value
    ItemId item = NoItem;
    std::string flag;
};

// A stage listens for one kind of event, filtered by a spawn or region marker name, a creature or an item.
struct Trigger {
    enum Kind : uint8_t { TalkedTo, Killed, KilledCreature, Entered, HasItem } kind = TalkedTo;
    std::string name;  // talked_to, killed: a spawn marker's name; entered: a region marker's name
    EntityKind creature{};
    ItemId item = NoItem;
    int count = 1;  // has_item: at least this many
    std::vector<Condition> conditions;  // "if": all must pass too
    std::vector<Action> actions;
};

struct Stage {
    int id = 0;  // 0: not started yet (only its triggers count: what starts the quest)
    std::string journal;
    std::vector<Action> actions;  // run on entering it
    std::vector<Trigger> on;
};
struct Objective {
    std::string id, text;
};
struct QuestDef {
    std::string id, title;
    std::vector<Stage> stages;
    std::vector<Objective> objectives;
};
// A quest-owned dialogue line: the NPCs of the spawn marker named `spawn` say `text` while all
// `conditions` pass. The first passing line (quests in load order, lines in file order) wins; when
// none does, the marker's own LINE lines cycle as before.
struct QuestLine {
    std::string spawn, text;
    std::vector<Condition> conditions;
    uint16_t quest = 0;  // whose
};

// A quest's runtime state, by quest handle in Session::quests.
struct QuestState {
    int stage = 0;
    bool done = false, failed = false;
    std::vector<ObjectiveState> objectives;  // by objective index
    bool active() const { return stage != 0 && !done && !failed; }
};

constexpr int kMaxStageId = 65535;
constexpr size_t kMaxFlags = 4096;
constexpr size_t kEventLogSize = 32;

const Stage* findStage(const QuestDef& q, int id);
int countItem(const Inventory& inv, ItemId id);  // in the item slots, worn and on the bag bar

// Whether `c` (all of `cs`) holds in `s` now. A plain function: dialogue and triggers ask it directly.
bool passes(const Session& s, const Condition& c);
bool passesAll(const Session& s, const std::vector<Condition>& cs);
// The index into defs().questLines that `e`'s spawn marker says now, or -1 (its LINE lines then).
int pickLine(const Session& s, const Entity& e);

// Every quest back at stage 0 (not started), no flags; for startSession.
void resetQuests(Session& s);
// Back at stage 0, objectives hidden, neither done nor failed (the debug window).
void resetQuest(Session& s, size_t quest);
// Enters stage `id` of `quest`: runs its actions and re-checks its has_item triggers (as next
// tick's ItemCountChanged events). Unknown stages, the current one and done or failed quests are ignored.
void setQuestStage(Session& s, size_t quest, int id);
// Drains this tick's events into the quests' triggers; what that raises lands in the next batch.
// An item's ItemCountChanged counts once per batch (has_item reads the count now). Each event goes to
// the quests as they were before it (one another quest started on it hears the next). A trigger
// whose items wouldn't fit (stowItem) waits, so a reward is never lost. Records each event in
// Session::eventLog and quest news in Session::toasts.
void updateQuests(Session& s);

// Warnings for marker names quests refer to that don't name exactly one marker of their kind in
// `markers` (spawn names for talked_to, killed, npc_dead and lines; region names for entered and in_region).
std::vector<std::string> checkQuestMarkers(const Defs& d, const std::vector<Marker>& markers);
