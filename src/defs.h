#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "entity.h"
#include "melee.h"
#include "progress.h"
#include "quest.h"

// Definitions loaded from data/ (ADR-018): one JSON file per type (quests: one folder, ADR-026;
// skills and talents, ADR-027: optional files; weapon classes, ADR-044: one record per WeaponClass),
// each an array of records keyed by a snake_case string id. Runtime code holds dense handles (an ItemId is the record's save_id),
// never strings. Move-only: the ItemType and EntityType names point into `itemNames` and `creatureNames`.
struct Defs {
    std::array<WeaponClassDef, size_t(WeaponClass::Count)> weaponClasses;  // by WeaponClass (ADR-044)
    std::vector<ItemType> items{ItemType{"", SlotNone, {0, 0, 0}, 0, 0, {}, {}}};  // by save id; 0 is NoItem
    std::vector<std::string> itemIds{""}, itemNames{""};                     // parallel to `items`
    std::map<std::string, ItemId, std::less<>> itemById;                     // string id -> save id, for references
    std::vector<EntityType> creatures;                     // by save id (EntityKind), from 0
    std::vector<std::string> creatureIds, creatureNames;   // parallel to `creatures`; "" name: a random villager name
    std::map<std::string, EntityKind, std::less<>> creatureById;
    std::vector<QuestDef> quests;          // by quest handle: data/quests/*.json in file name order, records in file order
    std::vector<std::string> questIds;     // parallel to `quests`
    std::map<std::string, uint16_t, std::less<>> questById;
    std::vector<QuestLine> questLines;     // every quest's lines, in that order (pickLine)
    std::array<SkillDef, size_t(Skill::Count)> skills;  // by Skill (ADR-027); without skills.json they never improve
    std::vector<TalentDef> talents;        // by talent handle: data/talents.json in file order
    std::map<std::string, uint16_t, std::less<>> talentById;
    std::vector<std::string> warnings;     // what loaded but looks wrong (a line that shadows a later one)

    Defs() = default;
    Defs(Defs&&) = default;
    Defs& operator=(Defs&&) = default;
};

// data/ in the working folder, else the source tree's (FLYCOOS_SOURCE_DIR).
std::filesystem::path dataDir();

// Parses every definition file in `dir` (pass 1), then resolves `copy_from` and checks every field
// and reference (pass 2). On failure returns false, leaves `out` alone, and `err` names the file,
// record and field.
bool loadDefs(const std::filesystem::path& dir, Defs& out, std::string& err);

// The registry itemType() and typeOf() read; empty (NoItem only, no creatures) until set. Swap it only between ticks.
const Defs& defs();
void setDefs(Defs&& d);
// Loads `dir` and swaps it in, unless it fails or would change what live ids mean: every loaded
// item and creature save id must keep its string id, every quest and talent handle that stays its quest
// or talent (Session::quests and Progress::talents are by handle; both may be added or dropped at the end). On a refusal the current defs stay and `err` says why.
bool reloadDefs(const std::filesystem::path& dir, std::string& err);
