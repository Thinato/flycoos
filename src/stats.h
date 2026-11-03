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
    int strength = 2;   // physical damage
    int agility = 2;    // physical attack speed
    int intellect = 2;  // unused until magic
    int spirit = 2;     // health regeneration out of combat
};

constexpr int kHealthPerStamina = 10;
constexpr float kHastePerAgility = 0.01f;  // +1% attack speed per point
constexpr float kRegenPerSpirit = 0.5f;    // health per second, out of combat only
constexpr float kCombatSeconds = 5;        // dealing or taking damage keeps you in combat this long

// A physical attack: damage range, and base swing time in seconds (WoW's "weapon speed").
struct Attack { int minDamage, maxDamage; float speed; };
constexpr Attack kPunch{1, 4, 1.0f};  // unarmed

// The stat pipeline (ADR-022): a body's final stats are its base attributes plus modifiers from
// its gear (and later talents and buffs), per stat: (base + sum of flat) x (1 + sum of increased) x
// product of (1 + more). The derived stats start from the final attributes, then take their own modifiers.
enum class Stat : uint8_t { Stamina, Strength, Agility, Intellect, Spirit,              // the five attributes
                            MaxHealth, Damage, Haste, Regen, Armor, DamageTaken, Count };  // derived from them
inline constexpr const char* kStatIds[] = {"stamina", "strength", "agility", "intellect", "spirit", "max_health",
                                           "damage", "haste", "regen", "armor", "damage_taken"};  // in data files
inline constexpr const char* kStatNames[] = {"Stamina", "Strength", "Agility", "Intellect", "Spirit", "Max health",
                                             "Damage", "Attack speed", "Regen", "Armor", "Damage taken"};
static_assert(std::size(kStatIds) == size_t(Stat::Count) && std::size(kStatNames) == size_t(Stat::Count));
enum class ModKind : uint8_t { Flat, Increased, More };  // increased and more are fractions: 0.1 = 10%
struct Modifier {
    Stat stat;
    ModKind kind;
    float value;
    const char* source = "";  // what it comes from (an item's name), for the UI
};

// Final stat values; consumers round (maxHealth, strengthBonus). Damage and DamageTaken multiply hits
// (1 = none), Haste adds attack speed (0.1 = 10% faster, -0.5 = half as fast).
struct StatSheet {
    std::array<float, size_t(Stat::Count)> v{};
    constexpr float operator[](Stat s) const { return v[size_t(s)]; }
};

constexpr float kMaxStat = 1e6f;    // every stat's ceiling, so int conversions can't overflow
constexpr float kMinHaste = -0.9f;  // haste may go negative (a slow), every other stat stops at 0
// Pure and uncached: rebuild it from the sources whenever it's needed. Values stay in
// 0..kMaxStat (haste kMinHaste..kMaxStat); a more factor below -100% counts as -100%.
constexpr StatSheet compute(const Stats& base, std::span<const Modifier> mods = {}) {
    std::array<float, size_t(Stat::Count)> flat{}, increased{}, more{};
    more.fill(1);
    for (const Modifier& m : mods) {
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
    return out;
}

constexpr int maxHealth(const StatSheet& s) { return std::max(1, int(s[Stat::MaxHealth] + 0.5f)); }
inline float healthRegen(const StatSheet& s) { return s[Stat::Regen]; }
// Seconds between swings: haste (from agility) is attack speed.
inline float swingTime(const StatSheet& s, const Attack& a) { return a.speed / (1 + s[Stat::Haste]); }
// damage = baseDamage + strength * speed. Speed is the attack's base speed, not the hasted swing
// time (as in WoW): slower attacks hit harder per swing, and agility adds damage per second only.
inline int strengthBonus(const StatSheet& s, const Attack& a) { return int(std::lround(s[Stat::Strength] * a.speed)); }
inline int rollDamage(const StatSheet& s, const Attack& a, std::mt19937& rng) {
    return std::uniform_int_distribution<int>(a.minDamage, a.maxDamage)(rng) + strengthBonus(s, a);
}
// A roll's damage before the defender (strength bonus and damage multiplier), rounded: what the UIs
// show. resolveHit keeps the fraction until after armor.
inline int hitBeforeArmor(const StatSheet& s, const Attack& a, int roll) {
    return int(std::lround(float(roll + strengthBonus(s, a)) * s[Stat::Damage]));
}
// The same of base attributes alone (creature types, tests).
constexpr int maxHealth(const Stats& s) { return maxHealth(compute(s)); }
inline float swingTime(const Stats& s, const Attack& a) { return swingTime(compute(s), a); }
inline int strengthBonus(const Stats& s, const Attack& a) { return strengthBonus(compute(s), a); }
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
