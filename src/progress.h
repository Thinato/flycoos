#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "stats.h"

// Progression (ADR-027): skills that improve with use, a character level from skill levels and
// kills, a talent point per level spent in one tree per skill. The numbers are data
// (data/skills.json, data/talents.json); the state is Progress on the Combatant, and its effects are
// modifiers that collect() adds to the stat pipeline, never applied by hand.

struct SkillDef {
    std::string name;
    float useMult = 0, useOffset = 0;            // XP per use: useMult x baseXp + useOffset
    float improveMult = 0, improveOffset = 1;    // XP to the next level: improveMult x level^1.95 + improveOffset
    std::vector<Modifier> perLevel;              // times the levels gained above kMinSkill
};

struct TalentDef {
    std::string id, name;
    Skill tree = Skill::OneHanded;
    int tier = 0;                // tier N needs kPointsPerTier x N points spent in its tree
    int maxRank = 1;
    int prerequisite = -1;       // a talent handle (same tree, lower tier) that must be at its max rank; -1: none
    std::vector<Modifier> perRank;  // times the rank
};
constexpr int kPointsPerTier = 3;
constexpr int kMaxTier = 4;

struct SkillLevel {
    int level = kMinSkill;
    float xp = 0;  // toward the next level
};
// The only progression state; the level itself is Combatant::level.
struct Progress {
    int xp = 0;  // character XP toward the next level
    std::array<SkillLevel, size_t(Skill::Count)> skills{};
    std::vector<uint8_t> talents;  // rank by talent handle (defs().talents); missing ones are rank 0
};

const SkillDef& skillDef(Skill s);  // from the loaded defs
int rank(const Progress& p, size_t talent);
int pointsSpent(const Progress& p, Skill tree = Skill::Count);  // Count: in every tree
// One point per level gained above 1, less those spent (negative after the debug window lowers the level).
inline int talentPoints(const Progress& p, int level) { return level - 1 - pointsSpent(p); }

// Adds character XP and levels up (1 talent point each) up to kMaxLevel. Returns the levels gained.
int addCharacterXp(Progress& p, int& level, int xp);
// Adds skill XP and levels the skill (each new skill level adds its number in character XP).
// Returns the skill levels gained.
int addSkillXp(Progress& p, int& level, Skill s, float xp);

// Whether `talent` can take another rank: below its max rank, its tree's points spent reach its
// tier, its requirement is at max rank, and a point to spend. `why` gets the first rule that fails.
bool canLearn(const Progress& p, int level, size_t talent, const char** why = nullptr);
bool learn(Progress& p, int level, size_t talent);  // canLearn, then one rank more
inline void respec(Progress& p) { p.talents.clear(); }
// Learns the ranks `p` holds again through canLearn, tier by tier (a load, a reload of the defs:
// the rules or the level may have changed). If any rank fails it respecs and returns false.
bool replayTalents(Progress& p, int level);

// The skills' per-level and the talents' per-rank modifiers, sourced by their names (collect()).
void progressModifiers(const Progress& p, std::vector<Modifier>& out);
