#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>

// The five attributes. Every other stat derives from them (WoW-style, but classless, so there's
// no attack power or other in-between step).
struct Stats {
    int stamina = 10;   // max health
    int strength = 2;   // physical damage, by each weapon's scaling (ADR-048)
    int agility = 2;    // physical attack speed, and damage by each weapon's scaling
    int intellect = 2;  // damage by a weapon's scaling (none scale with it yet; magic later)
    int spirit = 2;     // health regeneration out of combat, and damage by a weapon's scaling
};

constexpr int kHealthPerStamina = 10;
constexpr float kHastePerAgility = 0.01f;  // +1% attack speed per point
constexpr float kRegenPerSpirit = 0.5f;    // health per second, out of combat only
constexpr float kCombatSeconds = 5;        // dealing or taking damage keeps you in combat this long
constexpr float kBaseCritChance = 0.05f;   // every hit's chance to be a critical one (ADR-045)...
constexpr float kBaseCritDamage = 0.5f;    // ...which deals this much more
constexpr float kBaseStatusResist = 100;   // the build-up a status takes to fill (ADR-046), before gear and creature mods
constexpr int kBaseFlasks = 3;             // healing flask charges (ADR-047)...
constexpr float kBaseFlaskHealing = 0.4f;  // ...each healing this share of max health

// How much of each attribute a weapon's hits add (ADR-048, Elden Ring's scaling): by kScalingStats,
// x the attribute per point (1 = a point adds a point of damage at speed 1).
using Scaling = std::array<float, 4>;
// A physical attack: damage range, base swing time in seconds (WoW's "weapon speed"), and scaling
// (by default strength only, a point a point: creatures' and fists').
struct Attack { int minDamage, maxDamage; float speed; Scaling scaling{1, 0, 0, 0}; };
constexpr Attack kPunch{1, 4, 1.0f};  // unarmed

// The stat pipeline (ADR-022): a body's final stats are its base attributes plus modifiers from
// its gear (and later talents and buffs), per stat: (base + sum of flat) x (1 + sum of increased) x
// product of (1 + more). The derived stats start from the final attributes, then take their own modifiers.
enum class Stat : uint8_t { Stamina, Strength, Agility, Intellect, Spirit,              // the five attributes
                            MaxHealth, Damage, Haste, Regen, Armor, DamageTaken,       // derived from them
                            CritChance, CritDamage,                                    // critical hits (ADR-045)
                            Bleed, Poison, Frost, BleedResist, PoisonResist, FrostResist,  // status effects (ADR-046)
                            Flasks, FlaskHealing,                                      // healing flasks (ADR-047)
                            Scaling,                                                   // x every weapon's scaling (ADR-048)
                            Count };
inline constexpr const char* kStatIds[] = {"stamina", "strength", "agility", "intellect", "spirit", "max_health",
                                           "damage", "haste", "regen", "armor", "damage_taken",
                                           "crit_chance", "crit_damage",
                                           "bleed", "poison", "frost", "bleed_resist", "poison_resist", "frost_resist",
                                           "flasks", "flask_healing", "scaling"};  // in data files
inline constexpr const char* kStatNames[] = {"Vitality", "Strength", "Agility", "Intellect", "Spirit", "Max health",
                                             "Damage", "Attack speed", "Regen", "Armor", "Damage taken",
                                             "Crit chance", "Crit damage",
                                             "Bleed build-up", "Poison build-up", "Frost build-up",
                                             "Bleed resist", "Poison resist", "Frost resist",
                                             "Flask charges", "Flask healing", "Attribute scaling"};
static_assert(std::size(kStatIds) == size_t(Stat::Count) && std::size(kStatNames) == size_t(Stat::Count));
// Stats whose values are fractions the UIs show as percents (a flat +0.05 crit chance is +5%).
constexpr bool percentStat(Stat s) {
    return s == Stat::Haste || s == Stat::CritChance || s == Stat::CritDamage || s == Stat::FlaskHealing;
}
// The status effects (ADR-046): hits build them up (the attacker's Bleed, Poison, Frost) toward the
// defender's resistance (BleedResist...); full, they take effect, and the build-up starts over.
enum class Status : uint8_t { Bleed, Poison, Frost, Count };
inline constexpr const char* kStatusIds[] = {"bleed", "poison", "frost"};
static_assert(std::size(kStatusIds) == size_t(Status::Count));
constexpr Stat buildupStat(Status s) { return Stat(int(Stat::Bleed) + int(s)); }
constexpr Stat resistStat(Status s) { return Stat(int(Stat::BleedResist) + int(s)); }
enum class ModKind : uint8_t { Flat, Increased, More };  // increased and more are fractions: 0.1 = 10%
// The use-based skills (ADR-027). Code awards their XP, so the set is closed; data/skills.json has
// their numbers. Saves name them by id.
enum class Skill : uint8_t { OneHanded, Unarmed, Armor, Count };
inline constexpr const char* kSkillIds[] = {"one_handed", "unarmed", "armor"};
static_assert(std::size(kSkillIds) == size_t(Skill::Count));
struct Modifier {
    Stat stat;
    ModKind kind;
    float value;
    const char* source = "";     // what it comes from (an item's, skill's or talent's name), for the UI
    Skill with = Skill::Count;   // a weapon skill: only on hits (and swings) made with it, never in the plain sheet
};

// Final stat values; consumers round (maxHealth, attributeBonus). Damage and DamageTaken multiply hits
// (1 = none), Haste adds attack speed (0.1 = 10% faster, -0.5 = half as fast), CritChance is a hit's
// chance to be critical (0.05 = 5%, 1 and over: every hit) and CritDamage what a critical hit adds (0.5 = x1.5).
// Bleed, Poison and Frost are the status build-up a hit adds, the resists the build-up that fills it.
// Flasks is how many healing flask charges a rest fills, FlaskHealing the share of max health each heals.
// Scaling multiplies what the attributes add to hits by the weapon's scaling (1 = as the weapon says).
struct StatSheet {
    std::array<float, size_t(Stat::Count)> v{};
    constexpr float operator[](Stat s) const { return v[size_t(s)]; }
};

constexpr float kMaxStat = 1e6f;    // every stat's ceiling, so int conversions can't overflow
constexpr float kMinHaste = -0.9f;  // haste may go negative (a slow), every other stat stops at 0
// Pure and uncached: rebuild it from the sources whenever it's needed. Values stay in
// 0..kMaxStat (haste kMinHaste..kMaxStat); a more factor below -100% counts as -100%. `with`: the
// weapon skill of the swing being resolved, whose conditional modifiers count too (Count: none).
constexpr StatSheet compute(const Stats& base, std::span<const Modifier> mods = {}, Skill with = Skill::Count) {
    std::array<float, size_t(Stat::Count)> flat{}, increased{}, more{};
    more.fill(1);
    for (const Modifier& m : mods) {
        if (m.with != Skill::Count && m.with != with) continue;
        size_t i = size_t(m.stat);
        if (m.kind == ModKind::Flat) flat[i] += m.value;
        else if (m.kind == ModKind::Increased) increased[i] += m.value;
        else more[i] *= std::max(0.0f, 1 + m.value);
    }
    StatSheet out;
    auto apply = [&](Stat s, float b) {
        size_t i = size_t(s);
        float lo = s == Stat::Haste ? kMinHaste : 0;  // slows down to 10x the swing time
        return out.v[i] = std::clamp((b + flat[i]) * std::max(0.0f, 1 + increased[i]) * more[i], lo, kMaxStat);
    };
    float stamina = apply(Stat::Stamina, float(base.stamina));
    apply(Stat::Strength, float(base.strength));
    float agility = apply(Stat::Agility, float(base.agility));
    apply(Stat::Intellect, float(base.intellect));
    float spirit = apply(Stat::Spirit, float(base.spirit));
    apply(Stat::MaxHealth, stamina * float(kHealthPerStamina));
    apply(Stat::Damage, 1);
    apply(Stat::Haste, agility * kHastePerAgility);
    apply(Stat::Regen, spirit * kRegenPerSpirit);
    apply(Stat::Armor, 0);  // gear adds it as flat modifiers
    apply(Stat::DamageTaken, 1);
    apply(Stat::CritChance, kBaseCritChance);
    apply(Stat::CritDamage, kBaseCritDamage);
    for (int s = 0; s < int(Status::Count); ++s) apply(buildupStat(Status(s)), 0), apply(resistStat(Status(s)), kBaseStatusResist);
    apply(Stat::Flasks, float(kBaseFlasks));
    apply(Stat::FlaskHealing, kBaseFlaskHealing);
    apply(Stat::Scaling, 1);
    return out;
}

constexpr int maxHealth(const StatSheet& s) { return std::max(1, int(s[Stat::MaxHealth] + 0.5f)); }
inline float healthRegen(const StatSheet& s) { return s[Stat::Regen]; }
constexpr int maxFlasks(const StatSheet& s) { return std::min(int(s[Stat::Flasks] + 0.5f), 99); }  // the HUD shows two digits (compute keeps it >= 0)
// Seconds between swings: haste (from agility) is attack speed.
inline float swingTime(const StatSheet& s, const Attack& a) { return a.speed / (1 + s[Stat::Haste]); }
// The attributes Attack::scaling is by (ADR-048).
inline constexpr Stat kScalingStats[] = {Stat::Strength, Stat::Agility, Stat::Intellect, Stat::Spirit};
static_assert(std::size(kScalingStats) == std::tuple_size_v<Scaling>);
// What attribute `i` (of kScalingStats) adds to a hit, unrounded: the attribute x the weapon's scaling
// with it x Scaling x speed. Speed is the attack's base speed, not the hasted swing time (as in WoW):
// slower attacks hit harder per swing, and haste adds damage per second only.
inline float attributeShare(const StatSheet& s, const Attack& a, size_t i) {
    return s[kScalingStats[i]] * a.scaling[i] * s[Stat::Scaling] * a.speed;
}
// damage = baseDamage + the attributes' shares, rounded.
inline int attributeBonus(const StatSheet& s, const Attack& a) {
    float sum = 0;
    for (size_t i = 0; i < std::size(kScalingStats); ++i) sum += attributeShare(s, a, i);
    return int(std::lround(sum));
}
// A scaling's letter in the UIs, Elden Ring's E (a little) to S (the most); '-': none.
constexpr char scalingGrade(float k) {
    return k <= 0 ? '-' : k < 0.25f ? 'E' : k < 0.5f ? 'D' : k < 0.8f ? 'C' : k < 1.1f ? 'B' : k < 1.4f ? 'A' : 'S';
}
inline int rollDamage(const StatSheet& s, const Attack& a, std::mt19937& rng) {
    return std::uniform_int_distribution<int>(a.minDamage, a.maxDamage)(rng) + attributeBonus(s, a);
}
// A roll's damage before the defender (attribute bonus and damage multiplier), rounded: what the UIs
// show. resolveHit keeps the fraction until after armor.
inline int hitBeforeArmor(const StatSheet& s, const Attack& a, int roll) {
    return int(std::lround(float(roll + attributeBonus(s, a)) * s[Stat::Damage]));
}
// The same of base attributes alone (creature types, tests).
constexpr int maxHealth(const Stats& s) { return maxHealth(compute(s)); }
inline float swingTime(const Stats& s, const Attack& a) { return swingTime(compute(s), a); }
inline int attributeBonus(const Stats& s, const Attack& a) { return attributeBonus(compute(s), a); }
inline int rollDamage(const Stats& s, const Attack& a, std::mt19937& rng) { return rollDamage(compute(s), a, rng); }

// Armor (from worn gear) absorbs a share of each hit: armor / (armor + K x attacker level), at most
// 80%. K is the armor that halves a level 1 attacker's damage; higher levels get through more of
// it. The debug UI tunes K live (Session::armorK).
constexpr float kArmorK = 40;
constexpr float kMaxArmorReduction = 0.8f;
inline float armorReduction(float armor, int attackerLevel, float k = kArmorK) {
    return armor > 0 ? std::min(kMaxArmorReduction, armor / (armor + k * float(attackerLevel))) : 0;
}
// A hit after armor, then times the defender's damage taken; never below 1, but a hit of 0 stays 0.
inline int mitigate(float damage, float armor, int attackerLevel, float k = kArmorK, float taken = 1) {
    if (damage <= 0) return 0;
    return std::max(1, int(std::lround(damage * (1 - armorReduction(armor, attackerLevel, k)) * taken)));
}

// Progression (ADR-027), Skyrim's three formulas with their numbers in data/skills.json:
// a use earns useMult x baseXp + useOffset skill XP (weapons: baseXp is the attack's average base
// damage, never the final hit); a skill goes from `level` to level + 1 for
// improveMult x level^1.95 + improveOffset; reaching skill level L adds L character XP; character
// level N -> N + 1 costs 25 x (N + 3), so reaching level N takes 12.5 N^2 + 62.5 N - 75 in all.
constexpr int kMinSkill = 1, kMaxSkill = 100;  // every skill starts at kMinSkill
constexpr int kMaxLevel = 100;                 // the character level cap (also data's creature levels)
constexpr float kSkillCurve = 1.95f;
constexpr float skillUseXp(float useMult, float useOffset, float baseXp) { return useMult * baseXp + useOffset; }
inline float skillXpToNext(float improveMult, float improveOffset, int level) {
    return improveMult * std::pow(float(level), kSkillCurve) + improveOffset;
}
constexpr int characterXpForSkill(int newLevel) { return newLevel; }
constexpr int levelXpToNext(int level) { return 25 * (level + 3); }
constexpr int levelXpTotal(int level) { return (25 * level * level + 125 * level - 150) / 2; }  // from level 1: 12.5 N^2 + 62.5 N - 75
// A kill's character XP when its creature record names none: hostile, attackable ones 10 per level.
constexpr int defaultKillXp(int level) { return 10 * level; }
